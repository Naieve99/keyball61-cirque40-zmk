/*
 * Independent Keyball half layer synchronization over connectionless BLE.
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(keyball_peer_layer_sync, CONFIG_ZMK_LOG_LEVEL);

#define PEER_PROTOCOL_VERSION 1U
#define PEER_ADV_INTERVAL_UNITS 160U /* 100 ms; Bluetooth units are 0.625 ms. */
#define PEER_SCAN_INTERVAL_UNITS 96U /* 60 ms. */
#define PEER_SCAN_WINDOW_UNITS 32U   /* 20 ms (33% scan duty cycle). */

/* Private 128-bit service UUID, serialized in Bluetooth little-endian order. */
static const uint8_t peer_service_uuid[16] = {
    0x53, 0x4c, 0x42, 0x4b, 0x4d, 0x9b, 0xc1, 0xa8,
    0x6f, 0x4f, 0xa5, 0x3a, 0x01, 0x00, 0xe1, 0xb7,
};

struct peer_packet {
    uint8_t uuid[16];
    uint8_t version;
    uint8_t family;
    uint8_t group_le[2];
    uint8_t side;
    uint8_t sequence;
    uint8_t layers_le[4];
    uint8_t crc_le[2];
} __packed;

BUILD_ASSERT(sizeof(struct peer_packet) == 28,
             "Peer packet must fit in a 31-byte advertising PDU");
BUILD_ASSERT(ZMK_KEYMAP_LAYERS_LEN <= 32,
             "Peer layer protocol supports at most 32 keymap layers");

static struct bt_le_ext_adv *peer_adv;
static struct peer_packet tx_packet;
static struct bt_data tx_ad = BT_DATA(BT_DATA_SVC_DATA128, &tx_packet, sizeof(tx_packet));

static atomic_t local_layers;
static atomic_t remote_layers;
static atomic_t applying_remote;
static uint8_t tx_sequence;

static void apply_layers_work_handler(struct k_work *work);
static void advertise_work_handler(struct k_work *work);
static void peer_timeout_work_handler(struct k_work *work);

K_WORK_DEFINE(apply_layers_work, apply_layers_work_handler);
K_WORK_DEFINE(advertise_work, advertise_work_handler);
K_WORK_DELAYABLE_DEFINE(peer_timeout_work, peer_timeout_work_handler);

static uint16_t peer_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xffffU;

    while (len-- > 0U) {
        crc ^= (uint16_t)(*data++) << 8;
        for (uint8_t bit = 0U; bit < 8U; bit++) {
            crc = (crc & 0x8000U) != 0U ? (uint16_t)((crc << 1) ^ 0x1021U)
                                              : (uint16_t)(crc << 1);
        }
    }

    return crc;
}

static void prepare_tx_packet(void) {
    memcpy(tx_packet.uuid, peer_service_uuid, sizeof(peer_service_uuid));
    tx_packet.version = PEER_PROTOCOL_VERSION;
    tx_packet.family = CONFIG_KEYBALL_PEER_FAMILY;
    sys_put_le16(CONFIG_KEYBALL_PEER_GROUP_ID, tx_packet.group_le);
    tx_packet.side = CONFIG_KEYBALL_PEER_SIDE;
    tx_packet.sequence = tx_sequence++;
    sys_put_le32((uint32_t)atomic_get(&local_layers), tx_packet.layers_le);
    sys_put_le16(peer_crc16((const uint8_t *)&tx_packet,
                            offsetof(struct peer_packet, crc_le)),
                 tx_packet.crc_le);
}

static void advertise_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (peer_adv == NULL) {
        return;
    }

    prepare_tx_packet();
    int err = bt_le_ext_adv_set_data(peer_adv, &tx_ad, 1, NULL, 0);
    if (err != 0) {
        LOG_WRN("Failed to update peer layer advertisement (%d)", err);
    }
}

static void apply_layers_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    uint32_t desired = (uint32_t)atomic_get(&local_layers) |
                       (uint32_t)atomic_get(&remote_layers);
    uint32_t current = zmk_keymap_layer_state();

    atomic_set(&applying_remote, 1);

    for (uint8_t layer = 0U; layer < ZMK_KEYMAP_LAYERS_LEN; layer++) {
        uint32_t mask = BIT(layer);
        bool should_be_active = (desired & mask) != 0U;
        bool is_active = (current & mask) != 0U;

        if (should_be_active == is_active) {
            continue;
        }

        int err = should_be_active ? zmk_keymap_layer_activate(layer, false)
                                   : zmk_keymap_layer_deactivate(layer, false);
        if (err != 0) {
            LOG_WRN("Failed to apply peer layer %u=%u (%d)", layer,
                    should_be_active, err);
        }
    }

    atomic_clear(&applying_remote);
}

static void peer_timeout_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (atomic_set(&remote_layers, 0) != 0) {
        LOG_INF("Peer timed out; removed remote layer state");
        k_work_submit(&apply_layers_work);
    }
}

static bool parse_peer_ad(struct bt_data *data, void *user_data) {
    ARG_UNUSED(user_data);

    if (data->type != BT_DATA_SVC_DATA128 || data->data_len != sizeof(struct peer_packet)) {
        return true;
    }

    struct peer_packet packet;
    memcpy(&packet, data->data, sizeof(packet));

    if (memcmp(packet.uuid, peer_service_uuid, sizeof(peer_service_uuid)) != 0 ||
        packet.version != PEER_PROTOCOL_VERSION ||
        packet.family != CONFIG_KEYBALL_PEER_FAMILY ||
        sys_get_le16(packet.group_le) != CONFIG_KEYBALL_PEER_GROUP_ID ||
        packet.side == CONFIG_KEYBALL_PEER_SIDE) {
        return true;
    }

    uint16_t expected_crc = peer_crc16((const uint8_t *)&packet,
                                       offsetof(struct peer_packet, crc_le));
    if (sys_get_le16(packet.crc_le) != expected_crc) {
        LOG_WRN("Ignored peer packet with invalid CRC");
        return false;
    }

    uint32_t received_layers = sys_get_le32(packet.layers_le);
    atomic_set(&remote_layers, (atomic_val_t)received_layers);
    k_work_submit(&apply_layers_work);
    k_work_reschedule(&peer_timeout_work, K_MSEC(CONFIG_KEYBALL_PEER_TIMEOUT_MS));

    return false;
}

static void peer_scan_received(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                               struct net_buf_simple *ad) {
    ARG_UNUSED(addr);
    ARG_UNUSED(rssi);
    ARG_UNUSED(type);

    bt_data_parse(ad, parse_peer_ad, NULL);
}

static int peer_layer_sync_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *event = as_zmk_layer_state_changed(eh);

    if (event == NULL || atomic_get(&applying_remote) != 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    atomic_val_t old_layers;
    atomic_val_t new_layers;
    atomic_val_t mask = (atomic_val_t)BIT(event->layer);

    do {
        old_layers = atomic_get(&local_layers);
        new_layers = event->state ? (old_layers | mask) : (old_layers & ~mask);
    } while (!atomic_cas(&local_layers, old_layers, new_layers));

    k_work_submit(&advertise_work);
    k_work_submit(&apply_layers_work);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(keyball_peer_layer_sync, peer_layer_sync_listener);
ZMK_SUBSCRIPTION(keyball_peer_layer_sync, zmk_layer_state_changed);

static int peer_layer_sync_init(void) {
    static const struct bt_le_adv_param adv_params =
        BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_EXT_ADV, PEER_ADV_INTERVAL_UNITS,
                             PEER_ADV_INTERVAL_UNITS, NULL);
    static const struct bt_le_scan_param scan_params =
        BT_LE_SCAN_PARAM_INIT(BT_LE_SCAN_TYPE_PASSIVE, BT_LE_SCAN_OPT_NONE,
                              PEER_SCAN_INTERVAL_UNITS, PEER_SCAN_WINDOW_UNITS);

    atomic_set(&local_layers, (atomic_val_t)zmk_keymap_layer_state());
    atomic_clear(&remote_layers);
    atomic_clear(&applying_remote);
    prepare_tx_packet();

    int err = bt_le_ext_adv_create(&adv_params, NULL, &peer_adv);
    if (err != 0) {
        LOG_ERR("Failed to create peer advertising set (%d)", err);
        return err;
    }

    err = bt_le_ext_adv_set_data(peer_adv, &tx_ad, 1, NULL, 0);
    if (err != 0) {
        LOG_ERR("Failed to set peer advertising data (%d)", err);
        return err;
    }

    err = bt_le_ext_adv_start(peer_adv, BT_LE_EXT_ADV_START_DEFAULT);
    if (err != 0) {
        LOG_ERR("Failed to start peer advertising (%d)", err);
        return err;
    }

    err = bt_le_scan_start(&scan_params, peer_scan_received);
    if (err != 0 && err != -EALREADY) {
        LOG_ERR("Failed to start peer scanning (%d)", err);
        return err;
    }

    LOG_INF("Peer layer sync active: family=%u group=0x%04x side=%u",
            CONFIG_KEYBALL_PEER_FAMILY, CONFIG_KEYBALL_PEER_GROUP_ID,
            CONFIG_KEYBALL_PEER_SIDE);
    return 0;
}

SYS_INIT(peer_layer_sync_init, APPLICATION, CONFIG_KEYBALL_PEER_INIT_PRIORITY);

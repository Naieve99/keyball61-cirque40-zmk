/*
 * Independent Keyball half enrollment and layer synchronization.
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keyball_behavior_peer_enroll

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(keyball_peer_layer_sync, CONFIG_ZMK_LOG_LEVEL);

#define PEER_PROTOCOL_VERSION 2U
#define PEER_BOND_VERSION 1U
#define PEER_ADV_INTERVAL_UNITS 160U /* 100 ms; Bluetooth units are 0.625 ms. */
#define PEER_SCAN_INTERVAL_UNITS 96U /* 60 ms. */
#define PEER_SCAN_WINDOW_UNITS 32U   /* 20 ms (33% scan duty cycle). */
#define PEER_CONFIRM_DURATION_MS 2000U
#define PEER_SETTINGS_KEY "keyball_peer/bond"

/* Private 16-bit UUID. Bytes are serialized in Bluetooth little-endian order. */
#define PEER_SERVICE_UUID 0xB7E1U

enum peer_message_type {
    PEER_MESSAGE_IDLE = 0,
    PEER_MESSAGE_ENROLL_REQUEST = 1,
    PEER_MESSAGE_ENROLL_OFFER = 2,
    PEER_MESSAGE_ENROLL_ACK = 3,
    PEER_MESSAGE_ENROLL_CONFIRM = 4,
    PEER_MESSAGE_LAYER_STATE = 5,
};

struct peer_packet {
    uint8_t uuid_le[2];
    uint8_t magic[2];
    uint8_t version;
    uint8_t family;
    uint8_t side;
    uint8_t message_type;
    uint8_t sequence;
    uint8_t reserved;
    uint8_t source_id_le[4];
    uint8_t target_id_le[4];
    uint8_t pairing_id_le[4];
    uint8_t layers_le[4];
    uint8_t crc_le[2];
} __packed;

struct peer_bond_record {
    uint8_t version;
    uint8_t family;
    uint8_t local_side;
    uint8_t reserved;
    uint8_t local_id_le[4];
    uint8_t peer_id_le[4];
    uint8_t pairing_id_le[4];
    uint8_t crc_le[2];
} __packed;

BUILD_ASSERT(sizeof(struct peer_packet) == 28,
             "Peer packet must fit in a 31-byte advertising PDU");
BUILD_ASSERT(ZMK_KEYMAP_LAYERS_LEN <= 32,
             "Peer layer protocol supports at most 32 keymap layers");

static struct bt_le_ext_adv *peer_adv;
static struct peer_packet tx_packet;
static struct bt_data tx_ad = BT_DATA(BT_DATA_SVC_DATA16, &tx_packet, sizeof(tx_packet));

static atomic_t local_layers;
static atomic_t remote_layers;
static atomic_t applying_remote;
static uint8_t tx_sequence;

static uint32_t local_device_id;
static uint32_t bonded_peer_id;
static uint32_t bonded_pairing_id;
static bool bond_valid;

static bool enrollment_active;
static bool enrollment_confirming;
static uint32_t pending_peer_id;
static uint32_t pending_pairing_id;

static void apply_layers_work_handler(struct k_work *work);
static void advertise_work_handler(struct k_work *work);
static void peer_timeout_work_handler(struct k_work *work);
static void enrollment_timeout_work_handler(struct k_work *work);
static void enrollment_confirm_work_handler(struct k_work *work);
static void enrollment_hold_work_handler(struct k_work *work);

K_WORK_DEFINE(apply_layers_work, apply_layers_work_handler);
K_WORK_DEFINE(advertise_work, advertise_work_handler);
K_WORK_DELAYABLE_DEFINE(peer_timeout_work, peer_timeout_work_handler);
K_WORK_DELAYABLE_DEFINE(enrollment_timeout_work, enrollment_timeout_work_handler);
K_WORK_DELAYABLE_DEFINE(enrollment_confirm_work, enrollment_confirm_work_handler);
K_WORK_DELAYABLE_DEFINE(enrollment_hold_work, enrollment_hold_work_handler);

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

static uint32_t hash_bytes(const uint8_t *data, size_t len) {
    uint32_t hash = 2166136261U;

    while (len-- > 0U) {
        hash ^= *data++;
        hash *= 16777619U;
    }

    return hash == 0U ? 1U : hash;
}

static int ensure_local_device_id(void) {
    uint8_t hardware_id[16] = {0};

    if (local_device_id != 0U) {
        return 0;
    }

    ssize_t hardware_id_len = hwinfo_get_device_id(hardware_id, sizeof(hardware_id));
    if (hardware_id_len <= 0) {
        return hardware_id_len < 0 ? (int)hardware_id_len : -ENODEV;
    }

    local_device_id = hash_bytes(hardware_id, (size_t)hardware_id_len);
    return 0;
}

static uint32_t derive_pairing_id(uint32_t left_id, uint32_t right_id) {
    uint8_t data[11] = {'K', 'B', CONFIG_KEYBALL_PEER_FAMILY, 0U, 0U, 0U,
                        0U,  0U,  0U, 0U, 0U};

    sys_put_le32(left_id, &data[3]);
    sys_put_le32(right_id, &data[7]);
    return hash_bytes(data, sizeof(data));
}

static enum peer_message_type current_message_type(void) {
    if (enrollment_confirming) {
        return PEER_MESSAGE_ENROLL_CONFIRM;
    }

    if (enrollment_active) {
        if (pending_peer_id == 0U) {
            return PEER_MESSAGE_ENROLL_REQUEST;
        }

        return CONFIG_KEYBALL_PEER_SIDE == 0 ? PEER_MESSAGE_ENROLL_OFFER
                                             : PEER_MESSAGE_ENROLL_ACK;
    }

    return bond_valid ? PEER_MESSAGE_LAYER_STATE : PEER_MESSAGE_IDLE;
}

static void prepare_tx_packet(void) {
    enum peer_message_type message_type = current_message_type();
    uint32_t target_id = 0U;
    uint32_t pairing_id = 0U;

    if (message_type == PEER_MESSAGE_LAYER_STATE) {
        target_id = bonded_peer_id;
        pairing_id = bonded_pairing_id;
    } else if (message_type == PEER_MESSAGE_ENROLL_OFFER ||
               message_type == PEER_MESSAGE_ENROLL_ACK ||
               message_type == PEER_MESSAGE_ENROLL_CONFIRM) {
        target_id = pending_peer_id;
        pairing_id = pending_pairing_id;
    }

    memset(&tx_packet, 0, sizeof(tx_packet));
    sys_put_le16(PEER_SERVICE_UUID, tx_packet.uuid_le);
    tx_packet.magic[0] = 'K';
    tx_packet.magic[1] = 'P';
    tx_packet.version = PEER_PROTOCOL_VERSION;
    tx_packet.family = CONFIG_KEYBALL_PEER_FAMILY;
    tx_packet.side = CONFIG_KEYBALL_PEER_SIDE;
    tx_packet.message_type = message_type;
    tx_packet.sequence = tx_sequence++;
    sys_put_le32(local_device_id, tx_packet.source_id_le);
    sys_put_le32(target_id, tx_packet.target_id_le);
    sys_put_le32(pairing_id, tx_packet.pairing_id_le);
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
        LOG_WRN("Failed to update peer advertisement (%d)", err);
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

static void clear_remote_layers(void) {
    (void)k_work_cancel_delayable(&peer_timeout_work);
    if (atomic_set(&remote_layers, 0) != 0) {
        k_work_submit(&apply_layers_work);
    }
}

static int peer_bond_settings_set(const char *name, size_t len,
                                  settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    struct peer_bond_record record;

    if (!settings_name_steq(name, "bond", &next) || next != NULL) {
        return -ENOENT;
    }

    if (len != sizeof(record)) {
        LOG_WRN("Ignoring peer bond with unexpected length %u", (unsigned int)len);
        return -EINVAL;
    }

    int rc = ensure_local_device_id();
    if (rc != 0) {
        LOG_ERR("Cannot read controller identity while loading peer bond (%d)", rc);
        return rc;
    }

    rc = read_cb(cb_arg, &record, sizeof(record));
    if (rc < 0) {
        return rc;
    }

    uint16_t expected_crc = peer_crc16((const uint8_t *)&record,
                                       offsetof(struct peer_bond_record, crc_le));
    if (record.version != PEER_BOND_VERSION ||
        record.family != CONFIG_KEYBALL_PEER_FAMILY ||
        record.local_side != CONFIG_KEYBALL_PEER_SIDE ||
        sys_get_le32(record.local_id_le) != local_device_id ||
        sys_get_le16(record.crc_le) != expected_crc) {
        LOG_WRN("Ignoring invalid or foreign peer bond");
        return -EINVAL;
    }

    bonded_peer_id = sys_get_le32(record.peer_id_le);
    bonded_pairing_id = sys_get_le32(record.pairing_id_le);
    bond_valid = bonded_peer_id != 0U && bonded_pairing_id != 0U;
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(keyball_peer, "keyball_peer", NULL,
                               peer_bond_settings_set, NULL, NULL);

static int save_bond(void) {
    struct peer_bond_record record = {
        .version = PEER_BOND_VERSION,
        .family = CONFIG_KEYBALL_PEER_FAMILY,
        .local_side = CONFIG_KEYBALL_PEER_SIDE,
    };

    sys_put_le32(local_device_id, record.local_id_le);
    sys_put_le32(bonded_peer_id, record.peer_id_le);
    sys_put_le32(bonded_pairing_id, record.pairing_id_le);
    sys_put_le16(peer_crc16((const uint8_t *)&record,
                            offsetof(struct peer_bond_record, crc_le)),
                 record.crc_le);

    return settings_save_one(PEER_SETTINGS_KEY, &record, sizeof(record));
}

static void accept_bond(uint32_t peer_id, uint32_t pairing_id) {
    bonded_peer_id = peer_id;
    bonded_pairing_id = pairing_id;
    bond_valid = true;
    enrollment_active = false;
    (void)k_work_cancel_delayable(&enrollment_timeout_work);

    int err = save_bond();
    if (err != 0) {
        LOG_ERR("Failed to save peer enrollment (%d)", err);
    } else {
        LOG_INF("Peer enrollment saved");
    }
}

static void start_enrollment(void) {
    int err = settings_delete(PEER_SETTINGS_KEY);
    if (err != 0 && err != -ENOENT) {
        LOG_WRN("Failed to erase old peer enrollment (%d)", err);
    }

    bond_valid = false;
    bonded_peer_id = 0U;
    bonded_pairing_id = 0U;
    pending_peer_id = 0U;
    pending_pairing_id = 0U;
    enrollment_confirming = false;
    enrollment_active = true;
    clear_remote_layers();

    k_work_reschedule(&enrollment_timeout_work,
                      K_MSEC(CONFIG_KEYBALL_PEER_ENROLL_TIMEOUT_MS));
    k_work_submit(&advertise_work);
    LOG_INF("Peer enrollment open for %u seconds",
            CONFIG_KEYBALL_PEER_ENROLL_TIMEOUT_MS / 1000U);
}

static void enrollment_timeout_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    enrollment_active = false;
    pending_peer_id = 0U;
    pending_pairing_id = 0U;
    k_work_submit(&advertise_work);
    LOG_INF("Peer enrollment window closed");
}

static void enrollment_confirm_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    enrollment_confirming = false;
    pending_peer_id = 0U;
    pending_pairing_id = 0U;
    k_work_submit(&advertise_work);
}

static bool packet_header_valid(const struct peer_packet *packet) {
    uint16_t expected_crc = peer_crc16((const uint8_t *)packet,
                                       offsetof(struct peer_packet, crc_le));

    return sys_get_le16(packet->uuid_le) == PEER_SERVICE_UUID &&
           packet->magic[0] == 'K' && packet->magic[1] == 'P' &&
           packet->version == PEER_PROTOCOL_VERSION &&
           packet->family == CONFIG_KEYBALL_PEER_FAMILY &&
           packet->side != CONFIG_KEYBALL_PEER_SIDE &&
           sys_get_le16(packet->crc_le) == expected_crc;
}

static void handle_enrollment_packet(const struct peer_packet *packet, int8_t rssi) {
    if (!enrollment_active || rssi < CONFIG_KEYBALL_PEER_ENROLL_MIN_RSSI) {
        return;
    }

    uint32_t source_id = sys_get_le32(packet->source_id_le);
    uint32_t target_id = sys_get_le32(packet->target_id_le);
    uint32_t pairing_id = sys_get_le32(packet->pairing_id_le);

    if (source_id == 0U || source_id == local_device_id) {
        return;
    }

    if (CONFIG_KEYBALL_PEER_SIDE == 0 &&
        packet->message_type == PEER_MESSAGE_ENROLL_REQUEST &&
        pending_peer_id == 0U) {
        pending_peer_id = source_id;
        pending_pairing_id = derive_pairing_id(local_device_id, source_id);
        k_work_submit(&advertise_work);
        LOG_INF("Found right half; offering enrollment");
        return;
    }

    if (CONFIG_KEYBALL_PEER_SIDE == 1 &&
        packet->message_type == PEER_MESSAGE_ENROLL_OFFER &&
        target_id == local_device_id) {
        uint32_t expected = derive_pairing_id(source_id, local_device_id);
        if (pairing_id == expected) {
            pending_peer_id = source_id;
            pending_pairing_id = expected;
            k_work_submit(&advertise_work);
            LOG_INF("Received left-half enrollment offer");
        }
        return;
    }

    if (CONFIG_KEYBALL_PEER_SIDE == 0 &&
        packet->message_type == PEER_MESSAGE_ENROLL_ACK &&
        source_id == pending_peer_id && target_id == local_device_id &&
        pairing_id == pending_pairing_id) {
        accept_bond(source_id, pairing_id);
        enrollment_confirming = true;
        k_work_reschedule(&enrollment_confirm_work,
                          K_MSEC(PEER_CONFIRM_DURATION_MS));
        k_work_submit(&advertise_work);
        return;
    }

    if (CONFIG_KEYBALL_PEER_SIDE == 1 &&
        packet->message_type == PEER_MESSAGE_ENROLL_CONFIRM &&
        source_id == pending_peer_id && target_id == local_device_id &&
        pairing_id == pending_pairing_id) {
        accept_bond(source_id, pairing_id);
        pending_peer_id = 0U;
        pending_pairing_id = 0U;
        k_work_submit(&advertise_work);
    }
}

struct peer_parse_context {
    int8_t rssi;
};

static bool parse_peer_ad(struct bt_data *data, void *user_data) {
    struct peer_parse_context *context = user_data;

    if (data->type != BT_DATA_SVC_DATA16 || data->data_len != sizeof(struct peer_packet)) {
        return true;
    }

    struct peer_packet packet;
    memcpy(&packet, data->data, sizeof(packet));

    if (!packet_header_valid(&packet)) {
        return true;
    }

    if (packet.message_type >= PEER_MESSAGE_ENROLL_REQUEST &&
        packet.message_type <= PEER_MESSAGE_ENROLL_CONFIRM) {
        handle_enrollment_packet(&packet, context->rssi);
        return false;
    }

    if (packet.message_type != PEER_MESSAGE_LAYER_STATE || !bond_valid ||
        sys_get_le32(packet.source_id_le) != bonded_peer_id ||
        sys_get_le32(packet.target_id_le) != local_device_id ||
        sys_get_le32(packet.pairing_id_le) != bonded_pairing_id) {
        return true;
    }

    atomic_set(&remote_layers, (atomic_val_t)sys_get_le32(packet.layers_le));
    k_work_submit(&apply_layers_work);
    k_work_reschedule(&peer_timeout_work, K_MSEC(CONFIG_KEYBALL_PEER_TIMEOUT_MS));
    return false;
}

static void peer_scan_received(const struct bt_le_scan_recv_info *info,
                               struct net_buf_simple *ad) {
    struct peer_parse_context context = {.rssi = info->rssi};
    bt_data_parse(ad, parse_peer_ad, &context);
}

static struct bt_le_scan_cb peer_scan_callbacks = {
    .recv = peer_scan_received,
};

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

static void enrollment_hold_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    start_enrollment();
}

static int peer_enroll_pressed(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    k_work_reschedule(&enrollment_hold_work,
                      K_MSEC(CONFIG_KEYBALL_PEER_ENROLL_HOLD_MS));
    return ZMK_BEHAVIOR_OPAQUE;
}

static int peer_enroll_released(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    (void)k_work_cancel_delayable(&enrollment_hold_work);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api peer_enroll_driver_api = {
    .binding_pressed = peer_enroll_pressed,
    .binding_released = peer_enroll_released,
    .locality = BEHAVIOR_LOCALITY_EVENT_SOURCE,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define PEER_ENROLL_INST(n)                                                                        \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                               \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &peer_enroll_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PEER_ENROLL_INST)

static int peer_layer_sync_init(void) {
    static const struct bt_le_adv_param adv_params =
        BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_EXT_ADV, PEER_ADV_INTERVAL_UNITS,
                             PEER_ADV_INTERVAL_UNITS, NULL);
    static const struct bt_le_scan_param scan_params =
        BT_LE_SCAN_PARAM_INIT(BT_LE_SCAN_TYPE_PASSIVE, BT_LE_SCAN_OPT_NONE,
                              PEER_SCAN_INTERVAL_UNITS, PEER_SCAN_WINDOW_UNITS);
    int err = ensure_local_device_id();
    if (err != 0) {
        LOG_ERR("Cannot read controller hardware identity (%d)", err);
        return err;
    }

    atomic_set(&local_layers, (atomic_val_t)zmk_keymap_layer_state());
    atomic_clear(&remote_layers);
    atomic_clear(&applying_remote);
    prepare_tx_packet();

    err = bt_le_ext_adv_create(&adv_params, NULL, &peer_adv);
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

    err = bt_le_scan_cb_register(&peer_scan_callbacks);
    if (err != 0 && err != -EEXIST) {
        LOG_ERR("Failed to register peer scanner (%d)", err);
        return err;
    }

    err = bt_le_scan_start(&scan_params, NULL);
    if (err != 0 && err != -EALREADY) {
        LOG_ERR("Failed to start peer scanning (%d)", err);
        return err;
    }

    k_work_submit(&advertise_work);
    LOG_INF("Peer sync active: family=%u side=%u enrolled=%u",
            CONFIG_KEYBALL_PEER_FAMILY, CONFIG_KEYBALL_PEER_SIDE, bond_valid);
    return 0;
}

SYS_INIT(peer_layer_sync_init, APPLICATION, CONFIG_KEYBALL_PEER_INIT_PRIORITY);

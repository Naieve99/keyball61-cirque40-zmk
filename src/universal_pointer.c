/*
 * Fail-safe startup selection for the shared Keyball pointer connector.
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keyball_pointer_autodetect

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(keyball_universal_pointer, CONFIG_ZMK_LOG_LEVEL);

#if DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) != 1
#error "Universal pointer firmware requires exactly one keyball,pointer-autodetect node"
#endif

#define DETECTOR_NODE DT_DRV_INST(0)
#define CIRQUE_NODE DT_PHANDLE(DETECTOR_NODE, cirque)
#define PMW3360_NODE DT_PHANDLE(DETECTOR_NODE, pmw3360)
#define CIRQUE_BUS_NODE DT_BUS(CIRQUE_NODE)
#define PMW3360_BUS_NODE DT_BUS(PMW3360_NODE)

static const struct gpio_dt_spec id_gpio = GPIO_DT_SPEC_GET(DETECTOR_NODE, id_gpios);
static const struct device *const cirque_bus = DEVICE_DT_GET(CIRQUE_BUS_NODE);
static const struct device *const cirque = DEVICE_DT_GET(CIRQUE_NODE);
static const struct device *const pmw3360_bus = DEVICE_DT_GET(PMW3360_BUS_NODE);
static const struct device *const pmw3360 = DEVICE_DT_GET(PMW3360_NODE);

enum keyball_pointer_identity {
    KEYBALL_POINTER_NONE,
    KEYBALL_POINTER_CIRQUE,
    KEYBALL_POINTER_PMW3360,
    KEYBALL_POINTER_FAULT,
};

static int read_identity_with_pull(gpio_flags_t pull, int *value) {
    int err = gpio_pin_configure(id_gpio.port, id_gpio.pin, GPIO_INPUT | pull);
    if (err != 0) {
        return err;
    }

    k_busy_wait(1000);
    *value = gpio_pin_get_raw(id_gpio.port, id_gpio.pin);
    return *value < 0 ? *value : 0;
}

static enum keyball_pointer_identity detect_identity(void) {
    int with_pull_up;
    int with_pull_down;
    int err = read_identity_with_pull(GPIO_PULL_UP, &with_pull_up);

    if (err == 0) {
        err = read_identity_with_pull(GPIO_PULL_DOWN, &with_pull_down);
    }

    (void)gpio_pin_configure(id_gpio.port, id_gpio.pin, GPIO_INPUT);

    if (err != 0) {
        LOG_ERR("Cannot read pointer identity pin (%d)", err);
        return KEYBALL_POINTER_FAULT;
    }

    if (with_pull_up == 0 && with_pull_down == 0) {
        return KEYBALL_POINTER_CIRQUE;
    }

    if (with_pull_up == 1 && with_pull_down == 1) {
        return KEYBALL_POINTER_PMW3360;
    }

    if (with_pull_up == 1 && with_pull_down == 0) {
        return KEYBALL_POINTER_NONE;
    }

    return KEYBALL_POINTER_FAULT;
}

static int initialize_selected_device(const struct device *bus,
                                      const struct device *pointer,
                                      const char *name) {
    int err = device_init(bus);
    if (err != 0) {
        LOG_ERR("Failed to initialize %s bus (%d); keyboard remains available", name, err);
        return 0;
    }

    err = device_init(pointer);
    if (err != 0) {
        LOG_ERR("Failed to initialize %s (%d); keyboard remains available", name, err);
        return 0;
    }

    LOG_INF("Detected and enabled %s", name);
    return 0;
}

static int universal_pointer_init(void) {
    if (!device_is_ready(id_gpio.port)) {
        LOG_ERR("Pointer identity GPIO controller is not ready");
        return 0;
    }

    switch (detect_identity()) {
    case KEYBALL_POINTER_CIRQUE:
        return initialize_selected_device(cirque_bus, cirque, "Cirque I2C pointer");
    case KEYBALL_POINTER_PMW3360:
        return initialize_selected_device(pmw3360_bus, pmw3360, "PMW3360 SPI pointer");
    case KEYBALL_POINTER_NONE:
        LOG_INF("No pointer module detected; keyboard-only mode active");
        return 0;
    case KEYBALL_POINTER_FAULT:
    default:
        LOG_ERR("Invalid pointer identity; shared pins remain inputs");
        return 0;
    }
}

SYS_INIT(universal_pointer_init, APPLICATION, CONFIG_KEYBALL_POINTER_DETECT_INIT_PRIORITY);

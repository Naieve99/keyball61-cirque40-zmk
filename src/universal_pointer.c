/*
 * Runtime protocol detection for the shared Keyball pointer connector.
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keyball_pointer_autodetect

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
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

#define CIRQUE_I2C_ADDRESS DT_REG_ADDR(CIRQUE_NODE)
#define CIRQUE_READ_REGISTER_MASK 0xA0
#define CIRQUE_FIRMWARE_ID_REGISTER 0x00
#define CIRQUE_FIRMWARE_ID 0x07

#define PMW3360_PRODUCT_ID_REGISTER 0x00
#define PMW3360_PRODUCT_ID 0x42
#define PMW3360_SPI_READ 0x00
#define PMW3360_SPI_T_SRAD_US 160
#define PMW3360_SPI_T_SCLK_NCS_READ_US 120
#define PMW3360_SPI_OPERATION                                                                    \
    (SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_TRANSFER_MSB | \
     SPI_HOLD_ON_CS | SPI_LOCK_ON)

#define SOFT_I2C_HALF_PERIOD_US 5
#define SOFT_I2C_CLOCK_HIGH_TIMEOUT_US 100
#define CIRQUE_PROBE_ATTEMPTS 3
#define PMW3360_PROBE_ATTEMPTS 3

static const struct gpio_dt_spec scl_gpio = GPIO_DT_SPEC_GET(DETECTOR_NODE, scl_gpios);
static const struct gpio_dt_spec sda_gpio = GPIO_DT_SPEC_GET(DETECTOR_NODE, sda_gpios);
static const struct gpio_dt_spec mosi_dr_gpio =
    GPIO_DT_SPEC_GET(DETECTOR_NODE, mosi_dr_gpios);
static const struct gpio_dt_spec cs_gpio = GPIO_DT_SPEC_GET(DETECTOR_NODE, cs_gpios);

static const struct device *const cirque_bus = DEVICE_DT_GET(CIRQUE_BUS_NODE);
static const struct device *const cirque = DEVICE_DT_GET(CIRQUE_NODE);
static const struct device *const pmw3360_bus = DEVICE_DT_GET(PMW3360_BUS_NODE);
static const struct device *const pmw3360 = DEVICE_DT_GET(PMW3360_NODE);
static const struct spi_dt_spec pmw3360_spi =
    SPI_DT_SPEC_GET(PMW3360_NODE, PMW3360_SPI_OPERATION, 0);

static void soft_i2c_delay(void) { k_busy_wait(SOFT_I2C_HALF_PERIOD_US); }

/* Releasing rather than driving a logic high preserves I2C open-drain behavior. */
static int release_line(const struct gpio_dt_spec *gpio) {
    return gpio_pin_configure_dt(gpio, GPIO_INPUT | GPIO_PULL_UP);
}

static int drive_line_low(const struct gpio_dt_spec *gpio) {
    return gpio_pin_configure_dt(gpio, GPIO_OUTPUT_LOW);
}

static int wait_line_high(const struct gpio_dt_spec *gpio) {
    for (int elapsed = 0; elapsed < SOFT_I2C_CLOCK_HIGH_TIMEOUT_US; elapsed++) {
        int value = gpio_pin_get_raw(gpio->port, gpio->pin);

        if (value < 0) {
            return value;
        }
        if (value != 0) {
            return 0;
        }
        k_busy_wait(1);
    }

    return -ETIMEDOUT;
}

static int soft_i2c_raise_clock(void) {
    int err = release_line(&scl_gpio);

    if (err != 0) {
        return err;
    }

    return wait_line_high(&scl_gpio);
}

static int soft_i2c_start(void) {
    int err = release_line(&sda_gpio);

    if (err == 0) {
        err = soft_i2c_raise_clock();
    }
    if (err == 0) {
        err = wait_line_high(&sda_gpio);
    }
    if (err != 0) {
        return err;
    }

    soft_i2c_delay();
    err = drive_line_low(&sda_gpio);
    soft_i2c_delay();
    if (err == 0) {
        err = drive_line_low(&scl_gpio);
    }
    soft_i2c_delay();
    return err;
}

static void soft_i2c_stop(void) {
    (void)drive_line_low(&sda_gpio);
    soft_i2c_delay();
    (void)soft_i2c_raise_clock();
    soft_i2c_delay();
    (void)release_line(&sda_gpio);
    soft_i2c_delay();
}

static int soft_i2c_clock_bit(bool bit) {
    int err = bit ? release_line(&sda_gpio) : drive_line_low(&sda_gpio);

    soft_i2c_delay();
    if (err == 0) {
        err = soft_i2c_raise_clock();
    }
    soft_i2c_delay();
    if (err == 0) {
        err = drive_line_low(&scl_gpio);
    }
    soft_i2c_delay();
    return err;
}

static int soft_i2c_write_byte(uint8_t value) {
    for (uint8_t mask = BIT(7); mask != 0; mask >>= 1) {
        int err = soft_i2c_clock_bit((value & mask) != 0);

        if (err != 0) {
            return err;
        }
    }

    int err = release_line(&sda_gpio);

    soft_i2c_delay();
    if (err == 0) {
        err = soft_i2c_raise_clock();
    }
    soft_i2c_delay();
    if (err == 0 && gpio_pin_get_raw(sda_gpio.port, sda_gpio.pin) != 0) {
        err = -ENXIO;
    }
    (void)drive_line_low(&scl_gpio);
    soft_i2c_delay();
    return err;
}

static int soft_i2c_read_byte(uint8_t *value) {
    int err = release_line(&sda_gpio);
    uint8_t result = 0;

    for (int bit = 7; err == 0 && bit >= 0; bit--) {
        soft_i2c_delay();
        err = soft_i2c_raise_clock();
        soft_i2c_delay();
        if (err == 0) {
            int sample = gpio_pin_get_raw(sda_gpio.port, sda_gpio.pin);

            if (sample < 0) {
                err = sample;
            } else if (sample != 0) {
                result |= BIT(bit);
            }
        }
        (void)drive_line_low(&scl_gpio);
        soft_i2c_delay();
    }

    /* This is the only byte in the read, so finish it with a NACK. */
    if (err == 0) {
        err = soft_i2c_clock_bit(true);
    }
    if (err == 0) {
        *value = result;
    }
    return err;
}

static int read_cirque_firmware_id(uint8_t *firmware_id) {
    int err = soft_i2c_start();

    if (err == 0) {
        err = soft_i2c_write_byte((CIRQUE_I2C_ADDRESS << 1) | 0);
    }
    if (err == 0) {
        err = soft_i2c_write_byte(CIRQUE_READ_REGISTER_MASK | CIRQUE_FIRMWARE_ID_REGISTER);
    }
    if (err == 0) {
        err = soft_i2c_start();
    }
    if (err == 0) {
        err = soft_i2c_write_byte((CIRQUE_I2C_ADDRESS << 1) | 1);
    }
    if (err == 0) {
        err = soft_i2c_read_byte(firmware_id);
    }

    soft_i2c_stop();
    return err;
}

static bool detect_cirque(void) {
    for (int attempt = 0; attempt < CIRQUE_PROBE_ATTEMPTS; attempt++) {
        uint8_t firmware_id = 0;
        int err = read_cirque_firmware_id(&firmware_id);

        if (err == 0 && firmware_id == CIRQUE_FIRMWARE_ID) {
            LOG_INF("Cirque FirmwareId 0x%02x found at I2C address 0x%02x", firmware_id,
                    CIRQUE_I2C_ADDRESS);
            return true;
        }

        k_msleep(5);
    }

    return false;
}

static int read_pmw3360_product_id(uint8_t *product_id) {
    uint8_t address = PMW3360_SPI_READ | PMW3360_PRODUCT_ID_REGISTER;
    const struct spi_buf tx_buffer = {.buf = &address, .len = sizeof(address)};
    const struct spi_buf_set tx = {.buffers = &tx_buffer, .count = 1};
    struct spi_buf rx_buffer = {.buf = product_id, .len = sizeof(*product_id)};
    const struct spi_buf_set rx = {.buffers = &rx_buffer, .count = 1};
    int err = spi_write_dt(&pmw3360_spi, &tx);

    if (err == 0) {
        k_usleep(PMW3360_SPI_T_SRAD_US);
        err = spi_read_dt(&pmw3360_spi, &rx);
    }
    k_usleep(PMW3360_SPI_T_SCLK_NCS_READ_US);
    (void)spi_release_dt(&pmw3360_spi);
    return err;
}

static bool detect_pmw3360(void) {
    for (int attempt = 0; attempt < PMW3360_PROBE_ATTEMPTS; attempt++) {
        uint8_t product_id = 0;
        int err = read_pmw3360_product_id(&product_id);

        if (err == 0 && product_id == PMW3360_PRODUCT_ID) {
            LOG_INF("PMW3360 Product_ID 0x%02x found over SPI", product_id);
            return true;
        }

        k_msleep(5);
    }

    return false;
}

static bool detector_gpios_are_ready(void) {
    return device_is_ready(scl_gpio.port) && device_is_ready(sda_gpio.port) &&
           device_is_ready(mosi_dr_gpio.port) && device_is_ready(cs_gpio.port);
}

static int prepare_read_only_cirque_probe(void) {
    /* Keep PMW3360 deselected and never drive its MOSI / Cirque HW_DR pin. */
    int err = gpio_pin_configure_dt(&cs_gpio, GPIO_OUTPUT_HIGH);

    if (err == 0) {
        err = gpio_pin_configure_dt(&mosi_dr_gpio, GPIO_INPUT);
    }
    if (err == 0) {
        err = release_line(&scl_gpio);
    }
    if (err == 0) {
        err = release_line(&sda_gpio);
    }

    return err;
}

static void leave_shared_pins_as_inputs(void) {
    (void)gpio_pin_configure_dt(&scl_gpio, GPIO_INPUT);
    (void)gpio_pin_configure_dt(&sda_gpio, GPIO_INPUT);
    (void)gpio_pin_configure_dt(&mosi_dr_gpio, GPIO_INPUT);
    (void)gpio_pin_configure_dt(&cs_gpio, GPIO_INPUT);
}

static int initialize_pointer(const struct device *pointer, const char *name) {
    int err = device_init(pointer);

    if (err != 0) {
        LOG_ERR("Failed to initialize %s (%d); keyboard remains available", name, err);
        return 0;
    }

    LOG_INF("Detected and enabled %s", name);
    return 0;
}

static int universal_pointer_init(void) {
    if (!detector_gpios_are_ready()) {
        LOG_ERR("Pointer detector GPIO controller is not ready");
        return 0;
    }

    int err = prepare_read_only_cirque_probe();

    if (err != 0) {
        LOG_ERR("Cannot prepare pointer probe (%d); shared pins remain inputs", err);
        leave_shared_pins_as_inputs();
        return 0;
    }

    if (detect_cirque()) {
        err = device_init(cirque_bus);
        if (err != 0) {
            LOG_ERR("Failed to initialize Cirque I2C bus (%d); keyboard remains available", err);
            leave_shared_pins_as_inputs();
            return 0;
        }
        return initialize_pointer(cirque, "Cirque I2C pointer");
    }

    /*
     * No valid Cirque FirmwareId was returned. SPI probing necessarily drives
     * the shared MOSI/HW_DR wire, so this no-strap design cannot be fail-safe
     * against a present but broken or miswired Cirque module.
     */
    err = device_init(pmw3360_bus);
    if (err != 0) {
        LOG_ERR("Failed to initialize PMW3360 SPI bus (%d); keyboard remains available", err);
        leave_shared_pins_as_inputs();
        return 0;
    }

    if (detect_pmw3360()) {
        return initialize_pointer(pmw3360, "PMW3360 SPI pointer");
    }

    leave_shared_pins_as_inputs();
    LOG_INF("No recognized pointer module detected; keyboard-only mode active");
    return 0;
}

SYS_INIT(universal_pointer_init, APPLICATION, CONFIG_KEYBALL_POINTER_DETECT_INIT_PRIORITY);

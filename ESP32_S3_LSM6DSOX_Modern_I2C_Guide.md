# Modern ESP32-S3 LSM6DSOX Data Dump Guide

Detta dokument åtgärdar de två felen du stötte på: uppdelningen av `ESP_LOGI`-strängen samt migreringen till det moderna `driver/i2c_master.h` som krävs i moderna versioner av ESP-IDF.

## 1. Problemet förklarat
1. **Kompileringsfelet**: En C-sträng i ett makro som `ESP_LOGI` kan inte bara radbrytas mitt i utan ett avslutande citationstecken på samma rad. Det förvirrar kompilatorn och ger felet `missing terminating " character`.
2. **I2C EOL Varningen**: Ditt nuvarande ESP-IDF använder det gamla I2C-biblioteket (`driver/i2c.h`) vilket är markerat som End-Of-Life. Från och med ESP-IDF v6.0 tas det bort helt. Vi har här migrerat koden till det nya, stabila `driver/i2c_master.h`.

## 2. Komplett Källkod (`main.c`)

```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "LSM6DSOX_APP";

#define I2C_MASTER_SDA_IO           11
#define I2C_MASTER_SCL_IO           12
#define LSM6DSOX_I2C_ADDRESS        0x6A

#define LSM6DSOX_WHO_AM_I_REG       0x0F
#define LSM6DSOX_CTRL1_XL           0x10
#define LSM6DSOX_CTRL2_G            0x11
#define LSM6DSOX_OUTX_L_G           0x22

i2c_master_dev_handle_t dev_handle;

void i2c_master_init(void) {
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LSM6DSOX_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle));
}

uint8_t lsm6dsox_read_reg(uint8_t reg) {
    uint8_t data = 0;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev_handle, &reg, 1, &data, 1, -1));
    return data;
}

void lsm6dsox_write_reg(uint8_t reg, uint8_t data) {
    uint8_t write_buf[2] = {reg, data};
    ESP_ERROR_CHECK(i2c_master_transmit(dev_handle, write_buf, 2, -1));
}

void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(500));
    i2c_master_init();

    uint8_t who_am_i = lsm6dsox_read_reg(LSM6DSOX_WHO_AM_I_REG);
    if (who_am_i != 0x6C) {
        ESP_LOGE(TAG, "Felaktigt WHO_AM_I: 0x%02X (Förväntade 0x6C)", who_am_i);
        return;
    }
    ESP_LOGI(TAG, "LSM6DSOX hittad! ID: 0x%02X", who_am_i);

    lsm6dsox_write_reg(LSM6DSOX_CTRL1_XL, 0x40);
    lsm6dsox_write_reg(LSM6DSOX_CTRL2_G, 0x40);

    while (1) {
        uint8_t raw[12];
        uint8_t reg_start = LSM6DSOX_OUTX_L_G;
        ESP_ERROR_CHECK(i2c_master_transmit_receive(dev_handle, &reg_start, 1, raw, 12, -1));

        int16_t gyro_x = (int16_t)((raw[1] << 8) | raw[0]);
        int16_t gyro_y = (int16_t)((raw[3] << 8) | raw[2]);
        int16_t gyro_z = (int16_t)((raw[5] << 8) | raw[4]);

        int16_t accel_x = (int16_t)((raw[7] << 8) | raw[6]);
        int16_t accel_y = (int16_t)((raw[9] << 8) | raw[8]);
        int16_t accel_z = (int16_t)((raw[11] << 8) | raw[10]);

        float ax = (accel_x * 0.061) / 1000.0;
        float ay = (accel_y * 0.061) / 1000.0;
        float az = (accel_z * 0.061) / 1000.0;

        float gx = gyro_x * 0.00875;
        float gy = gyro_y * 0.00875;
        float gz = gyro_z * 0.00875;

        ESP_LOGI(TAG, "ACCEL [g] | X: %5.2f Y: %5.2f Z: %5.2f || GYRO [dps] | X: %5.1f Y: %5.1f Z: %5.1f", ax, ay, az, gx, gy, gz);

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
```

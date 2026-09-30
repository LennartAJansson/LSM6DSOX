#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "cJSON.h"

static const char *TAG = "LSM6DSOX_APP";

// WiFi-uppgifter kommer från .env via read_env.py (se .env.example) – ligger inte i git
#if !defined(WIFI_SSID) || !defined(WIFI_PASS)
#error "WIFI_SSID/WIFI_PASS saknas. Skapa en .env i projektroten (se .env.example)."
#endif

#define I2C_MASTER_SDA_IO           11
#define I2C_MASTER_SCL_IO           12
#define LSM6DSOX_I2C_ADDRESS        0x6A

#define LSM6DSOX_WHO_AM_I_REG       0x0F
#define LSM6DSOX_CTRL1_XL           0x10
#define LSM6DSOX_CTRL2_G            0x11
#define LSM6DSOX_OUTX_L_G           0x22

#define NATS_HOST                   "192.168.1.254"   // <-- din NATS-server
#define NATS_PORT                   4222
#define NATS_SUBJECT                "sensors.lsm6dsox.raw"
#define SENSOR_TYPE                 "lsm6dsox"

i2c_master_dev_handle_t dev_handle;
static EventGroupHandle_t wifi_event_group;
static int nats_sock = -1;
static esp_netif_t *sta_netif = NULL; // För att kunna läsa ut IP-adressen i kuvertet
#define WIFI_CONNECTED_BIT BIT0

// ---------------- WiFi ----------------

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi tappat, försöker ansluta igen...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Fick IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_LOGI(TAG, "WiFi SSID: %s", WIFI_SSID);
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    // Kopiera in de dolda värdena säkert i minnet om det bråkar
    // strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
    // strcpy((char *)wifi_config.sta.password, WIFI_PASS);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Ansluter till WiFi...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

// ---------------- NATS ----------------

static esp_err_t nats_connect(void) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(NATS_PORT),
    };
    struct hostent *he = gethostbyname(NATS_HOST);
    if (!he) {
        ESP_LOGE(TAG, "Kunde inte slå upp NATS-host");
        return ESP_FAIL;
    }
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);

    nats_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (nats_sock < 0) return ESP_FAIL;

    if (connect(nats_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "NATS connect() misslyckades: errno %d", errno);
        close(nats_sock);
        nats_sock = -1;
        return ESP_FAIL;
    }

    // Läs bort serverns INFO-rad först, skicka sedan CONNECT (samma ordning som Barometer)
    char buf[512];
    recv(nats_sock, buf, sizeof(buf), 0);

    const char *connect_msg = "CONNECT {\"verbose\":false,\"pedantic\":false,\"tls_required\":false,\"name\":\"esp32-s3-lsm6dsox\"}\r\n";
    send(nats_sock, connect_msg, strlen(connect_msg), 0);

    ESP_LOGI(TAG, "Ansluten till NATS på %s:%d", NATS_HOST, NATS_PORT);
    return ESP_OK;
}

// Läser det servern skickat (utan att blockera) och svarar på PING med PONG,
// annars kopplar servern ner oss som "stale connection" efter några minuter.
static void nats_service_incoming(void) {
    char buf[256];
    int len = recv(nats_sock, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    if (len > 0) {
        buf[len] = 0;
        if (strstr(buf, "PING") != NULL) {
            send(nats_sock, "PONG\r\n", 6, 0);
        }
    } else if (len == 0) {
        ESP_LOGW(TAG, "NATS-servern stängde anslutningen, kopplar om");
        close(nats_sock);
        nats_sock = -1;
    }
    // len < 0 med EAGAIN/EWOULDBLOCK = inget att läsa just nu
}

static esp_err_t nats_publish(const char *subject, const char *payload, size_t len) {
    if (nats_sock >= 0) {
        nats_service_incoming();
    }
    if (nats_sock < 0) {
        if (nats_connect() != ESP_OK) return ESP_FAIL;
    }
    char header[128];
    int hlen = snprintf(header, sizeof(header), "PUB %s %d\r\n", subject, (int)len);

    if (send(nats_sock, header, hlen, 0) < 0 ||
        send(nats_sock, payload, len, 0) < 0 ||
        send(nats_sock, "\r\n", 2, 0) < 0) {
        ESP_LOGW(TAG, "NATS send() misslyckades, kopplar om");
        close(nats_sock);
        nats_sock = -1;
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ---------------- Kuvert ----------------

// Samma sändar-identitet som Barometer: "MAC=XX:XX:XX:XX:XX:XX,IP=YYY.YYY.YYY.YYY"
static void get_sender_identity(char *id_buffer, size_t max_len) {
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    esp_netif_ip_info_t ip_info;
    char ip_str[16] = "0.0.0.0";
    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
        esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    }

    snprintf(id_buffer, max_len, "MAC=%02X:%02X:%02X:%02X:%02X:%02X,IP=%s",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ip_str);
}

// ---------------- LSM6DSOX ----------------

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

// ---------------- app_main ----------------

void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();

    i2c_master_init();

    uint8_t who_am_i = lsm6dsox_read_reg(LSM6DSOX_WHO_AM_I_REG);
    if (who_am_i != 0x6C) {
        ESP_LOGE(TAG, "Felaktigt WHO_AM_I: 0x%02X (Förväntade 0x6C)", who_am_i);
        return;
    }
    ESP_LOGI(TAG, "LSM6DSOX hittad! ID: 0x%02X", who_am_i);

    lsm6dsox_write_reg(LSM6DSOX_CTRL1_XL, 0x40);
    lsm6dsox_write_reg(LSM6DSOX_CTRL2_G, 0x40);

    nats_connect();

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

        // ---- Publicera till NATS ----
        // 1. Mätvärdes-JSON (det inre värdet)
        cJSON *value = cJSON_CreateObject();
        cJSON_AddNumberToObject(value, "t", (double)esp_log_timestamp());
        cJSON_AddNumberToObject(value, "ax", ax);
        cJSON_AddNumberToObject(value, "ay", ay);
        cJSON_AddNumberToObject(value, "az", az);
        cJSON_AddNumberToObject(value, "gx", gx);
        cJSON_AddNumberToObject(value, "gy", gy);
        cJSON_AddNumberToObject(value, "gz", gz);
        char *value_json = cJSON_PrintUnformatted(value);

        // 2. Samma kuvert som Barometer: {"type":..,"sender":..,"value":"<mätvärdes-JSON som sträng>"}
        //    cJSON eskapar citationstecknen i value åt oss.
        char sender[64];
        get_sender_identity(sender, sizeof(sender));

        cJSON *envelope = cJSON_CreateObject();
        cJSON_AddStringToObject(envelope, "type", SENSOR_TYPE);
        cJSON_AddStringToObject(envelope, "sender", sender);
        cJSON_AddStringToObject(envelope, "value", value_json);
        char *json = cJSON_PrintUnformatted(envelope);

        if (json) {
            nats_publish(NATS_SUBJECT, json, strlen(json));
        }

        free(json);
        free(value_json);
        cJSON_Delete(envelope);
        cJSON_Delete(value);

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

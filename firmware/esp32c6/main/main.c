#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

#include "app.h"

static const char *TAG = "c6test";

#define LED_GPIO    CONFIG_KB_LED_GPIO
#define BUTTON_GPIO CONFIG_KB_BUTTON_GPIO

volatile net_state_t g_net_state = NET_CONNECTING;
volatile led_mode_t g_led_mode = LED_AUTO;
char g_hostname[32];
char g_service_name[32];

static temperature_sensor_handle_t s_temp;
static bool s_web_started;

float app_chip_temp(void)
{
    float t = 0;
    if (s_temp) {
        temperature_sensor_get_celsius(s_temp, &t);
    }
    return t;
}

void app_reprovision(void)
{
    ESP_LOGW(TAG, "clearing Wi-Fi credentials, restarting into BLE provisioning");
    wifi_prov_mgr_reset_provisioning();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_PROV_EVENT) {
        switch (id) {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "BLE provisioning started, device name %s, PoP %s", g_service_name, CONFIG_KB_PROV_POP);
            break;
        case WIFI_PROV_CRED_RECV: {
            wifi_sta_config_t *c = (wifi_sta_config_t *)data;
            ESP_LOGI(TAG, "received SSID \"%s\"", (const char *)c->ssid);
            break;
        }
        case WIFI_PROV_CRED_FAIL:
            ESP_LOGE(TAG, "provisioning failed (%s), retry from the app",
                     *(wifi_prov_sta_fail_reason_t *)data == WIFI_PROV_STA_AUTH_ERROR ? "wrong password" : "AP not found");
            wifi_prov_mgr_reset_sm_state_on_failure();
            break;
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "provisioning successful");
            break;
        case WIFI_PROV_END:
            wifi_prov_mgr_deinit();
            break;
        }
    } else if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            if (g_net_state != NET_PROVISIONING) {
                g_net_state = NET_CONNECTING;
            }
            ESP_LOGW(TAG, "disconnected, reconnecting");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR ", open http://" IPSTR "/ or http://%s.local/",
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.ip), g_hostname);
        g_net_state = NET_CONNECTED;
        if (!s_web_started) {
            s_web_started = true;
            web_start();
        }
    }
}

static void led_task(void *arg)
{
    int tick = 0;
    for (;;) {
        int on;
        switch (g_led_mode) {
        case LED_ON:
            on = 1;
            break;
        case LED_OFF:
            on = 0;
            break;
        default:
            if (g_net_state == NET_PROVISIONING) {
                on = (tick % 10) < 1;      /* short blink every 1 s: waiting for BLE */
            } else if (g_net_state == NET_CONNECTING) {
                on = (tick % 2) == 0;      /* 5 Hz: connecting */
            } else {
                on = 1;                    /* steady: connected */
            }
        }
        gpio_set_level(LED_GPIO, on);
        tick++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void button_task(void *arg)
{
    int held = 0;
    for (;;) {
        if (gpio_get_level(BUTTON_GPIO) == 0) {
            if (++held == 50) {            /* 5 s */
                app_reprovision();
            }
        } else {
            held = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void io_init(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_INPUT_OUTPUT);
    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);

    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tc, &s_temp) == ESP_OK) {
        temperature_sensor_enable(s_temp);
    } else {
        s_temp = NULL;
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    io_init();
    xTaskCreate(led_task, "led", 2048, NULL, 3, NULL);
    xTaskCreate(button_task, "button", 2048, NULL, 3, NULL);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_service_name, sizeof(g_service_name), "ESP32C6_%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(g_hostname, sizeof(g_hostname), "esp32c6-%02x%02x%02x", mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));

    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, g_hostname);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(g_hostname);
    mdns_instance_name_set("ESP32-C6 MCU test");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    wifi_prov_mgr_config_t pcfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(pcfg));

    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    ESP_LOGI(TAG, "ESP32-C6 MCU test, BLE name %s, host %s", g_service_name, g_hostname);
    if (!provisioned) {
        g_net_state = NET_PROVISIONING;
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, CONFIG_KB_PROV_POP,
                                                         g_service_name, NULL));
    } else {
        ESP_LOGI(TAG, "already provisioned, connecting (hold SERVICE 5 s to re-provision)");
        wifi_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* WROOM-1 GPIOs offered on the test panel: IO12/IO13 (USB) and the LED / button pins are excluded */
static const int s_pins[] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
#define NPINS (sizeof(s_pins) / sizeof(s_pins[0]))
static char s_mode[NPINS][4]; /* "-", "in", "pu", "pd", "o0", "o1" */

static const char *pin_note(int pin)
{
    switch (pin) {
    case 0: case 1: case 2: case 3: return "TIC CS";
    case 4: return "TWAI0 TX";
    case 5: return "TWAI0 RX";
    case 6: return "VIN ADC";
    case 8: return "AIN latch (strap)";
    case 10: return "DO_EN";
    case 11: return "ADC CS";
    case 15: return "TIC INT";
    case 16: return "UART0 TX";
    case 17: return "UART0 RX";
    case 18: return "SPI MOSI";
    case 19: return "SPI SCLK";
    case 20: return "SPI MISO";
    case 21: return "DO CS";
    case 22: return "TWAI1 TX";
    case 23: return "TWAI1 RX";
    default: return "";
    }
}

static int pin_index(int pin)
{
    for (int i = 0; i < NPINS; i++) {
        if (s_pins[i] == pin) {
            return i;
        }
    }
    return -1;
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t status_get(httpd_req_t *req)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    wifi_ap_record_t ap = {0};
    bool linked = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);

    const esp_app_desc_t *app = esp_app_get_description();
    static const char *net[] = {"provisioning", "connecting", "connected"};
    static const char *led[] = {"auto", "off", "on"};

    char buf[768];
    snprintf(buf, sizeof(buf),
             "{\"chip\":\"ESP32-C6\",\"rev\":\"v%d.%d\",\"cores\":%d,\"flash_mb\":%lu,"
             "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"idf\":\"%s\",\"app\":\"%s %s\","
             "\"uptime_s\":%llu,\"heap\":%lu,\"heap_min\":%lu,\"temp_c\":%.1f,"
             "\"reset\":%d,\"host\":\"%s\",\"ble\":\"%s\",\"net\":\"%s\","
             "\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"ip\":\"" IPSTR "\","
             "\"led\":\"%s\",\"led_level\":%d,\"button\":%d}",
             chip.revision / 100, chip.revision % 100, chip.cores, (unsigned long)(flash >> 20),
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], esp_get_idf_version(), app->date, app->time,
             (unsigned long long)(esp_timer_get_time() / 1000000), (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(), app_chip_temp(), (int)esp_reset_reason(),
             g_hostname, g_service_name, net[g_net_state], linked ? (const char *)ap.ssid : "",
             linked ? ap.rssi : 0, linked ? ap.primary : 0, IP2STR(&ip.ip), led[g_led_mode],
             gpio_get_level(CONFIG_KB_LED_GPIO), gpio_get_level(CONFIG_KB_BUTTON_GPIO));
    return send_json(req, buf);
}

static esp_err_t gpio_get(httpd_req_t *req)
{
    static char buf[2048]; /* httpd runs handlers on one task */
    int n = snprintf(buf, sizeof(buf), "[");
    for (int i = 0; i < NPINS; i++) {
        int p = s_pins[i];
        n += snprintf(buf + n, sizeof(buf) - n, "%s{\"pin\":%d,\"mode\":\"%s\",\"level\":%d,\"note\":\"%s\"}",
                      i ? "," : "", p, s_mode[i][0] ? s_mode[i] : "-",
                      s_mode[i][0] ? gpio_get_level(p) : -1, pin_note(p));
    }
    snprintf(buf + n, sizeof(buf) - n, "]");
    return send_json(req, buf);
}

static bool query(httpd_req_t *req, const char *key, char *val, size_t len)
{
    char q[96];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
           httpd_query_key_value(q, key, val, len) == ESP_OK;
}

/* POST /api/gpio?pin=N&mode=in|pu|pd|o0|o1|- */
static esp_err_t gpio_post(httpd_req_t *req)
{
    char pin_s[8], mode[4];
    if (!query(req, "pin", pin_s, sizeof(pin_s)) || !query(req, "mode", mode, sizeof(mode))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pin and mode required");
    }
    int pin = atoi(pin_s);
    int i = pin_index(pin);
    if (i < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pin not on test list");
    }

    if (strcmp(mode, "-") == 0) {
        gpio_reset_pin(pin);
        s_mode[i][0] = 0;
    } else if (strcmp(mode, "in") == 0 || strcmp(mode, "pu") == 0 || strcmp(mode, "pd") == 0) {
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin, mode[1] == 'u' ? GPIO_PULLUP_ONLY : mode[1] == 'd' ? GPIO_PULLDOWN_ONLY : GPIO_FLOATING);
    } else if (strcmp(mode, "o0") == 0 || strcmp(mode, "o1") == 0) {
        gpio_reset_pin(pin);
        gpio_set_pull_mode(pin, GPIO_FLOATING);
        gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_level(pin, mode[1] == '1');
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad mode");
    }
    if (strcmp(mode, "-") != 0) {
        strlcpy(s_mode[i], mode, sizeof(s_mode[i]));
    }
    ESP_LOGI(TAG, "IO%d -> %s", pin, mode);
    return gpio_get(req);
}

/* POST /api/led?mode=auto|off|on */
static esp_err_t led_post(httpd_req_t *req)
{
    char mode[8];
    if (!query(req, "mode", mode, sizeof(mode))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode required");
    }
    g_led_mode = strcmp(mode, "on") == 0 ? LED_ON : strcmp(mode, "off") == 0 ? LED_OFF : LED_AUTO;
    return send_json(req, "{\"ok\":true}");
}

static void later(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    if (arg) {
        app_reprovision();
    } else {
        esp_restart();
    }
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    send_json(req, "{\"ok\":true}");
    xTaskCreate(later, "later", 3072, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t reprov_post(httpd_req_t *req)
{
    send_json(req, "{\"ok\":true}");
    xTaskCreate(later, "later", 3072, (void *)1, 5, NULL);
    return ESP_OK;
}

void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    httpd_handle_t srv;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/gpio", .method = HTTP_GET, .handler = gpio_get},
        {.uri = "/api/gpio", .method = HTTP_POST, .handler = gpio_post},
        {.uri = "/api/led", .method = HTTP_POST, .handler = led_post},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post},
        {.uri = "/api/reprovision", .method = HTTP_POST, .handler = reprov_post},
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
    ESP_LOGI(TAG, "web server on port 80");
}

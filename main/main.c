#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/gpio.h"

#define WIFI_SSID      "Your_SSID"
#define WIFI_PASS      "Your_Password"
#define MAX_STA_CONN   4

// तीन रिले के GPIO पिन (ज़रूरत के हिसाब से बदलें)
#define RELAY1_GPIO    16
#define RELAY2_GPIO    17
#define RELAY3_GPIO    18

static const char *TAG = "HOME_AUTOMATION";
static int relay_state[3] = {0, 0, 0};   // 0 = OFF, 1 = ON

// HTML + CSS + JavaScript वेब पेज
static const char *html_page =
"<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Home Automation</title><style>"
"body{font-family:Arial;text-align:center;margin-top:50px;}"
".switch{position:relative;display:inline-block;width:60px;height:34px;margin:20px;}"
".switch input{display:none;}"
".slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background-color:#ccc;transition:.4s;border-radius:34px;}"
".slider:before{position:absolute;content:'';height:26px;width:26px;left:4px;bottom:4px;background-color:white;transition:.4s;border-radius:50%;}"
"input:checked+.slider{background-color:#2196F3;}"
"input:checked+.slider:before{transform:translateX(26px);}"
".label{font-size:20px;margin-top:10px;}"
"</style></head><body>"
"<h2>ESP32 Relay Control</h2>"
"<div id='relay1'><span class='label'>Relay 1</span><label class='switch'><input type='checkbox' id='r1' onchange='setRelay(1)'><span class='slider'></span></label></div>"
"<div id='relay2'><span class='label'>Relay 2</span><label class='switch'><input type='checkbox' id='r2' onchange='setRelay(2)'><span class='slider'></span></label></div>"
"<div id='relay3'><span class='label'>Relay 3</span><label class='switch'><input type='checkbox' id='r3' onchange='setRelay(3)'><span class='slider'></span></label></div>"
"<script>"
"function updateUI(){fetch('/status').then(r=>r.json()).then(d=>{"
"document.getElementById('r1').checked=d.relay1;"
"document.getElementById('r2').checked=d.relay2;"
"document.getElementById('r3').checked=d.relay3;});}"
"function setRelay(n){let s=document.getElementById('r'+n).checked?1:0;"
"fetch('/set?relay='+n+'&state='+s).then(r=>r.text()).then(()=>updateUI());}"
"window.onload=updateUI;"
"</script></body></html>";

// WiFi इवेंट हैंडलर
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
        esp_wifi_connect();
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
        esp_wifi_connect();
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// WiFi स्टेशन मोड शुरू करें
static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t any_id;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, &any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, &any_id);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // ⚠️ यहाँ बदलाव: ESP_IF_WIFI_STA की जगह WIFI_IF_STA का प्रयोग
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// GPIO सेटअप (रिले आउटपुट)
static void gpio_init_relays(void) {
    gpio_reset_pin(RELAY1_GPIO);
    gpio_set_direction(RELAY1_GPIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin(RELAY2_GPIO);
    gpio_set_direction(RELAY2_GPIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin(RELAY3_GPIO);
    gpio_set_direction(RELAY3_GPIO, GPIO_MODE_OUTPUT);
}

// सभी रिले को मौजूदा स्टेट के अनुसार सेट करें
static void apply_relay_states(void) {
    gpio_set_level(RELAY1_GPIO, relay_state[0]);
    gpio_set_level(RELAY2_GPIO, relay_state[1]);
    gpio_set_level(RELAY3_GPIO, relay_state[2]);
}

// /status हैंडलर (JSON रिस्पॉन्स)
static esp_err_t status_handler(httpd_req_t *req) {
    char json[128];
    snprintf(json, sizeof(json),
             "{\"relay1\":%d,\"relay2\":%d,\"relay3\":%d}",
             relay_state[0], relay_state[1], relay_state[2]);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

// /set हैंडलर (GET पैरामीटर: relay=X&state=Y)
static esp_err_t set_handler(httpd_req_t *req) {
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[16];
        int relay = 0, state = 0;
        if (httpd_query_key_value(buf, "relay", param, sizeof(param)) == ESP_OK)
            relay = atoi(param);
        if (httpd_query_key_value(buf, "state", param, sizeof(param)) == ESP_OK)
            state = atoi(param);

        if (relay >= 1 && relay <= 3 && (state == 0 || state == 1)) {
            relay_state[relay-1] = state;
            apply_relay_states();
            httpd_resp_sendstr(req, "OK");
            return ESP_OK;
        }
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

// रूट हैंडलर (HTML पेज)
static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, strlen(html_page));
    return ESP_OK;
}

// HTTP सर्वर शुरू करें
static void start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler};
        httpd_register_uri_handler(server, &root);
        httpd_uri_t status = {.uri = "/status", .method = HTTP_GET, .handler = status_handler};
        httpd_register_uri_handler(server, &status);
        httpd_uri_t set = {.uri = "/set", .method = HTTP_GET, .handler = set_handler};
        httpd_register_uri_handler(server, &set);
        ESP_LOGI(TAG, "Webserver started");
    }
}

void app_main(void) {
    // NVS फ्लैश इनिशियलाइज़ करें
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    gpio_init_relays();
    apply_relay_states();

    wifi_init_sta();
    // WiFi कनेक्ट होने का इंतज़ार
    vTaskDelay(pdMS_TO_TICKS(5000));
    start_webserver();
}

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "esp_task_wdt.h"

// -------------------- कॉन्फ़िगरेशन --------------------
#define WIFI_SSID           "Your_SSID"
#define WIFI_PASS           "Your_Password"
#define RELAY1_GPIO         16
#define RELAY2_GPIO         17
#define RELAY3_GPIO         18
#define NVS_NAMESPACE       "relay_state"
#define NVS_KEY             "states"
#define WATCHDOG_TIMEOUT_S  10

static const char *TAG = "SMART_HOME";
static int relay_state[3] = {0, 0, 0};   // 0 = OFF, 1 = ON

// -------------------- आधुनिक, स्मूथ HTML/UI --------------------
static const char *html_page =
"<!DOCTYPE html><html lang=\"hi\"><head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
"<title>Smart Home - Relay Control</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box;}"
"body{font-family:'Segoe UI','Poppins',Arial,sans-serif;"
"background:linear-gradient(135deg,#0f0c29,#302b63,#24243e);"
"min-height:100vh;display:flex;justify-content:center;align-items:center;"
"padding:20px;animation:gradientShift 8s ease infinite;background-size:400% 400%;}"
"@keyframes gradientShift{0%{background-position:0% 50%}50%{background-position:100% 50%}100%{background-position:0% 50%}}"
".container{width:100%;max-width:500px;}"
"h1{text-align:center;font-size:2.5rem;font-weight:700;color:#fff;"
"text-shadow:0 0 20px rgba(0,255,255,0.6),0 0 40px rgba(0,255,255,0.3);"
"margin-bottom:30px;letter-spacing:2px;"
"animation:glowPulse 2s ease-in-out infinite alternate;}"
"@keyframes glowPulse{from{text-shadow:0 0 10px #0ff,0 0 30px #0ff}to{text-shadow:0 0 25px #f0f,0 0 50px #f0f}}"
".card{background:rgba(255,255,255,0.08);backdrop-filter:blur(12px);"
"-webkit-backdrop-filter:blur(12px);border-radius:20px;padding:20px 25px;"
"margin:20px 0;display:flex;align-items:center;justify-content:space-between;"
"border:1px solid rgba(255,255,255,0.15);box-shadow:0 10px 30px rgba(0,0,0,0.5);"
"transition:transform 0.4s cubic-bezier(0.25,0.1,0.25,1),box-shadow 0.4s;}"
".card:hover{transform:translateY(-5px);box-shadow:0 15px 40px rgba(0,255,200,0.4);}"
".device-info{display:flex;align-items:center;gap:15px;}"
".icon{font-size:2.5rem;filter:drop-shadow(0 0 10px currentColor);}"
".device-name{color:#fff;font-size:1.3rem;font-weight:600;letter-spacing:1px;text-shadow:0 2px 5px rgba(0,0,0,0.5);}"
".switch{position:relative;display:inline-block;width:70px;height:36px;}"
".switch input{opacity:0;width:0;height:0;}"
".slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;"
"background:linear-gradient(145deg,#444,#222);border-radius:34px;"
"transition:0.4s cubic-bezier(0.4,0,0.2,1);"
"box-shadow:inset 0 2px 5px rgba(0,0,0,0.6),0 2px 10px rgba(0,0,0,0.3);}"
".slider:before{position:absolute;content:\"\";height:28px;width:28px;left:4px;bottom:4px;"
"background:radial-gradient(circle at 30% 30%,#fff,#ccc);border-radius:50%;"
"transition:0.4s cubic-bezier(0.4,0,0.2,1);box-shadow:0 2px 8px rgba(0,0,0,0.5);}"
"input:checked+.slider{background:linear-gradient(145deg,#00c853,#009624);"
"box-shadow:0 0 20px #00e676,inset 0 1px 4px rgba(255,255,255,0.4);}"
"input:checked+.slider:before{transform:translateX(34px);"
"background:radial-gradient(circle at 30% 30%,#fff,#b9f6ca);box-shadow:0 0 15px #0f0;}"
"@media(max-width:400px){.card{padding:15px 20px}.device-name{font-size:1.1rem}}"
"</style></head>"
"<body>"
"<div class=\"container\">"
"<h1>⚡ Smart Home ⚡</h1>"
"<div class=\"card\"><div class=\"device-info\"><span class=\"icon\">💡</span><span class=\"device-name\">Living Room Light</span></div>"
"<label class=\"switch\"><input type=\"checkbox\" id=\"r1\" onchange=\"setRelay(1)\"><span class=\"slider\"></span></label></div>"
"<div class=\"card\"><div class=\"device-info\"><span class=\"icon\">🌀</span><span class=\"device-name\">Ceiling Fan</span></div>"
"<label class=\"switch\"><input type=\"checkbox\" id=\"r2\" onchange=\"setRelay(2)\"><span class=\"slider\"></span></label></div>"
"<div class=\"card\"><div class=\"device-info\"><span class=\"icon\">🔌</span><span class=\"device-name\">Water Heater</span></div>"
"<label class=\"switch\"><input type=\"checkbox\" id=\"r3\" onchange=\"setRelay(3)\"><span class=\"slider\"></span></label></div>"
"</div>"
"<script>"
"function updateUI(){fetch('/status').then(function(r){if(!r.ok)throw new Error('Network error');return r.json()})"
".then(function(d){document.getElementById('r1').checked=d.relay1;"
"document.getElementById('r2').checked=d.relay2;"
"document.getElementById('r3').checked=d.relay3;})"
".catch(function(e){console.log('Status fetch error:',e);});}"
"function setRelay(n){var s=document.getElementById('r'+n).checked?1:0;"
"fetch('/set?relay='+n+'&state='+s).then(function(r){if(r.ok)updateUI();})"
".catch(function(e){console.log('Set error:',e);});}"
"window.onload=updateUI;"
"</script></body></html>";

// -------------------- NVS से रिले स्टेट लोड/सेव --------------------
static void load_relay_states_from_nvs(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open read failed, using defaults");
        return;
    }
    uint8_t states[3];
    size_t size = sizeof(states);
    err = nvs_get_blob(handle, NVS_KEY, states, &size);
    if (err == ESP_OK && size == 3) {
        relay_state[0] = states[0] ? 1 : 0;
        relay_state[1] = states[1] ? 1 : 0;
        relay_state[2] = states[2] ? 1 : 0;
        ESP_LOGI(TAG, "Loaded relay states: %d %d %d", relay_state[0], relay_state[1], relay_state[2]);
    } else {
        ESP_LOGW(TAG, "No saved states, using defaults");
    }
    nvs_close(handle);
}

static void save_relay_states_to_nvs(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open write error: %d", err);
        return;
    }
    uint8_t states[3] = {relay_state[0], relay_state[1], relay_state[2]};
    err = nvs_set_blob(handle, NVS_KEY, states, sizeof(states));
    if (err == ESP_OK) {
        nvs_commit(handle);
    } else {
        ESP_LOGE(TAG, "NVS write error");
    }
    nvs_close(handle);
}

// -------------------- GPIO और रिले --------------------
static void apply_relay_states(void) {
    gpio_set_level(RELAY1_GPIO, relay_state[0]);
    gpio_set_level(RELAY2_GPIO, relay_state[1]);
    gpio_set_level(RELAY3_GPIO, relay_state[2]);
}

static void gpio_init_relays(void) {
    gpio_reset_pin(RELAY1_GPIO);
    gpio_set_direction(RELAY1_GPIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin(RELAY2_GPIO);
    gpio_set_direction(RELAY2_GPIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin(RELAY3_GPIO);
    gpio_set_direction(RELAY3_GPIO, GPIO_MODE_OUTPUT);
}

// -------------------- WiFi --------------------
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

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
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// -------------------- HTTP सर्वर हैंडलर --------------------
static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, strlen(html_page));
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req) {
    char json[128];
    snprintf(json, sizeof(json), "{\"relay1\":%d,\"relay2\":%d,\"relay3\":%d}",
             relay_state[0], relay_state[1], relay_state[2]);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

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
            save_relay_states_to_nvs();   // हर बदलाव के बाद NVS में सेव करें
            httpd_resp_sendstr(req, "OK");
            return ESP_OK;
        }
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

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
    } else {
        ESP_LOGE(TAG, "Failed to start webserver");
    }
}

// -------------------- वॉचडॉग टास्क --------------------
static void watchdog_task(void *arg) {
    while (1) {
        // वॉचडॉग को फीड करें ताकि सिस्टम हैंग न हो
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// -------------------- मुख्य app_main --------------------
void app_main(void) {
    // NVS इनिशियलाइज़
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // रिले GPIO और पिछली स्टेट लोड करें
    gpio_init_relays();
    load_relay_states_from_nvs();
    apply_relay_states();

    // WiFi कनेक्ट करें
    wifi_init_sta();
    vTaskDelay(pdMS_TO_TICKS(5000));   // कनेक्शन का थोड़ा इंतज़ार

    // वॉचडॉग शुरू करें (वैकल्पिक)
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WATCHDOG_TIMEOUT_S * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_config));
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    xTaskCreate(watchdog_task, "watchdog", 2048, NULL, 1, NULL);

    // वेबसर्वर प्रारंभ
    start_webserver();

    ESP_LOGI(TAG, "System ready. 24x7 reliable.");
}

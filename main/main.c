#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "driver/gpio.h"

/* =========================================================
 *                      USER CONFIG
 * ========================================================= */

/* -------- STA Wi-Fi -------- */
#define WIFI_STA_SSID       "Airtel_2.4GHz"
#define WIFI_STA_PASSWORD   "Kgf@0987"

/* -------- ESP32 Access Point -------- */
#define WIFI_AP_SSID        "ESP32-SMART-HOME"
#define WIFI_AP_PASSWORD    "ak@12345"

/*
 * AP password should normally be at least 8 characters.
 */

/* -------- Relay GPIO -------- */
#define RELAY1_GPIO         GPIO_NUM_16
#define RELAY2_GPIO         GPIO_NUM_17
#define RELAY3_GPIO         GPIO_NUM_18

/*
 * Change to 0 if your relay board is ACTIVE LOW.
 *
 * ACTIVE HIGH:
 *      GPIO HIGH = Relay ON
 *
 * ACTIVE LOW:
 *      GPIO LOW = Relay ON
 */
#define RELAY_ACTIVE_LEVEL  1

/* -------- NVS -------- */
#define NVS_NAMESPACE       "relay_state"
#define NVS_KEY             "states"

/* -------- HTTP -------- */
#define HTTP_PORT            80

/* -------- OTA -------- */
#define OTA_BUFFER_SIZE      4096

/* ========================================================= */

static const char *TAG = "SMART_HOME";

static int relay_state[3] = {0, 0, 0};

static SemaphoreHandle_t relay_mutex = NULL;

static httpd_handle_t web_server = NULL;


/* =========================================================
 *                    HTML USER INTERFACE
 * ========================================================= */

static const char html_page[] =
"<!DOCTYPE html>"
"<html lang='en'>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Smart Home</title>"

"<style>"

"*{box-sizing:border-box;margin:0;padding:0}"

"body{"
"font-family:Inter,system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
"background:#f3f5f7;"
"color:#1f2933;"
"min-height:100vh;"
"display:flex;"
"justify-content:center;"
"padding:32px 16px;"
"}"

".container{"
"width:100%;"
"max-width:560px;"
"}"

".header{"
"background:#ffffff;"
"border:1px solid #e3e7eb;"
"border-radius:18px;"
"padding:24px;"
"margin-bottom:16px;"
"box-shadow:0 5px 20px rgba(20,30,40,.06);"
"}"

".header h1{"
"font-size:25px;"
"font-weight:700;"
"letter-spacing:-.4px;"
"}"

".header p{"
"margin-top:6px;"
"font-size:14px;"
"color:#6b7280;"
"}"

".card{"
"background:#ffffff;"
"border:1px solid #e3e7eb;"
"border-radius:18px;"
"padding:20px;"
"margin-bottom:12px;"
"display:flex;"
"align-items:center;"
"justify-content:space-between;"
"box-shadow:0 4px 16px rgba(20,30,40,.045);"
"transition:.2s ease;"
"}"

".card:hover{"
"transform:translateY(-1px);"
"box-shadow:0 7px 20px rgba(20,30,40,.08);"
"}"

".info{"
"display:flex;"
"align-items:center;"
"gap:14px;"
"}"

".icon{"
"width:42px;"
"height:42px;"
"border-radius:12px;"
"background:#f1f4f6;"
"display:flex;"
"align-items:center;"
"justify-content:center;"
"font-size:20px;"
"}"

".name{"
"font-size:16px;"
"font-weight:600;"
"}"

".status{"
"font-size:12px;"
"color:#8a949e;"
"margin-top:3px;"
"}"

".status.on{"
"color:#16834a;"
"}"

".switch{"
"position:relative;"
"display:inline-block;"
"width:54px;"
"height:30px;"
"}"

".switch input{"
"opacity:0;"
"width:0;"
"height:0;"
"}"

".slider{"
"position:absolute;"
"inset:0;"
"background:#cbd1d7;"
"border-radius:30px;"
"cursor:pointer;"
"transition:.22s ease;"
"}"

".slider:before{"
"content:'';"
"position:absolute;"
"width:24px;"
"height:24px;"
"left:3px;"
"top:3px;"
"background:white;"
"border-radius:50%;"
"box-shadow:0 2px 5px rgba(0,0,0,.18);"
"transition:.22s ease;"
"}"

"input:checked+.slider{"
"background:#198754;"
"}"

"input:checked+.slider:before{"
"transform:translateX(24px);"
"}"

".footer{"
"text-align:center;"
"font-size:12px;"
"color:#9aa2aa;"
"margin-top:20px;"
"}"

"@media(max-width:400px){"
".header{padding:20px}"
".card{padding:17px}"
"}"

"</style>"
"</head>"

"<body>"

"<div class='container'>"

"<div class='header'>"
"<h1>Smart Home</h1>"
"<p>Relay Control</p>"
"</div>"

"<div class='card'>"
"<div class='info'>"
"<div class='icon'>💡</div>"
"<div>"
"<div class='name'>Living Room Light</div>"
"<div class='status' id='s1'>OFF</div>"
"</div>"
"</div>"
"<label class='switch'>"
"<input type='checkbox' id='r1' onchange='setRelay(1)'>"
"<span class='slider'></span>"
"</label>"
"</div>"

"<div class='card'>"
"<div class='info'>"
"<div class='icon'>🌀</div>"
"<div>"
"<div class='name'>Ceiling Fan</div>"
"<div class='status' id='s2'>OFF</div>"
"</div>"
"</div>"
"<label class='switch'>"
"<input type='checkbox' id='r2' onchange='setRelay(2)'>"
"<span class='slider'></span>"
"</label>"
"</div>"

"<div class='card'>"
"<div class='info'>"
"<div class='icon'>🔌</div>"
"<div>"
"<div class='name'>Water Heater</div>"
"<div class='status' id='s3'>OFF</div>"
"</div>"
"</div>"
"<label class='switch'>"
"<input type='checkbox' id='r3' onchange='setRelay(3)'>"
"<span class='slider'></span>"
"</label>"
"</div>"

"<div class='footer'>ESP32 Smart Home</div>"

"</div>"

"<script>"

"let busy=false;"

"function updateUI(){"
"fetch('/status',{cache:'no-store'})"
".then(r=>r.json())"
".then(d=>{"
"for(let i=1;i<=3;i++){"
"let on=!!d['relay'+i];"
"document.getElementById('r'+i).checked=on;"
"let s=document.getElementById('s'+i);"
"s.textContent=on?'ON':'OFF';"
"s.className='status'+(on?' on':'');"
"}"
"})"
".catch(()=>{});"
"}"

"function setRelay(n){"
"if(busy)return;"
"busy=true;"

"let checkbox=document.getElementById('r'+n);"
"let state=checkbox.checked?1:0;"

"fetch('/set?relay='+n+'&state='+state,{cache:'no-store'})"
".then(r=>{"
"if(!r.ok)throw new Error('failed');"
"return r.text();"
"})"
".then(()=>updateUI())"
".catch(()=>updateUI())"
".finally(()=>{busy=false;});"
"}"

"window.addEventListener('load',updateUI);"

"</script>"

"</body>"
"</html>";


/* =========================================================
 *                  RELAY HARDWARE
 * ========================================================= */

static int relay_output_level(int state)
{
    if (state) {
        return RELAY_ACTIVE_LEVEL;
    }

    return !RELAY_ACTIVE_LEVEL;
}


static void relay_apply_one(int index)
{
    gpio_num_t pin;

    switch(index) {
        case 0:
            pin = RELAY1_GPIO;
            break;

        case 1:
            pin = RELAY2_GPIO;
            break;

        case 2:
            pin = RELAY3_GPIO;
            break;

        default:
            return;
    }

    gpio_set_level(pin, relay_output_level(relay_state[index]));
}


static void relay_apply_all(void)
{
    relay_apply_one(0);
    relay_apply_one(1);
    relay_apply_one(2);
}


static void relay_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask =
            (1ULL << RELAY1_GPIO) |
            (1ULL << RELAY2_GPIO) |
            (1ULL << RELAY3_GPIO),

        .mode = GPIO_MODE_OUTPUT,

        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,

        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /*
     * Start safely OFF.
     */
    gpio_set_level(RELAY1_GPIO,
                   relay_output_level(0));

    gpio_set_level(RELAY2_GPIO,
                   relay_output_level(0));

    gpio_set_level(RELAY3_GPIO,
                   relay_output_level(0));
}


/* =========================================================
 *                        NVS
 * ========================================================= */

static esp_err_t relay_nvs_load(void)
{
    nvs_handle_t handle;

    esp_err_t err =
        nvs_open(NVS_NAMESPACE,
                 NVS_READONLY,
                 &handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "No saved relay state. Using OFF.");
        return err;
    }

    uint8_t states[3] = {0,0,0};
    size_t size = sizeof(states);

    err = nvs_get_blob(handle,
                       NVS_KEY,
                       states,
                       &size);

    nvs_close(handle);

    if (err != ESP_OK || size != sizeof(states)) {

        ESP_LOGW(TAG,
                 "Invalid saved state. Using OFF.");

        relay_state[0] = 0;
        relay_state[1] = 0;
        relay_state[2] = 0;

        return ESP_OK;
    }

    relay_state[0] = states[0] ? 1 : 0;
    relay_state[1] = states[1] ? 1 : 0;
    relay_state[2] = states[2] ? 1 : 0;

    ESP_LOGI(TAG,
             "Restored state: %d %d %d",
             relay_state[0],
             relay_state[1],
             relay_state[2]);

    return ESP_OK;
}


static esp_err_t relay_nvs_save(void)
{
    nvs_handle_t handle;

    esp_err_t err =
        nvs_open(NVS_NAMESPACE,
                 NVS_READWRITE,
                 &handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "NVS open failed: %s",
                 esp_err_to_name(err));

        return err;
    }

    uint8_t states[3];

    states[0] = relay_state[0];
    states[1] = relay_state[1];
    states[2] = relay_state[2];

    err = nvs_set_blob(handle,
                       NVS_KEY,
                       states,
                       sizeof(states));

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "NVS save failed: %s",
                 esp_err_to_name(err));
    }

    return err;
}


/* =========================================================
 *                     WIFI EVENT HANDLER
 * ========================================================= */

static void wifi_event_handler(
        void *arg,
        esp_event_base_t event_base,
        int32_t event_id,
        void *event_data)
{
    if (event_base == WIFI_EVENT) {

        if (event_id == WIFI_EVENT_STA_START) {

            ESP_LOGI(TAG,
                     "STA started. Connecting...");

            esp_wifi_connect();
        }

        else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {

            ESP_LOGW(TAG,
                     "STA disconnected. Reconnecting...");

            esp_wifi_connect();
        }

        else if (event_id == WIFI_EVENT_AP_START) {

            ESP_LOGI(TAG,
                     "Access Point started.");
        }
    }

    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG,
                 "STA IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));
    }
}


/* =========================================================
 *                       WIFI INIT
 * ========================================================= */

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(
        esp_event_loop_create_default());

    esp_netif_t *sta_netif =
        esp_netif_create_default_wifi_sta();

    esp_netif_t *ap_netif =
        esp_netif_create_default_wifi_ap();

    (void)sta_netif;
    (void)ap_netif;

    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL));

    wifi_config_t sta_config = {0};

    strncpy(
        (char *)sta_config.sta.ssid,
        WIFI_STA_SSID,
        sizeof(sta_config.sta.ssid));

    strncpy(
        (char *)sta_config.sta.password,
        WIFI_STA_PASSWORD,
        sizeof(sta_config.sta.password));

    sta_config.sta.threshold.authmode =
        WIFI_AUTH_WPA2_PSK;

    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;


    wifi_config_t ap_config = {0};

    strncpy(
        (char *)ap_config.ap.ssid,
        WIFI_AP_SSID,
        sizeof(ap_config.ap.ssid));

    strncpy(
        (char *)ap_config.ap.password,
        WIFI_AP_PASSWORD,
        sizeof(ap_config.ap.password));

    ap_config.ap.ssid_len =
        strlen(WIFI_AP_SSID);

    ap_config.ap.channel = 1;

    ap_config.ap.max_connection = 4;

    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;


    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_APSTA));

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &sta_config));

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_AP,
            &ap_config));

    ESP_ERROR_CHECK(
        esp_wifi_start());

    ESP_LOGI(TAG,
             "WiFi started.");
}


/* =========================================================
 *                    HTTP: ROOT PAGE
 * ========================================================= */

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store, no-cache, must-revalidate");

    return httpd_resp_send(
        req,
        html_page,
        HTTPD_RESP_USE_STRLEN);
}


/* =========================================================
 *                    HTTP: STATUS
 * ========================================================= */

static esp_err_t status_handler(httpd_req_t *req)
{
    char json[128];

    xSemaphoreTake(relay_mutex, portMAX_DELAY);

    int r1 = relay_state[0];
    int r2 = relay_state[1];
    int r3 = relay_state[2];

    xSemaphoreGive(relay_mutex);

    snprintf(
        json,
        sizeof(json),
        "{\"relay1\":%d,\"relay2\":%d,\"relay3\":%d}",
        r1,
        r2,
        r3);

    httpd_resp_set_type(
        req,
        "application/json");

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store");

    return httpd_resp_send(
        req,
        json,
        HTTPD_RESP_USE_STRLEN);
}


/* =========================================================
 *                     HTTP: SET RELAY
 * ========================================================= */

static esp_err_t set_handler(httpd_req_t *req)
{
    char query[128];

    if (httpd_req_get_url_query_str(
            req,
            query,
            sizeof(query)) != ESP_OK) {

        httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Missing parameters");

        return ESP_FAIL;
    }

    char relay_str[8] = {0};
    char state_str[8] = {0};

    if (httpd_query_key_value(
            query,
            "relay",
            relay_str,
            sizeof(relay_str)) != ESP_OK) {

        httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid relay");

        return ESP_FAIL;
    }

    if (httpd_query_key_value(
            query,
            "state",
            state_str,
            sizeof(state_str)) != ESP_OK) {

        httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid state");

        return ESP_FAIL;
    }

    char *endptr;

    long relay =
        strtol(relay_str, &endptr, 10);

    if (*endptr != '\0' ||
        relay < 1 ||
        relay > 3) {

        httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid relay");

        return ESP_FAIL;
    }

    long state =
        strtol(state_str, &endptr, 10);

    if (*endptr != '\0' ||
        (state != 0 && state != 1)) {

        httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid state");

        return ESP_FAIL;
    }


    xSemaphoreTake(
        relay_mutex,
        portMAX_DELAY);

    relay_state[relay - 1] = state;

    relay_apply_one(relay - 1);

    /*
     * Save immediately.
     *
     * This means that after a successful command,
     * the state is persistent even after reboot.
     */
    esp_err_t save_err =
        relay_nvs_save();

    xSemaphoreGive(
        relay_mutex);


    if (save_err != ESP_OK) {

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "State save failed");

        return ESP_FAIL;
    }

    httpd_resp_set_type(
        req,
        "text/plain");

    return httpd_resp_sendstr(
        req,
        "OK");
}


/* =========================================================
 *                        OTA PAGE
 * ========================================================= */

static const char ota_page[] =
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 OTA</title>"
"<style>"
"body{font-family:system-ui;background:#f3f5f7;padding:30px;color:#222}"
".box{max-width:500px;margin:auto;background:white;padding:25px;"
"border-radius:18px;box-shadow:0 5px 20px #0001}"
"input,button{width:100%;padding:12px;margin-top:12px}"
"button{border:0;border-radius:10px;background:#198754;color:white;font-weight:600}"
"</style>"
"</head>"
"<body>"
"<div class='box'>"
"<h2>ESP32 Firmware Update</h2>"
"<form method='POST' action='/update' enctype='multipart/form-data'>"
"<input type='file' name='firmware' accept='.bin' required>"
"<button type='submit'>Update Firmware</button>"
"</form>"
"</div>"
"</body>"
"</html>";


static esp_err_t ota_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(
        req,
        "text/html");

    return httpd_resp_send(
        req,
        ota_page,
        HTTPD_RESP_USE_STRLEN);
}


/* =========================================================
 *                         OTA UPDATE
 * ========================================================= */

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);

    if (update_partition == NULL) {

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "No OTA partition");

        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "OTA partition: %s",
             update_partition->label);


    esp_ota_handle_t ota_handle = 0;

    esp_err_t err =
        esp_ota_begin(
            update_partition,
            OTA_SIZE_UNKNOWN,
            &ota_handle);

    if (err != ESP_OK) {

        ESP_LOGE(TAG,
                 "esp_ota_begin failed: %s",
                 esp_err_to_name(err));

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "OTA begin failed");

        return ESP_FAIL;
    }


    char *buffer =
        malloc(OTA_BUFFER_SIZE);

    if (buffer == NULL) {

        esp_ota_abort(ota_handle);

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Memory allocation failed");

        return ESP_FAIL;
    }


    int remaining = req->content_len;

    while (remaining > 0) {

        int received =
            httpd_req_recv(
                req,
                buffer,
                remaining > OTA_BUFFER_SIZE
                    ? OTA_BUFFER_SIZE
                    : remaining);

        if (received <= 0) {

            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }

            ESP_LOGE(TAG,
                     "OTA receive failed");

            free(buffer);

            esp_ota_abort(ota_handle);

            httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "OTA receive failed");

            return ESP_FAIL;
        }


        err =
            esp_ota_write(
                ota_handle,
                buffer,
                received);

        if (err != ESP_OK) {

            ESP_LOGE(TAG,
                     "OTA write failed: %s",
                     esp_err_to_name(err));

            free(buffer);

            esp_ota_abort(ota_handle);

            httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "OTA write failed");

            return ESP_FAIL;
        }

        remaining -= received;
    }


    free(buffer);


    err =
        esp_ota_end(ota_handle);

    if (err != ESP_OK) {

        ESP_LOGE(TAG,
                 "OTA end failed: %s",
                 esp_err_to_name(err));

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Invalid firmware");

        return ESP_FAIL;
    }


    err =
        esp_ota_set_boot_partition(
            update_partition);

    if (err != ESP_OK) {

        ESP_LOGE(TAG,
                 "Set boot partition failed: %s",
                 esp_err_to_name(err));

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Boot partition failed");

        return ESP_FAIL;
    }


    httpd_resp_set_type(
        req,
        "text/plain");

    httpd_resp_sendstr(
        req,
        "Firmware uploaded successfully. Restarting...");

    vTaskDelay(
        pdMS_TO_TICKS(1000));

    esp_restart();

    return ESP_OK;
}


/* =========================================================
 *                    HTTP SERVER START
 * ========================================================= */

static void start_webserver(void)
{
    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();

    config.server_port = HTTP_PORT;

    config.max_uri_handlers = 8;

    config.stack_size = 8192;

    config.recv_wait_timeout = 10;

    config.send_wait_timeout = 10;


    if (httpd_start(
            &web_server,
            &config) != ESP_OK) {

        ESP_LOGE(TAG,
                 "HTTP server failed");

        return;
    }


    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL
    };

    httpd_register_uri_handler(
        web_server,
        &root_uri);


    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL
    };

    httpd_register_uri_handler(
        web_server,
        &status_uri);


    httpd_uri_t set_uri = {
        .uri = "/set",
        .method = HTTP_GET,
        .handler = set_handler,
        .user_ctx = NULL
    };

    httpd_register_uri_handler(
        web_server,
        &set_uri);


    httpd_uri_t ota_page_uri = {
        .uri = "/update",
        .method = HTTP_GET,
        .handler = ota_page_handler,
        .user_ctx = NULL
    };

    httpd_register_uri_handler(
        web_server,
        &ota_page_uri);


    httpd_uri_t ota_upload_uri = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
        .user_ctx = NULL
    };

    httpd_register_uri_handler(
        web_server,
        &ota_upload_uri);


    ESP_LOGI(TAG,
             "HTTP server started on port %d",
             HTTP_PORT);
}


/* =========================================================
 *                         APP MAIN
 * ========================================================= */

void app_main(void)
{
    ESP_LOGI(TAG,
             "================================");

    ESP_LOGI(TAG,
             "ESP32 SMART HOME STARTING");

    ESP_LOGI(TAG,
             "================================");


    /*
     * NVS
     */

    esp_err_t ret =
        nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(
            nvs_flash_erase());

        ESP_ERROR_CHECK(
            nvs_flash_init());
    }

    ESP_ERROR_CHECK(ret);


    /*
     * Mutex
     */

    relay_mutex =
        xSemaphoreCreateMutex();

    if (relay_mutex == NULL) {

        ESP_LOGE(TAG,
                 "Mutex creation failed");

        esp_restart();
    }


    /*
     * GPIO first.
     * Start all relays OFF.
     */

    relay_gpio_init();


    /*
     * Restore previous state.
     */

    relay_nvs_load();


    /*
     * Apply saved state.
     */

    xSemaphoreTake(
        relay_mutex,
        portMAX_DELAY);

    relay_apply_all();

    xSemaphoreGive(
        relay_mutex);


    /*
     * Wi-Fi
     */

    wifi_init();


    /*
     * HTTP server
     */

    start_webserver();


    ESP_LOGI(TAG,
             "System ready.");
}

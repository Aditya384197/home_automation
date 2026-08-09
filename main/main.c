#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"

#define TAG "OFFLINE_HOME"

/* ---------------- User hardware/configuration ---------------- */

#define RELAY1_GPIO             16
#define RELAY2_GPIO             17
#define RELAY3_GPIO             18

/* Change to 0 if your relay board is active-low. */
#define RELAY_ACTIVE_LEVEL      1

#define DEFAULT_AP_SSID         "ESP32-SMART-HOME"
#define DEFAULT_AP_PASSWORD     "ChangeMe123"
#define DEFAULT_AP_CHANNEL      6
#define AP_MAX_CONNECTIONS      4

#define AP_IP_ADDR              "192.168.4.1"
#define AP_GW_ADDR              "192.168.4.1"
#define AP_NETMASK              "255.255.255.0"

#define NVS_NAMESPACE           "home_cfg"
#define NVS_KEY_RELAY_STATES    "relay"
#define NVS_KEY_AP_SSID         "ap_ssid"
#define NVS_KEY_AP_PASS         "ap_pass"

#define WATCHDOG_TIMEOUT_MS     10000
#define DNS_PORT                53
#define DNS_STACK_SIZE          3072
#define DNS_RX_SIZE             512
#define OTA_BUFFER_SIZE         4096

#define MAX_AP_SSID_LEN         32
#define MAX_AP_PASS_LEN         63

/* ------------------------------------------------------------- */

static int relay_state[3] = {0, 0, 0};
static SemaphoreHandle_t relay_mutex;
static SemaphoreHandle_t storage_mutex;
static SemaphoreHandle_t ota_mutex;

static char ap_ssid[MAX_AP_SSID_LEN + 1] = DEFAULT_AP_SSID;
static char ap_password[MAX_AP_PASS_LEN + 1] = DEFAULT_AP_PASSWORD;

static TaskHandle_t dns_task_handle = NULL;
static volatile bool ota_in_progress = false;
static httpd_handle_t http_server = NULL;

/* -------------------- Local Web UI -------------------- */

static const char *HTML_PAGE =
"<!doctype html><html lang='en'><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>"
"<meta name='theme-color' content='#111827'>"
"<title>ESP32 Smart Home</title>"
"<style>"
":root{--bg:#f3f5f7;--card:#fff;--text:#17202a;--muted:#697586;--line:#e5e7eb;--on:#168a4b;--off:#9aa3ad;--accent:#2563eb;--danger:#b42318}"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif}"
".wrap{width:min(680px,100%);margin:auto;padding:18px 14px 34px}.top{padding:8px 4px 18px}"
".topbar{display:flex;align-items:center;justify-content:space-between;gap:12px}"
"h1{font-size:25px;margin:0 0 5px}.sub{color:var(--muted);font-size:14px}"
".settings-btn{width:42px;height:42px;border:1px solid var(--line);border-radius:12px;background:#fff;display:flex;align-items:center;justify-content:center;font-size:21px;cursor:pointer;box-shadow:0 2px 8px rgba(15,23,42,.06)}"
".settings-btn:active{transform:scale(.96)}"
".card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;margin:12px 0;box-shadow:0 2px 10px rgba(15,23,42,.04)}"
".row{display:flex;align-items:center;justify-content:space-between;gap:15px}.name{font-weight:650;font-size:17px}.state{font-size:13px;color:var(--muted);margin-top:4px}"
".switch{position:relative;width:58px;height:32px;flex:none}.switch input{opacity:0;width:0;height:0}"
".slider{position:absolute;inset:0;background:#c8ced5;border-radius:40px;transition:.18s;cursor:pointer}"
".slider:before{content:'';position:absolute;width:26px;height:26px;left:3px;top:3px;background:white;border-radius:50%;box-shadow:0 1px 4px #0003;transition:.18s}"
"input:checked+.slider{background:var(--on)}input:checked+.slider:before{transform:translateX(26px)}"
".bar{display:flex;gap:8px;flex-wrap:wrap;margin-top:14px}button{border:1px solid var(--line);background:#fff;border-radius:10px;padding:10px 13px;font:inherit;cursor:pointer}"
"button.primary{background:var(--accent);border-color:var(--accent);color:#fff}button:disabled{opacity:.55;cursor:not-allowed}"
".msg{font-size:13px;margin-top:10px;color:var(--muted)}"
"input[type=text],input[type=password],input[type=file]{width:100%;padding:11px;border:1px solid #d5dae0;border-radius:10px;background:#fff;font:inherit}"
"label.field{display:block;font-size:13px;color:var(--muted);margin:13px 0 6px}.hidden{display:none}"
".status{display:inline-flex;align-items:center;gap:7px;font-size:12px;color:var(--muted)}.dot{width:8px;height:8px;border-radius:50%;background:var(--on)}"
".setting-item{display:flex;align-items:center;justify-content:space-between;gap:14px;padding:14px 0;border-top:1px solid var(--line)}"
".setting-item:first-child{border-top:0;padding-top:4px}.setting-title{font-weight:650;font-size:15px}.setting-desc{font-size:12px;color:var(--muted);margin-top:3px}"
".progress-wrap{margin-top:14px}.progress-head{display:flex;justify-content:space-between;gap:10px;font-size:12px;color:var(--muted);margin-bottom:6px}"
".progress{height:8px;background:#e8ebef;border-radius:20px;overflow:hidden}.progress-fill{height:100%;width:0;background:var(--accent);transition:width:.12s ease}"
"</style></head><body><main class='wrap'>"
"<header class='top'><div class='topbar'>"
"<div><h1>Smart Home</h1><div class='sub'>Local offline control</div></div>"
"<button class='settings-btn' onclick='togglePanel(\"settingsMenu\")' aria-label='Settings' title='Settings'>⚙</button>"
"</div></header>"
"<section id='controls'></section>"
"<section id='settingsMenu' class='card hidden'>"
"<div class='row'><div><div class='name'>Settings</div><div class='state'>Device configuration</div></div>"
"<button onclick='togglePanel(\"settingsMenu\")'>Close</button></div>"
"<div class='setting-item' style='margin-top:10px'>"
"<div><div class='setting-title'>OTA Update</div><div class='setting-desc'>Update firmware locally from a .bin file</div></div>"
"<button class='primary' onclick='togglePanel(\"ota\")'>Open</button></div>"
"<div class='setting-item'>"
"<div><div class='setting-title'>AP Configuration</div><div class='setting-desc'>Change the ESP32 local Wi-Fi SSID and password</div></div>"
"<button onclick='togglePanel(\"settings\")'>Open</button></div>"
"<div id='ota' class='hidden'>"
"<label class='field'>Firmware .bin</label><input id='fw' type='file' accept='.bin,application/octet-stream'>"
"<div class='bar'><button id='uploadBtn' class='primary' onclick='uploadFirmware()'>Upload & Restart</button></div>"
"<div id='otaProgress' class='progress-wrap hidden'>"
"<div class='progress-head'><span id='otaProgressText'>Uploading...</span><span id='otaPercent'>0%</span></div>"
"<div class='progress'><div id='otaFill' class='progress-fill'></div></div></div>"
"<div id='otamsg' class='msg'></div></div>"
"<div id='settings' class='hidden'>"
"<label class='field'>SSID</label><input id='ssid' maxlength='32'>"
"<label class='field'>Password (8-63 characters)</label><input id='pass' type='password' maxlength='63'>"
"<div class='bar'><button class='primary' onclick='saveSettings()'>Save & Restart</button></div>"
"<div id='setmsg' class='msg'></div></div>"
"</section>"
"<div class='status'><span class='dot'></span> ESP32 local AP</div>"
"</main><script>"
"const names=['Living Room Light','Ceiling Fan','Charging Socket'];"
"function render(a){let h='';a.forEach((v,i)=>{h+=`<section class='card'><div class='row'><div><div class='name'>${names[i]}</div><div class='state' id='st${i}'>${v?'ON':'OFF'}</div></div><label class='switch'><input type='checkbox' id='r${i}' ${v?'checked':''} onchange='setRelay(${i},this.checked)'><span class='slider'></span></label></div></section>`});document.getElementById('controls').innerHTML=h}"
"async function load(){try{let r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw 0;let d=await r.json();render([!!d.relay1,!!d.relay2,!!d.relay3])}catch(e){setTimeout(load,1200)}}"
"async function setRelay(i,on){let el=document.getElementById('r'+i);el.disabled=true;try{let r=await fetch(`/api/relay?relay=${i+1}&state=${on?1:0}`,{cache:'no-store'});if(!r.ok)throw 0;await load()}catch(e){el.checked=!on;alert('Relay command failed.')}finally{el.disabled=false}}"
"function togglePanel(id){document.getElementById(id).classList.toggle('hidden')}"
"async function saveSettings(){let s=document.getElementById('ssid').value,p=document.getElementById('pass').value,m=document.getElementById('setmsg');if(s.length<1||s.length>32||p.length<8||p.length>63){m.textContent='Invalid SSID or password.';return}m.textContent='Saving and restarting...';try{let r=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,password:p})});if(!r.ok)throw 0}catch(e){m.textContent='Connection lost. The AP may be restarting.'}}"
"async function loadSettings(){try{let r=await fetch('/api/settings',{cache:'no-store'}),d=await r.json();document.getElementById('ssid').value=d.ssid||''}catch(e){}}"
"function setOtaProgress(p){p=Math.max(0,Math.min(100,p));document.getElementById('otaProgress').classList.remove('hidden');document.getElementById('otaFill').style.width=p+'%';document.getElementById('otaPercent').textContent=Math.round(p)+'%'}"
"function uploadFirmware(){let f=document.getElementById('fw').files[0],m=document.getElementById('otamsg'),btn=document.getElementById('uploadBtn');if(!f){m.textContent='Select a .bin file first.';return}if(f.size<1024){m.textContent='Firmware file is too small.';return}if(!confirm('Start OTA update? The device will restart after a successful update.'))return;btn.disabled=true;m.textContent='Uploading... Do not disconnect.';setOtaProgress(0);let xhr=new XMLHttpRequest();xhr.open('POST','/api/ota',true);xhr.setRequestHeader('Content-Type','application/octet-stream');xhr.upload.onprogress=function(e){if(e.lengthComputable){setOtaProgress((e.loaded/e.total)*100);m.textContent='Uploading firmware...'}};xhr.onload=function(){if(xhr.status>=200&&xhr.status<300){setOtaProgress(100);m.textContent=xhr.responseText||'OTA successful. Restarting...';setTimeout(()=>location.reload(),8000)}else{btn.disabled=false;m.textContent='OTA failed. Current firmware should remain active.'}};xhr.onerror=function(){if(document.getElementById('otaPercent').textContent==='100%'){m.textContent='Firmware uploaded. Device may be restarting...'}else{btn.disabled=false;m.textContent='Upload interrupted. Current firmware should remain active.'}};xhr.ontimeout=function(){btn.disabled=false;m.textContent='OTA request timed out.'};xhr.send(f)}"
"load();loadSettings();"
"</script></body></html>";
/* -------------------- NVS / persistence -------------------- */

static bool valid_ssid(const char *s)
{
    size_t n = strnlen(s, MAX_AP_SSID_LEN + 1);
    return n >= 1 && n <= MAX_AP_SSID_LEN;
}

static bool valid_password(const char *s)
{
    size_t n = strnlen(s, MAX_AP_PASS_LEN + 1);
    return n >= 8 && n <= MAX_AP_PASS_LEN;
}

static void load_defaults(void)
{
    strlcpy(ap_ssid, DEFAULT_AP_SSID, sizeof(ap_ssid));
    strlcpy(ap_password, DEFAULT_AP_PASSWORD, sizeof(ap_password));
    relay_state[0] = relay_state[1] = relay_state[2] = 0;
}

static void load_nvs(void)
{
    load_defaults();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No existing config; using defaults");
        return;
    }

    uint8_t states[3];
    size_t sz = sizeof(states);
    if (nvs_get_blob(h, NVS_KEY_RELAY_STATES, states, &sz) == ESP_OK && sz == sizeof(states)) {
        for (int i = 0; i < 3; ++i) relay_state[i] = states[i] ? 1 : 0;
    }

    char tmp_ssid[MAX_AP_SSID_LEN + 1] = {0};
    sz = sizeof(tmp_ssid);
    if (nvs_get_str(h, NVS_KEY_AP_SSID, tmp_ssid, &sz) == ESP_OK && valid_ssid(tmp_ssid)) {
        strlcpy(ap_ssid, tmp_ssid, sizeof(ap_ssid));
    }

    char tmp_pass[MAX_AP_PASS_LEN + 1] = {0};
    sz = sizeof(tmp_pass);
    if (nvs_get_str(h, NVS_KEY_AP_PASS, tmp_pass, &sz) == ESP_OK && valid_password(tmp_pass)) {
        strlcpy(ap_password, tmp_pass, sizeof(ap_password));
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Restored relay states: %d %d %d", relay_state[0], relay_state[1], relay_state[2]);
}

static esp_err_t save_relay_states(void)
{
    uint8_t states[3];
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    for (int i = 0; i < 3; ++i) states[i] = relay_state[i];
    xSemaphoreGive(relay_mutex);

    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_RELAY_STATES, states, sizeof(states));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(storage_mutex);

    if (err != ESP_OK) ESP_LOGE(TAG, "Relay NVS save failed: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t save_ap_settings(const char *ssid, const char *password)
{
    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_AP_SSID, ssid);
        if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_AP_PASS, password);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(storage_mutex);

    if (err == ESP_OK) {
        strlcpy(ap_ssid, ssid, sizeof(ap_ssid));
        strlcpy(ap_password, password, sizeof(ap_password));
    }
    return err;
}

/* -------------------- GPIO / relay -------------------- */

static int relay_output_level(int logical_state)
{
    return logical_state ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL;
}

static void apply_all_relays(void)
{
    int s[3];
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    memcpy(s, relay_state, sizeof(s));
    xSemaphoreGive(relay_mutex);

    gpio_set_level(RELAY1_GPIO, relay_output_level(s[0]));
    gpio_set_level(RELAY2_GPIO, relay_output_level(s[1]));
    gpio_set_level(RELAY3_GPIO, relay_output_level(s[2]));
}

static void init_relays(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << RELAY1_GPIO) | (1ULL << RELAY2_GPIO) | (1ULL << RELAY3_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* Safe physical OFF before restoring persistent state. */
    gpio_set_level(RELAY1_GPIO, relay_output_level(0));
    gpio_set_level(RELAY2_GPIO, relay_output_level(0));
    gpio_set_level(RELAY3_GPIO, relay_output_level(0));
}

/* -------------------- Wi-Fi AP -------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_AP_START) ESP_LOGI(TAG, "AP started");
        else if (id == WIFI_EVENT_AP_STACONNECTED) ESP_LOGI(TAG, "Client connected");
        else if (id == WIFI_EVENT_AP_STADISCONNECTED) ESP_LOGI(TAG, "Client disconnected");
    }
}

static void wifi_init_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) ESP_ERROR_CHECK(ESP_FAIL);

    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(ap_netif, &ip_info));
    ip4addr_aton(AP_IP_ADDR, &ip_info.ip);
    ip4addr_aton(AP_GW_ADDR, &ip_info.gw);
    ip4addr_aton(AP_NETMASK, &ip_info.netmask);
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any));

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, ap_password, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(ap_ssid);
    ap.ap.channel = DEFAULT_AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONNECTIONS;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;
    ap.ap.pmf_cfg.capable = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP SSID: %s", ap_ssid);
    ESP_LOGI(TAG, "AP IP: %s", AP_IP_ADDR);
}

/* -------------------- Local DNS captive portal -------------------- */

static int find_question_end(const uint8_t *buf, int len)
{
    if (len < 17) return -1;
    int p = 12;
    int jumps = 0;
    while (p < len && jumps++ < 64) {
        uint8_t l = buf[p++];
        if (l == 0) {
            if (p + 4 > len) return -1;
            return p + 4;
        }
        if ((l & 0xC0) != 0 || l > 63 || p + l > len) return -1;
        p += l;
    }
    return -1;
}

static int build_dns_answer(uint8_t *out, int out_cap, const uint8_t *query, int qlen)
{
    int qend = find_question_end(query, qlen);
    if (qend < 0 || qend + 16 > out_cap || qend > qlen) return -1;

    memcpy(out, query, qend);
    out[2] = 0x81; out[3] = 0x80; /* standard response, no error */
    out[4] = 0x00; out[5] = 0x00; /* QDCOUNT = 0 in our response copy */
    out[6] = 0x00; out[7] = 0x01; /* ANCOUNT = 1 */
    out[8] = out[9] = out[10] = out[11] = 0;

    /* Keep the original question. DNS header QDCOUNT should be 1. */
    out[4] = 0x00; out[5] = 0x01;

    int p = qend;
    out[p++] = 0xC0; out[p++] = 0x0C; /* name pointer */
    out[p++] = 0x00; out[p++] = 0x01; /* A */
    out[p++] = 0x00; out[p++] = 0x01; /* IN */
    out[p++] = 0x00; out[p++] = 0x00; out[p++] = 0x00; out[p++] = 0x3C; /* TTL 60 */
    out[p++] = 0x00; out[p++] = 0x04;
    out[p++] = 192; out[p++] = 168; out[p++] = 4; out[p++] = 1;
    return p;
}

static void dns_task(void *arg)
{
    uint8_t rx[DNS_RX_SIZE];
    uint8_t tx[DNS_RX_SIZE + 32];

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed: errno=%d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "Local DNS started on UDP/53");

    while (1) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) continue;

        int out_len = build_dns_answer(tx, sizeof(tx), rx, n);
        if (out_len > 0) {
            sendto(sock, tx, out_len, 0, (struct sockaddr *)&from, from_len);
        }
    }
}

/* -------------------- HTTP helpers -------------------- */

static esp_err_t send_json(httpd_req_t *req, const char *json, const char *status)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t redirect_to_root(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, HTML_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    int s[3];
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    memcpy(s, relay_state, sizeof(s));
    xSemaphoreGive(relay_mutex);

    char json[128];
    snprintf(json, sizeof(json), "{\"relay1\":%d,\"relay2\":%d,\"relay3\":%d}", s[0], s[1], s[2]);
    return send_json(req, json, "200 OK");
}

static esp_err_t relay_handler(httpd_req_t *req)
{
    if (ota_in_progress) return send_json(req, "{\"error\":\"OTA in progress\"}", "409 Conflict");

    char query[128];
    char value[20];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return send_json(req, "{\"error\":\"missing query\"}", "400 Bad Request");

    if (httpd_query_key_value(query, "relay", value, sizeof(value)) != ESP_OK)
        return send_json(req, "{\"error\":\"relay\"}", "400 Bad Request");
    char *end = NULL;
    long relay = strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0' || relay < 1 || relay > 3)
        return send_json(req, "{\"error\":\"relay\"}", "400 Bad Request");

    if (httpd_query_key_value(query, "state", value, sizeof(value)) != ESP_OK)
        return send_json(req, "{\"error\":\"state\"}", "400 Bad Request");
    end = NULL;
    long state = strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0' || (state != 0 && state != 1))
        return send_json(req, "{\"error\":\"state\"}", "400 Bad Request");

    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    relay_state[relay - 1] = (int)state;
    gpio_num_t pins[3] = {RELAY1_GPIO, RELAY2_GPIO, RELAY3_GPIO};
    gpio_set_level(pins[relay - 1], relay_output_level((int)state));
    xSemaphoreGive(relay_mutex);

    /* Persist only actual user changes. */
    esp_err_t err = save_relay_states();
    if (err != ESP_OK) {
        return send_json(req, "{\"error\":\"state applied but not saved\"}", "500 Internal Server Error");
    }

    return send_json(req, "{\"ok\":true}", "200 OK");
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    char json[160];
    snprintf(json, sizeof(json), "{\"ssid\":\"%s\"}", ap_ssid);
    return send_json(req, json, "200 OK");
}

static bool json_extract_string(const char *body, const char *key, char *out, size_t out_sz)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(body, needle);
    if (!p) return false;
    p = strchr(p + strlen(needle), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\' && p[1]) return false; /* reject escapes for simplicity */
        out[i++] = *p++;
    }
    if (*p != '"') return false;
    out[i] = '\0';
    return true;
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (ota_in_progress) return send_json(req, "{\"error\":\"OTA in progress\"}", "409 Conflict");
    if (req->content_len <= 0 || req->content_len > 1024)
        return send_json(req, "{\"error\":\"invalid body\"}", "400 Bad Request");

    char body[1025];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += (size_t)n;
    }
    body[received] = '\0';

    char ssid[MAX_AP_SSID_LEN + 1];
    char pass[MAX_AP_PASS_LEN + 1];
    if (!json_extract_string(body, "ssid", ssid, sizeof(ssid)) ||
        !json_extract_string(body, "password", pass, sizeof(pass)) ||
        !valid_ssid(ssid) || !valid_password(pass)) {
        return send_json(req, "{\"error\":\"invalid SSID/password\"}", "400 Bad Request");
    }

    if (save_ap_settings(ssid, pass) != ESP_OK)
        return send_json(req, "{\"error\":\"save failed\"}", "500 Internal Server Error");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");

    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
    return ESP_OK;
}

/* -------------------- OTA -------------------- */

static esp_err_t ota_handler(httpd_req_t *req)
{
    if (ota_in_progress) return send_json(req, "{\"error\":\"OTA busy\"}", "409 Conflict");

    if (req->content_len < 1024)
        return send_json(req, "{\"error\":\"firmware too small\"}", "400 Bad Request");

    ota_in_progress = true;
    xSemaphoreTake(ota_mutex, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        err = ESP_FAIL;
        goto ota_fail;
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    if (err != ESP_OK) goto ota_fail;

    uint8_t *buf = malloc(OTA_BUFFER_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        err = ESP_ERR_NO_MEM;
        goto ota_fail;
    }

    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t want = remaining > OTA_BUFFER_SIZE ? OTA_BUFFER_SIZE : remaining;
        int n = httpd_req_recv(req, (char *)buf, want);
        if (n <= 0) {
            free(buf);
            esp_ota_abort(ota_handle);
            err = ESP_FAIL;
            goto ota_fail;
        }

        err = esp_ota_write(ota_handle, buf, n);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(ota_handle);
            goto ota_fail;
        }
        remaining -= (size_t)n;
    }

    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) goto ota_fail;

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) goto ota_fail;

    xSemaphoreGive(ota_mutex);
    ota_in_progress = false;

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "OTA successful. Restarting...");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK;

ota_fail:
    xSemaphoreGive(ota_mutex);
    ota_in_progress = false;

    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"error\":\"OTA failed\",\"code\":%d}", (int)err);
    return send_json(req, msg, "500 Internal Server Error");
}

/* Common captive portal probe paths. */
static esp_err_t captive_handler(httpd_req_t *req)
{
    return redirect_to_root(req);
}

/* -------------------- HTTP server -------------------- */

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 16;
    config.stack_size = 6144;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.lru_purge_enable = true;

    if (httpd_start(&http_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return;
    }

    httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=root_handler};
    httpd_uri_t status = {.uri="/api/status", .method=HTTP_GET, .handler=status_handler};
    httpd_uri_t relay = {.uri="/api/relay", .method=HTTP_GET, .handler=relay_handler};
    httpd_uri_t settings_get = {.uri="/api/settings", .method=HTTP_GET, .handler=settings_get_handler};
    httpd_uri_t settings_post = {.uri="/api/settings", .method=HTTP_POST, .handler=settings_post_handler};
    httpd_uri_t ota = {.uri="/api/ota", .method=HTTP_POST, .handler=ota_handler};

    httpd_uri_t c1 = {.uri="/generate_204", .method=HTTP_GET, .handler=captive_handler};
    httpd_uri_t c2 = {.uri="/hotspot-detect.html", .method=HTTP_GET, .handler=captive_handler};
    httpd_uri_t c3 = {.uri="/connecttest.txt", .method=HTTP_GET, .handler=captive_handler};
    httpd_uri_t c4 = {.uri="/ncsi.txt", .method=HTTP_GET, .handler=captive_handler};
    httpd_uri_t c5 = {.uri="/connectivitycheck.gstatic.com/generate_204", .method=HTTP_GET, .handler=captive_handler};
    httpd_uri_t c6 = {.uri="/success.txt", .method=HTTP_GET, .handler=captive_handler};

    httpd_register_uri_handler(http_server, &root);
    httpd_register_uri_handler(http_server, &status);
    httpd_register_uri_handler(http_server, &relay);
    httpd_register_uri_handler(http_server, &settings_get);
    httpd_register_uri_handler(http_server, &settings_post);
    httpd_register_uri_handler(http_server, &ota);
    httpd_register_uri_handler(http_server, &c1);
    httpd_register_uri_handler(http_server, &c2);
    httpd_register_uri_handler(http_server, &c3);
    httpd_register_uri_handler(http_server, &c4);
    httpd_register_uri_handler(http_server, &c5);
    httpd_register_uri_handler(http_server, &c6);

    ESP_LOGI(TAG, "HTTP server ready");
}

/* -------------------- Watchdog -------------------- */

static void watchdog_keepalive_task(void *arg)
{
    /* This task is intentionally simple. Idle-task TWDT monitoring is also
       enabled by the configuration below; this task protects its own loop. */
    esp_task_wdt_add(NULL);

    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* -------------------- app_main -------------------- */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    relay_mutex = xSemaphoreCreateMutex();
    storage_mutex = xSemaphoreCreateMutex();
    ota_mutex = xSemaphoreCreateMutex();
    if (!relay_mutex || !storage_mutex || !ota_mutex) {
        ESP_LOGE(TAG, "Mutex allocation failed");
        abort();
    }

    load_nvs();
    init_relays();
    apply_all_relays();

    /* Configure TWDT with idle-task monitoring. */
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WATCHDOG_TIMEOUT_MS,
        .idle_core_mask = (1U << portNUM_PROCESSORS) - 1U,
        .trigger_panic = true
    };
    ret = esp_task_wdt_init(&wdt_config);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    xTaskCreate(watchdog_keepalive_task, "wdt_keepalive", 2048, NULL, 1, NULL);

    wifi_init_ap();

    BaseType_t ok = xTaskCreate(dns_task, "local_dns", DNS_STACK_SIZE, NULL, 3, &dns_task_handle);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "DNS task creation failed");
    }

    start_http_server();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Offline Smart Home ready");
    ESP_LOGI(TAG, "Control:  http://%s/", AP_IP_ADDR);
    ESP_LOGI(TAG, "AP only: no STA, no Internet");
    ESP_LOGI(TAG, "========================================");

    /* Keep app_main alive as a lightweight supervisor. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

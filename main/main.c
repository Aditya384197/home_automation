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
#define RELAY4_GPIO             19
#define RELAY5_GPIO             21

#define RELAY_COUNT             5

/* Physical wall-switch inputs: connect one switch terminal to GND and the
 * other terminal to the corresponding GPIO. Internal pull-ups are used, so
 * an open switch reads HIGH and a closed switch reads LOW. */
#define SWITCH1_GPIO            32
#define SWITCH2_GPIO            33
#define SWITCH3_GPIO            25
#define SWITCH4_GPIO            26
#define SWITCH5_GPIO            27
#define SWITCH_COUNT            5
#define SWITCH_ACTIVE_LEVEL     0
#define SWITCH_DEBOUNCE_SAMPLES 3
#define SWITCH_POLL_MS          20

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
#define NVS_KEY_RELAY_ENABLED   "renable"
#define NVS_KEY_RELAY_NAMES     "rnames"
#define NVS_KEY_AP_SSID         "ap_ssid"
#define NVS_KEY_AP_PASS         "ap_pass"

#define WATCHDOG_TIMEOUT_MS     10000
#define DNS_PORT                53
#define DNS_STACK_SIZE          3072
#define DNS_RX_SIZE             512
#define OTA_BUFFER_SIZE         4096

#define MAX_AP_SSID_LEN         32
#define MAX_AP_PASS_LEN         63
#define MAX_RELAY_NAME_LEN      31
#define OTA_UPDATE_PASSWORD     "OTA@ESP32#2026"

/* ------------------------------------------------------------- */

static int relay_state[RELAY_COUNT] = {0, 0, 0, 0, 0};
static bool relay_enabled[RELAY_COUNT] = {true, true, true, false, false};
static char relay_name[RELAY_COUNT][MAX_RELAY_NAME_LEN + 1] = {
    "Living Room Light",
    "Ceiling Fan",
    "Charging Socket",
    "Relay 4",
    "Relay 5"
};

static SemaphoreHandle_t relay_mutex;
static SemaphoreHandle_t storage_mutex;
static SemaphoreHandle_t ota_mutex;

static char ap_ssid[MAX_AP_SSID_LEN + 1] = DEFAULT_AP_SSID;
static char ap_password[MAX_AP_PASS_LEN + 1] = DEFAULT_AP_PASSWORD;

static TaskHandle_t dns_task_handle = NULL;
static TaskHandle_t switch_task_handle = NULL;
static volatile bool ota_in_progress = false;
static httpd_handle_t http_server = NULL;

/* -------------------- Local Web UI -------------------- */

static const char *HTML_PAGE =
"<!doctype html><html lang='en'><head>\n<meta charset='utf-8'>\n<meta name='viewport' content='width=device-width,initial-s"
"cale=1,viewport-fit=cover'>\n<meta name='theme-color' content='#111827'>\n<title>ESP32 Smart Home</title>\n<style>\n:root{--"
"bg:#f3f5f7;--card:#fff;--text:#17202a;--muted:#697586;--line:#e5e7eb;--on:#168a4b;--off:#9aa3ad;--accent:#2563eb;--dange"
"r:#b42318}\n*{box-sizing:border-box}\nhtml,body{margin:0;min-height:100%;background:var(--bg);color:var(--text);font-famil"
"y:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif}\nbody{overflow-x:hidden}\n.wrap{width:min(680px,100%);margin:a"
"uto;padding:18px 14px 34px}\n.top{padding:8px 4px 18px}\n.topbar{display:flex;align-items:center;justify-content:space-bet"
"ween;gap:12px}\nh1{font-size:25px;margin:0 0 5px}.sub{color:var(--muted);font-size:14px}\n.settings-btn,.icon-btn{width:42"
"px;height:42px;border:1px solid var(--line);border-radius:12px;background:#fff;display:flex;align-items:center;justify-c"
"ontent:center;font-size:21px;cursor:pointer;box-shadow:0 2px 8px rgba(15,23,42,.06)}\n.settings-btn:active,.icon-btn:acti"
"ve{transform:scale(.96)}\n.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;margi"
"n:12px 0;box-shadow:0 2px 10px rgba(15,23,42,.04)}\n.row{display:flex;align-items:center;justify-content:space-between;ga"
"p:15px}\n.name{font-weight:650;font-size:17px}.state{font-size:13px;color:var(--muted);margin-top:4px}\n.switch{position:r"
"elative;width:58px;height:32px;flex:none}.switch input{opacity:0;width:0;height:0}\n.slider{position:absolute;inset:0;bac"
"kground:#c8ced5;border-radius:40px;transition:.18s;cursor:pointer}\n.slider:before{content:'';position:absolute;width:26p"
"x;height:26px;left:3px;top:3px;background:white;border-radius:50%;box-shadow:0 1px 4px #0003;transition:.18s}\ninput:chec"
"ked+.slider{background:var(--on)}input:checked+.slider:before{transform:translateX(26px)}\nbutton{border:1px solid var(--"
"line);background:#fff;border-radius:10px;padding:10px 13px;font:inherit;cursor:pointer}\nbutton.primary{background:var(--"
"accent);border-color:var(--accent);color:#fff}button:disabled{opacity:.55;cursor:not-allowed}\n.msg{font-size:13px;margin"
"-top:10px;color:var(--muted)}\ninput[type=text],input[type=password],input[type=file]{width:100%;padding:11px;border:1px "
"solid #d5dae0;border-radius:10px;background:#fff;font:inherit}\nlabel.field{display:block;font-size:13px;color:var(--mute"
"d);margin:13px 0 6px}\n.hidden{display:none!important}\n.status{display:inline-flex;align-items:center;gap:7px;font-size:1"
"2px;color:var(--muted)}.dot{width:8px;height:8px;border-radius:50%;background:var(--on)}\n.progress-wrap{margin-top:14px}"
".progress-head{display:flex;justify-content:space-between;gap:10px;font-size:12px;color:var(--muted);margin-bottom:6px}\n"
".progress{height:8px;background:#e8ebef;border-radius:20px;overflow:hidden}.progress-fill{height:100%;width:0;background"
":var(--accent);transition:width:.12s ease}\n.bar{display:flex;gap:8px;flex-wrap:wrap;margin-top:14px}.setting-list{margin"
"-top:12px}.setting-item{display:flex;align-items:center;justify-content:space-between;gap:14px;padding:16px 2px;border-t"
"op:1px solid var(--line);cursor:pointer}\n.setting-item:first-child{border-top:0}.setting-item:active{opacity:.72}\n.setti"
"ng-title{font-weight:650;font-size:16px}.setting-desc{font-size:12px;color:var(--muted);margin-top:3px}\n.chevron{font-si"
"ze:25px;color:var(--muted);line-height:1}\n.panel{position:fixed;inset:0;background:var(--bg);z-index:1000;overflow-y:aut"
"o;transform:translateX(100%);transition:transform .28s cubic-bezier(.22,.61,.36,1);visibility:hidden}\n.panel.open{transf"
"orm:translateX(0);visibility:visible}\n.panel-inner{width:min(680px,100%);min-height:100%;margin:auto;padding:18px 14px 3"
"4px}\n.panel-header{display:flex;align-items:center;gap:12px;padding:4px 0 16px;position:sticky;top:0;background:var(--bg"
");z-index:2}\n.panel-header h2{font-size:21px;margin:0;flex:1}.back-btn{font-size:20px;padding:8px 12px}\n.close-btn{font-"
"size:22px;padding:8px 12px}\n.subpage{position:fixed;inset:0;background:var(--bg);z-index:1100;overflow-y:auto;transform:"
"translateX(100%);transition:transform .25s cubic-bezier(.22,.61,.36,1);visibility:hidden}\n.subpage.open{transform:transl"
"ateX(0);visibility:visible}\n.subpage-inner{width:min(680px,100%);min-height:100%;margin:auto;padding:18px 14px 34px}\n.su"
"bpage-header{display:flex;align-items:center;gap:12px;padding:4px 0 16px;position:sticky;top:0;background:var(--bg);z-in"
"dex:2}\n.subpage-header h2{font-size:21px;margin:0;flex:1}\n.back-wide{width:100%;margin-top:22px}\n.relay-config{margin-to"
"p:4px}.relay-config-item{padding:14px 0;border-top:1px solid var(--line)}.relay-config-item:first-child{border-top:0}\n.r"
"elay-config-head{display:flex;align-items:center;justify-content:space-between;gap:12px}\n.small-switch{position:relative"
";width:48px;height:27px;flex:none}.small-switch input{opacity:0;width:0;height:0}\n.small-slider{position:absolute;inset:"
"0;background:#c8ced5;border-radius:40px;transition:.18s;cursor:pointer}\n.small-slider:before{content:'';position:absolut"
"e;width:21px;height:21px;left:3px;top:3px;background:#fff;border-radius:50%;box-shadow:0 1px 4px #0003;transition:.18s}\n"
".small-switch input:checked+.small-slider{background:var(--on)}.small-switch input:checked+.small-slider:before{transfor"
"m:translateX(21px)}\n.relay-number{font-weight:650;font-size:15px}.relay-gpio{font-size:12px;color:var(--muted);margin-to"
"p:3px}.relay-switch-gpio{font-size:12px;color:var(--muted);margin-top:2px}\n</style></head><body>\n<main class='wrap'><hea"
"der class='top'><div class='topbar'>\n<div><h1>Smart Home</h1><div class='sub'>Local offline control</div></div>\n<button "
"class='settings-btn' onclick='openSettings()' aria-label='Settings' title='Settings'>&#9881;</button>\n</div></header><se"
"ction id='controls'></section>\n<div class='status'><span class='dot'></span> ESP32 local AP</div></main>\n\n<section id='s"
"ettingsMenu' class='panel' aria-hidden='true'>\n<div class='panel-inner'><header class='panel-header'>\n<div><h2>Settings<"
"/h2><div class='sub'>Device configuration</div></div>\n<button class='icon-btn close-btn' onclick='closeSettings()' aria-"
"label='Close settings' title='Close'>&#10005;</button>\n</header>\n<div class='card setting-list'>\n<div class='setting-ite"
"m' onclick='openSubPage(\"otaPage\")' role='button' tabindex='0'><div><div class='setting-title'>OTA Update</div><div clas"
"s='setting-desc'>Update firmware locally from a .bin file</div></div><div class='chevron'>&#8250;</div></div>\n<div class"
"='setting-item' onclick='openSubPage(\"relayPage\")' role='button' tabindex='0'><div><div class='setting-title'>Relay Conf"
"iguration</div><div class='setting-desc'>Enable Relay 4/5 and rename any relay</div></div><div class='chevron'>&#8250;</"
"div></div>\n<div class='setting-item' onclick='openSubPage(\"apPage\")' role='button' tabindex='0'><div><div class='setting"
"-title'>AP Configuration</div><div class='setting-desc'>Change the ESP32 local Wi-Fi SSID and password</div></div><div c"
"lass='chevron'>&#8250;</div></div>\n</div></div>\n</section>\n\n<section id='otaPage' class='subpage' aria-hidden='true'><di"
"v class='subpage-inner'>\n<header class='subpage-header'><button class='icon-btn back-btn' onclick='backToSettings()' ari"
"a-label='Back'>&#8592;</button><h2>OTA Update</h2></header>\n<div class='card'>\n<div class='state'>Update firmware locall"
"y from a .bin file.</div>\n<label class='field'>Firmware .bin</label><input id='fw' type='file' accept='.bin,application/"
"octet-stream'>\n<div class='bar'><button id='uploadBtn' class='primary' onclick='uploadFirmware()'>Upload &amp; Restart</"
"button></div>\n<div id='otaProgress' class='progress-wrap hidden'><div class='progress-head'><span id='otaProgressText'>U"
"ploading...</span><span id='otaPercent'>0%</span></div><div class='progress'><div id='otaFill' class='progress-fill'></d"
"iv></div></div>\n<div id='otamsg' class='msg'></div>\n</div><button class='back-wide' onclick='backToSettings()'>&#8592; B"
"ack to Settings</button>\n</div></section>\n\n<section id='relayPage' class='subpage' aria-hidden='true'><div class='subpag"
"e-inner'>\n<header class='subpage-header'><button class='icon-btn back-btn' onclick='backToSettings()' aria-label='Back'>"
"&#8592;</button><h2>Relay Configuration</h2></header>\n<div class='card'><div class='state'>Relay 1-3 are always availabl"
"e. Relay 4-5 are optional.</div><div id='relayConfigList' class='relay-config'></div>\n<div class='bar'><button class='pr"
"imary' onclick='saveRelayConfig()'>Save Relay Configuration</button></div><div id='relaymsg' class='msg'></div></div>\n<b"
"utton class='back-wide' onclick='backToSettings()'>&#8592; Back to Settings</button>\n</div></section>\n\n<section id='apPa"
"ge' class='subpage' aria-hidden='true'><div class='subpage-inner'>\n<header class='subpage-header'><button class='icon-bt"
"n back-btn' onclick='backToSettings()' aria-label='Back'>&#8592;</button><h2>AP Configuration</h2></header>\n<div class='"
"card'><div class='state'>Configure the ESP32 local Wi-Fi network.</div>\n<label class='field'>SSID</label><input id='ssid"
"' maxlength='32'>\n<label class='field'>Password (8-63 characters)</label><input id='pass' type='password' maxlength='63'"
">\n<div class='bar'><button class='primary' onclick='saveSettings()'>Save &amp; Restart</button></div><div id='setmsg' cl"
"ass='msg'></div>\n</div><button class='back-wide' onclick='backToSettings()'>&#8592; Back to Settings</button>\n</div></se"
"ction>\n\n<script>\nlet relayCfg=[{enabled:true,name:'Living Room Light',gpio:16,switchGpio:32},{enabled:true,name:'Ceiling"
" Fan',gpio:17,switchGpio:33},{enabled:true,name:'Charging Socket',gpio:18,switchGpio:25},{enabled:false,name:'Relay 4',g"
"pio:19,switchGpio:26},{enabled:false,name:'Relay 5',gpio:21,switchGpio:27}];\n\nfunction esc(s){return String(s).replace(/"
"[&<>'\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',\"'\":'&#39;','\"':'&quot;'}[c]))}\nfunction render(a){let h='';a.forEach(("
"v,i)=>{if(!relayCfg[i]||!relayCfg[i].enabled)return;h+=`<section class='card'><div class='row'><div><div class='name'>${"
"esc(relayCfg[i].name)}</div><div class='state' id='st${i}'>${v?'ON':'OFF'}</div></div><label class='switch'><input type="
"'checkbox' id='r${i}' ${v?'checked':''} onchange='setRelay(${i},this.checked)'><span class='slider'></span></label></div"
"></section>`});document.getElementById('controls').innerHTML=h}\nasync function load(){try{let r=await fetch('/api/status"
"',{cache:'no-store'});if(!r.ok)throw 0;let d=await r.json();relayCfg=d.config||relayCfg;render(d.states||[])}catch(e){}}"
"\nasync function setRelay(i,on){let el=document.getElementById('r'+i);if(!el)return;el.disabled=true;try{let r=await fetc"
"h(`/api/relay?relay=${i+1}&state=${on?1:0}`,{cache:'no-store'});if(!r.ok)throw 0;await load()}catch(e){el.checked=!on;al"
"ert('Relay command failed.')}finally{el.disabled=false}}\n\nfunction openSettings(){let p=document.getElementById('setting"
"sMenu');p.classList.add('open');p.setAttribute('aria-hidden','false');document.body.style.overflow='hidden'}\nfunction cl"
"oseSettings(){closeSubPages();let p=document.getElementById('settingsMenu');p.classList.remove('open');p.setAttribute('a"
"ria-hidden','true');document.body.style.overflow=''}\nfunction openSubPage(id){closeSubPages();let p=document.getElementB"
"yId(id);if(!p)return;p.classList.add('open');p.setAttribute('aria-hidden','false');if(id==='relayPage'){document.getElem"
"entById('relaymsg').textContent='';renderRelayConfig()}if(id==='apPage')loadSettings()}\nfunction closeSubPages(){documen"
"t.querySelectorAll('.subpage').forEach(p=>{p.classList.remove('open');p.setAttribute('aria-hidden','true')})}\nfunction b"
"ackToSettings(){closeSubPages();let s=document.getElementById('settingsMenu');s.classList.add('open');s.setAttribute('ar"
"ia-hidden','false')}\n\ndocument.querySelectorAll('.setting-item[role=button]').forEach(el=>el.addEventListener('keydown',"
"e=>{if(e.key==='Enter'||e.key===' '){e.preventDefault();el.click()}}));\n\nfunction renderRelayConfig(){let h='';relayCfg."
"forEach((r,i)=>{let optional=i>=3;h+=`<div class='relay-config-item'><div class='relay-config-head'><div><div class='rel"
"ay-number'>Relay ${i+1}</div><div class='relay-gpio'>Relay GPIO ${r.gpio}${optional?' · Optional':''}</div><div class='r"
"elay-switch-gpio'>Physical Switch GPIO ${r.switchGpio}</div></div>${optional?`<label class='small-switch'><input type='c"
"heckbox' id='en${i}' ${r.enabled?'checked':''} onchange='relayEnableChanged(${i})'><span class='small-slider'></span></l"
"abel>`:''}</div><label class='field'>Name</label><input type='text' id='rn${i}' maxlength='31' value='${esc(r.name)}' ${"
"optional&&!r.enabled?'disabled':''}></div>`});document.getElementById('relayConfigList').innerHTML=h}\nfunction relayEnab"
"leChanged(i){let en=document.getElementById('en'+i).checked;document.getElementById('rn'+i).disabled=!en}\nasync function"
" saveRelayConfig(){let m=document.getElementById('relaymsg');let body={};for(let i=0;i<5;i++){let enabled=i<3?true:docum"
"ent.getElementById('en'+i).checked;let name=document.getElementById('rn'+i).value.trim();if(!name)name='Relay '+(i+1);if"
"(name.length>31){m.textContent='Relay name is too long.';return}body['r'+(i+1)+'_enabled']=enabled;body['r'+(i+1)+'_name"
"']=name}m.textContent='Saving...';try{let r=await fetch('/api/relays',{method:'POST',headers:{'Content-Type':'applicatio"
"n/json'},body:JSON.stringify(body)});let d=await r.json().catch(()=>({}));if(!r.ok)throw new Error(d.error||'save failed"
"');relayCfg=d.config||relayCfg;m.textContent='Saved successfully.';renderRelayConfig();await load()}catch(e){m.textConte"
"nt='Could not save relay configuration: '+(e.message||'request failed')}}\nasync function saveSettings(){let s=document.g"
"etElementById('ssid').value,p=document.getElementById('pass').value,m=document.getElementById('setmsg');if(s.length<1||s"
".length>32||p.length<8||p.length>63){m.textContent='Invalid SSID or password.';return}m.textContent='Saving and restarti"
"ng...';try{let r=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.string"
"ify({ssid:s,password:p})});if(!r.ok)throw 0}catch(e){m.textContent='Connection lost. The AP may be restarting.'}}\nasync "
"function loadSettings(){try{let r=await fetch('/api/settings',{cache:'no-store'}),d=await r.json();document.getElementBy"
"Id('ssid').value=d.ssid||''}catch(e){}}\n\nfunction setOtaProgress(p){p=Math.max(0,Math.min(100,p));document.getElementByI"
"d('otaProgress').classList.remove('hidden');document.getElementById('otaFill').style.width=p+'%';document.getElementById"
"('otaPercent').textContent=Math.round(p)+'%'}\nfunction uploadFirmware(){let f=document.getElementById('fw').files[0],m=d"
"ocument.getElementById('otamsg'),btn=document.getElementById('uploadBtn');if(!f){m.textContent='Select a .bin file first"
".';return}if(f.size<1024){m.textContent='Firmware file is too small.';return}if(!confirm('Start OTA update? The device w"
"ill restart after a successful update.'))return;let otaPassword=prompt('Enter OTA update password:');if(otaPassword===nu"
"ll)return;if(!otaPassword){m.textContent='OTA password is required.';return}btn.disabled=true;m.textContent='Uploading.."
". Do not disconnect.';setOtaProgress(0);let xhr=new XMLHttpRequest();xhr.open('POST','/api/ota',true);xhr.setRequestHead"
"er('Content-Type','application/octet-stream');xhr.setRequestHeader('X-OTA-Password',otaPassword);xhr.upload.onprogress=f"
"unction(e){if(e.lengthComputable){setOtaProgress((e.loaded/e.total)*100);m.textContent='Uploading firmware...'}};xhr.onl"
"oad=function(){if(xhr.status>=200&&xhr.status<300){setOtaProgress(100);m.textContent=xhr.responseText||'OTA successful. "
"Restarting...';setTimeout(()=>location.reload(),8000)}else{btn.disabled=false;m.textContent='OTA failed. Current firmwar"
"e should remain active.'}};xhr.onerror=function(){if(document.getElementById('otaPercent').textContent==='100%'){m.textC"
"ontent='Firmware uploaded. Device may be restarting...'}else{btn.disabled=false;m.textContent='Upload interrupted. Curre"
"nt firmware should remain active.'}};xhr.ontimeout=function(){btn.disabled=false;m.textContent='OTA request timed out.'}"
";xhr.send(f)}\nload();loadSettings();setInterval(load,500);\n</script></body></html>";
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

static bool valid_relay_name(const char *s)
{
    size_t n = strnlen(s, MAX_RELAY_NAME_LEN + 1);
    if (n < 1 || n > MAX_RELAY_NAME_LEN) return false;

    /* Reject ASCII control characters while still allowing UTF-8 names. */
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

static void load_defaults(void)
{
    strlcpy(ap_ssid, DEFAULT_AP_SSID, sizeof(ap_ssid));
    strlcpy(ap_password, DEFAULT_AP_PASSWORD, sizeof(ap_password));

    for (int i = 0; i < RELAY_COUNT; ++i) {
        relay_state[i] = 0;
        relay_enabled[i] = (i < 3);
    }

    strlcpy(relay_name[0], "Living Room Light", sizeof(relay_name[0]));
    strlcpy(relay_name[1], "Ceiling Fan", sizeof(relay_name[1]));
    strlcpy(relay_name[2], "Charging Socket", sizeof(relay_name[2]));
    strlcpy(relay_name[3], "Relay 4", sizeof(relay_name[3]));
    strlcpy(relay_name[4], "Relay 5", sizeof(relay_name[4]));
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

    uint8_t states[RELAY_COUNT] = {0};
    size_t sz = sizeof(states);
    if (nvs_get_blob(h, NVS_KEY_RELAY_STATES, states, &sz) == ESP_OK && sz == sizeof(states)) {
        for (int i = 0; i < RELAY_COUNT; ++i) relay_state[i] = states[i] ? 1 : 0;
    }

    uint8_t enabled[RELAY_COUNT] = {1, 1, 1, 0, 0};
    sz = sizeof(enabled);
    if (nvs_get_blob(h, NVS_KEY_RELAY_ENABLED, enabled, &sz) == ESP_OK && sz == sizeof(enabled)) {
        for (int i = 0; i < RELAY_COUNT; ++i) {
            relay_enabled[i] = (i < 3) ? true : (enabled[i] != 0);
        }
    }

    size_t names_sz = sizeof(relay_name);
    if (nvs_get_blob(h, NVS_KEY_RELAY_NAMES, relay_name, &names_sz) == ESP_OK &&
        names_sz == sizeof(relay_name)) {
        for (int i = 0; i < RELAY_COUNT; ++i) {
            relay_name[i][MAX_RELAY_NAME_LEN] = '\0';
            if (!valid_relay_name(relay_name[i])) {
                if (i == 0) strlcpy(relay_name[i], "Living Room Light", sizeof(relay_name[i]));
                else if (i == 1) strlcpy(relay_name[i], "Ceiling Fan", sizeof(relay_name[i]));
                else if (i == 2) strlcpy(relay_name[i], "Charging Socket", sizeof(relay_name[i]));
                else {
                    snprintf(relay_name[i], sizeof(relay_name[i]), "Relay %d", i + 1);
                }
            }
        }
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
    ESP_LOGI(TAG, "Restored relay states: %d %d %d %d %d",
             relay_state[0], relay_state[1], relay_state[2], relay_state[3], relay_state[4]);
    ESP_LOGI(TAG, "Relay enabled: %d %d %d %d %d",
             relay_enabled[0], relay_enabled[1], relay_enabled[2], relay_enabled[3], relay_enabled[4]);
}

static esp_err_t save_relay_states(void)
{
    uint8_t states[RELAY_COUNT];
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    for (int i = 0; i < RELAY_COUNT; ++i) states[i] = relay_state[i] ? 1 : 0;
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

static esp_err_t save_relay_config(void)
{
    uint8_t enabled[RELAY_COUNT];
    uint8_t states[RELAY_COUNT];
    char names[RELAY_COUNT][MAX_RELAY_NAME_LEN + 1];

    /* Serialize the complete relay configuration/state snapshot. The relay
     * mutex stays held until the NVS transaction has committed, so a physical
     * switch or web command cannot change relay_state between the snapshot and
     * the configuration write. No other path takes storage_mutex and then
     * relay_mutex, so this lock order is safe. */
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    for (int i = 0; i < RELAY_COUNT; ++i) {
        enabled[i] = relay_enabled[i] ? 1 : 0;
        states[i] = relay_state[i] ? 1 : 0;
    }
    memcpy(names, relay_name, sizeof(names));

    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_RELAY_ENABLED, enabled, sizeof(enabled));
        if (err == ESP_OK) err = nvs_set_blob(h, NVS_KEY_RELAY_NAMES, names, sizeof(names));
        if (err == ESP_OK) err = nvs_set_blob(h, NVS_KEY_RELAY_STATES, states, sizeof(states));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(storage_mutex);
    xSemaphoreGive(relay_mutex);

    if (err != ESP_OK) ESP_LOGE(TAG, "Relay config NVS save failed: %s", esp_err_to_name(err));
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

static gpio_num_t relay_gpio(int index)
{
    static const gpio_num_t pins[RELAY_COUNT] = {
        RELAY1_GPIO, RELAY2_GPIO, RELAY3_GPIO, RELAY4_GPIO, RELAY5_GPIO
    };
    return pins[index];
}

static void apply_all_relays(void)
{
    int s[RELAY_COUNT];
    bool enabled[RELAY_COUNT];

    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    memcpy(s, relay_state, sizeof(s));
    memcpy(enabled, relay_enabled, sizeof(enabled));
    xSemaphoreGive(relay_mutex);

    for (int i = 0; i < RELAY_COUNT; ++i) {
        gpio_set_level(relay_gpio(i),
                       (enabled[i] && s[i]) ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL);
    }
}

static void init_relays(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < RELAY_COUNT; ++i) mask |= (1ULL << relay_gpio(i));

    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* Safe physical OFF before restoring persistent state. */
    for (int i = 0; i < RELAY_COUNT; ++i) {
        gpio_set_level(relay_gpio(i), relay_output_level(0));
    }
}

/* -------------------- Physical wall switches -------------------- */

static gpio_num_t switch_gpio(int index)
{
    static const gpio_num_t pins[SWITCH_COUNT] = {
        SWITCH1_GPIO, SWITCH2_GPIO, SWITCH3_GPIO, SWITCH4_GPIO, SWITCH5_GPIO
    };
    return pins[index];
}

static bool read_switch_state(int index)
{
    return gpio_get_level(switch_gpio(index)) == SWITCH_ACTIVE_LEVEL;
}

static void init_switches(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < SWITCH_COUNT; ++i) mask |= (1ULL << switch_gpio(i));

    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

static void apply_switch_command(int index, bool on)
{
    bool changed = false;

    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    if (relay_enabled[index] && relay_state[index] != (int)on) {
        relay_state[index] = on ? 1 : 0;
        gpio_set_level(relay_gpio(index), relay_output_level(on ? 1 : 0));
        changed = true;
    }
    xSemaphoreGive(relay_mutex);

    if (changed) {
        /* Persist physical-switch changes so the last known state survives a
         * power cycle. NVS handles wear-leveling internally. */
        if (save_relay_states() != ESP_OK) {
            ESP_LOGE(TAG, "Physical switch state save failed for relay %d", index + 1);
        }
    }
}

static void physical_switch_task(void *arg)
{
    int last_raw[SWITCH_COUNT];
    int stable[SWITCH_COUNT];
    uint8_t samples[SWITCH_COUNT] = {0};

    esp_task_wdt_add(NULL);

    for (int i = 0; i < SWITCH_COUNT; ++i) {
        last_raw[i] = gpio_get_level(switch_gpio(i));
        /* Establish a boot baseline without generating a relay command.
         * This preserves the NVS-restored relay state across power cycles.
         * A later physical transition is what changes the relay. */
        stable[i] = last_raw[i];
        samples[i] = SWITCH_DEBOUNCE_SAMPLES;
    }

    while (1) {
        for (int i = 0; i < SWITCH_COUNT; ++i) {
            int raw = gpio_get_level(switch_gpio(i));

            if (raw == last_raw[i]) {
                if (samples[i] < SWITCH_DEBOUNCE_SAMPLES) samples[i]++;
            } else {
                last_raw[i] = raw;
                samples[i] = 0;
            }

            if (samples[i] >= SWITCH_DEBOUNCE_SAMPLES && stable[i] != raw) {
                stable[i] = raw;
                apply_switch_command(i, raw == SWITCH_ACTIVE_LEVEL);
                ESP_LOGI(TAG, "Physical switch %d -> %s", i + 1,
                         (raw == SWITCH_ACTIVE_LEVEL) ? "ON" : "OFF");
            }
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(SWITCH_POLL_MS));
    }
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
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(AP_IP_ADDR, &ip_info.ip));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(AP_GW_ADDR, &ip_info.gw));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(AP_NETMASK, &ip_info.netmask));
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
    out[2] = 0x81; out[3] = 0x80;
    out[4] = 0x00; out[5] = 0x01;
    out[6] = 0x00; out[7] = 0x01;
    out[8] = out[9] = out[10] = out[11] = 0;

    int p = qend;
    out[p++] = 0xC0; out[p++] = 0x0C;
    out[p++] = 0x00; out[p++] = 0x01;
    out[p++] = 0x00; out[p++] = 0x01;
    out[p++] = 0x00; out[p++] = 0x00; out[p++] = 0x00; out[p++] = 0x3C;
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
    esp_task_wdt_add(NULL);

    while (1) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            esp_task_wdt_reset();
            continue;
        }

        int out_len = build_dns_answer(tx, sizeof(tx), rx, n);
        if (out_len > 0) {
            sendto(sock, tx, out_len, 0, (struct sockaddr *)&from, from_len);
        }
        esp_task_wdt_reset();
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
    int s[RELAY_COUNT];
    bool enabled[RELAY_COUNT];
    char names[RELAY_COUNT][MAX_RELAY_NAME_LEN + 1];

    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    memcpy(s, relay_state, sizeof(s));
    memcpy(enabled, relay_enabled, sizeof(enabled));
    memcpy(names, relay_name, sizeof(names));
    xSemaphoreGive(relay_mutex);

    char json[1024];
    int pos = snprintf(json, sizeof(json), "{\"states\":[");
    for (int i = 0; i < RELAY_COUNT; ++i) {
        pos += snprintf(json + pos, sizeof(json) - pos, "%d%s", s[i], i == RELAY_COUNT - 1 ? "" : ",");
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "],\"config\":[");
    for (int i = 0; i < RELAY_COUNT; ++i) {
        pos += snprintf(json + pos, sizeof(json) - pos,
                        "{\"enabled\":%s,\"name\":\"%s\",\"gpio\":%d,\"switchGpio\":%d}%s",
                        enabled[i] ? "true" : "false",
                        names[i],
                        (int)relay_gpio(i),
                        (int)switch_gpio(i),
                        i == RELAY_COUNT - 1 ? "" : ",");
    }
    snprintf(json + pos, sizeof(json) - pos, "]}");

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
    if (*value == '\0' || *end != '\0' || relay < 1 || relay > RELAY_COUNT)
        return send_json(req, "{\"error\":\"relay\"}", "400 Bad Request");

    if (httpd_query_key_value(query, "state", value, sizeof(value)) != ESP_OK)
        return send_json(req, "{\"error\":\"state\"}", "400 Bad Request");

    end = NULL;
    long state = strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0' || (state != 0 && state != 1))
        return send_json(req, "{\"error\":\"state\"}", "400 Bad Request");

    int idx = (int)relay - 1;

    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    if (!relay_enabled[idx]) {
        xSemaphoreGive(relay_mutex);
        return send_json(req, "{\"error\":\"relay disabled\"}", "409 Conflict");
    }

    relay_state[idx] = (int)state;
    gpio_set_level(relay_gpio(idx), relay_output_level((int)state));
    xSemaphoreGive(relay_mutex);

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
        if (*p == '\\' && p[1]) return false;
        out[i++] = *p++;
    }
    if (*p != '"') return false;
    out[i] = '\0';
    return true;
}

static bool json_extract_bool(const char *body, const char *key, bool *out)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(body, needle);
    if (!p) return false;
    p = strchr(p + strlen(needle), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
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

static esp_err_t relay_config_post_handler(httpd_req_t *req)
{
    if (ota_in_progress) return send_json(req, "{\"error\":\"OTA in progress\"}", "409 Conflict");
    if (req->content_len <= 0 || req->content_len > 2048)
        return send_json(req, "{\"error\":\"invalid body\"}", "400 Bad Request");

    char body[2049];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += (size_t)n;
    }
    body[received] = '\0';

    bool new_enabled[RELAY_COUNT];
    char new_names[RELAY_COUNT][MAX_RELAY_NAME_LEN + 1];

    for (int i = 0; i < RELAY_COUNT; ++i) {
        char key[16];

        if (i < 3) {
            new_enabled[i] = true;
        } else {
            snprintf(key, sizeof(key), "r%d_enabled", i + 1);
            if (!json_extract_bool(body, key, &new_enabled[i])) {
                return send_json(req, "{\"error\":\"invalid relay enable state\"}", "400 Bad Request");
            }
        }

        snprintf(key, sizeof(key), "r%d_name", i + 1);
        if (!json_extract_string(body, key, new_names[i], sizeof(new_names[i])) ||
            !valid_relay_name(new_names[i])) {
            return send_json(req, "{\"error\":\"invalid relay name\"}", "400 Bad Request");
        }
    }

    bool old_enabled[RELAY_COUNT];
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    memcpy(old_enabled, relay_enabled, sizeof(old_enabled));
    for (int i = 0; i < RELAY_COUNT; ++i) {
        relay_enabled[i] = new_enabled[i];
        strlcpy(relay_name[i], new_names[i], sizeof(relay_name[i]));

        if (!relay_enabled[i]) {
            relay_state[i] = 0;
            gpio_set_level(relay_gpio(i), relay_output_level(0));
        } else if (i >= 3 && !old_enabled[i]) {
            /* When optional Relay 4/5 is enabled, immediately adopt the
             * current corresponding physical switch position. */
            bool on = read_switch_state(i);
            relay_state[i] = on ? 1 : 0;
            gpio_set_level(relay_gpio(i), relay_output_level(on ? 1 : 0));
        }
    }
    xSemaphoreGive(relay_mutex);

    esp_err_t err = save_relay_config();
    if (err != ESP_OK)
        return send_json(req, "{\"error\":\"configuration save failed\"}", "500 Internal Server Error");

    /* Return the same compact configuration format the page already uses. */
    char json[1024];
    int pos = snprintf(json, sizeof(json), "{\"config\":[");
    for (int i = 0; i < RELAY_COUNT; ++i) {
        pos += snprintf(json + pos, sizeof(json) - pos,
                        "{\"enabled\":%s,\"name\":\"%s\",\"gpio\":%d,\"switchGpio\":%d}%s",
                        relay_enabled[i] ? "true" : "false",
                        relay_name[i],
                        (int)relay_gpio(i),
                        (int)switch_gpio(i),
                        i == RELAY_COUNT - 1 ? "" : ",");
    }
    snprintf(json + pos, sizeof(json) - pos, "]}");

    return send_json(req, json, "200 OK");
}

/* -------------------- OTA -------------------- */

static bool constant_time_equal(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t n = la > lb ? la : lb;
    unsigned char diff = (unsigned char)(la ^ lb);

    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = (i < la) ? (unsigned char)a[i] : 0;
        unsigned char cb = (i < lb) ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    size_t pass_len = httpd_req_get_hdr_value_len(req, "X-OTA-Password");
    if (pass_len == 0 || pass_len > MAX_AP_PASS_LEN) {
        return send_json(req, "{\"error\":\"OTA password required\"}", "401 Unauthorized");
    }
    char ota_password[64];
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Password", ota_password, sizeof(ota_password)) != ESP_OK ||
        !constant_time_equal(ota_password, OTA_UPDATE_PASSWORD)) {
        return send_json(req, "{\"error\":\"invalid OTA password\"}", "403 Forbidden");
    }

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
    httpd_uri_t relay_config_post = {.uri="/api/relays", .method=HTTP_POST, .handler=relay_config_post_handler};
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
    httpd_register_uri_handler(http_server, &relay_config_post);
    httpd_register_uri_handler(http_server, &ota);
    httpd_register_uri_handler(http_server, &c1);
    httpd_register_uri_handler(http_server, &c2);
    httpd_register_uri_handler(http_server, &c3);
    httpd_register_uri_handler(http_server, &c4);
    httpd_register_uri_handler(http_server, &c5);
    httpd_register_uri_handler(http_server, &c6);

    ESP_LOGI(TAG, "HTTP server ready");
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
    init_switches();
    apply_all_relays();

    /* Configure TWDT before creating tasks that register themselves with it. */
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WATCHDOG_TIMEOUT_MS,
        .idle_core_mask = (1U << portNUM_PROCESSORS) - 1U,
        .trigger_panic = true
    };
    ret = esp_task_wdt_init(&wdt_config);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    BaseType_t switch_ok = xTaskCreate(physical_switch_task, "physical_switches", 3072, NULL, 4, &switch_task_handle);
    if (switch_ok != pdPASS) {
        ESP_LOGE(TAG, "Physical switch task creation failed");
    }

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
    ESP_LOGI(TAG, "Relays: 3 fixed + 2 optional");
    ESP_LOGI(TAG, "========================================");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

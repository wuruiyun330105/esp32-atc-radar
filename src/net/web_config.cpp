/**
 * net/web_config.cpp —— 设备端配网门户（AP + 网页 + 手机定位 JS）
 *
 * 手机连热点 "HGH-RADAR" → 打开 http://192.168.4.1 → 填 WiFi / 和风 Key / 经纬度 → 保存重启。
 * 注意：部分手机浏览器在 http 局域网下会拦截 navigator.geolocation（安全上下文限制），
 *      所以页面同时提供"手动填写经纬度"，JS 定位仅作最佳尝试。
 */
#include "web_config.h"
#include "data/data_config.h"

#ifdef ESP_PLATFORM
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

static WebServer s_server(80);
static bool s_active = false;
static bool s_reboot = false;

static const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>塔台雷达 · 配网</title>
<style>
body{font-family:system-ui,-apple-system,"PingFang SC",sans-serif;background:#f6ebdd;color:#7a4230;margin:0;padding:20px}
.card{max-width:420px;margin:0 auto;background:#fffaf2;border:1px solid #e1cdb8;border-radius:14px;padding:20px}
h2{margin:0 0 14px;font-size:20px}
label{display:block;font-size:13px;margin:12px 0 5px;color:#8a4b36}
input,select{width:100%;box-sizing:border-box;padding:10px;border:1px solid #e1cdb8;border-radius:8px;font-size:15px;background:#fff}
.row{display:flex;gap:10px}.row>*{flex:1}
button{width:100%;margin-top:18px;padding:12px;border:0;border-radius:10px;background:#c98e6e;color:#fff;font-size:16px}
button.sec{background:#e0c3a2;color:#7a4230;margin-top:10px}
.hint{font-size:12px;color:#a88976;margin-top:6px}
</style></head><body>
<div class="card">
<h2>塔台雷达 · 配网</h2>
<form method="POST" action="/save">
<label>WiFi 名称</label><input name="ssid" required>
<label>WiFi 密码</label><input name="pass" type="password">
<label>和风天气 API Key</label><input name="qkey">
<label>和风 API Host（控制台查看）</label><input name="qhost" placeholder="xxxx.qweatherapi.com">
<label>飞常准 MCP Key（航班动态页，可选）</label><input name="vfkey" placeholder="sk-user-...">
<label>纬度 / 经度（雷达中心 / 天气位置）</label>
<div class="row"><input name="lat" id="lat" value="31.2304"><input name="lon" id="lon" value="121.4737"></div>
<label>雷达量程</label>
<select name="range"><option value="60">60 NM（近场，塔台视角）</option>
<option value="120" selected>120 NM（推荐）</option>
<option value="200">200 NM（大范围）</option></select>
<label>航班数据源</label>
<select name="fsrc">
<option value="1" selected>ADS-B 公益源（免 key，带机型）</option>
<option value="2">OpenSky Network（覆盖广，刷新慢）</option>
<option value="3">本地接收机（自己架 dump1090）</option>
<option value="0">演示模式（无网也能看）</option></select>
<label>本地接收机地址（选"本地接收机"时填）</label>
<input name="ahost" placeholder="192.168.1.50:8080">
<button type="button" class="sec" onclick="getLoc()">📍 使用当前位置（部分浏览器可用）</button>
<div class="hint">若定位无效，请手动填写经纬度（手机地图长按可复制坐标）</div>
<button type="submit">保存并重启</button>
</form>
</div>
<script>
function getLoc(){
  if(!navigator.geolocation){alert('此浏览器不支持定位，请手动填写');return;}
  navigator.geolocation.getCurrentPosition(
    p=>{document.getElementById('lat').value=p.coords.latitude.toFixed(5);
        document.getElementById('lon').value=p.coords.longitude.toFixed(5);
        alert('已填入当前位置');},
    e=>alert('定位失败：'+e.message+'\n请手动填写经纬度'),
    {enableHighAccuracy:true,timeout:8000});
}
</script>
</body></html>
)HTML";

static void handle_root(void)
{
    s_server.send_P(200, "text/html", PAGE_HTML);
}

static void handle_save(void)
{
    app_config_t c;
    cfg_load(&c);                                   /* 先取现有，未填项保留 */
    /* 空值一律保留原值：手机上只改一项时不会把其它项清空 */
    if (s_server.arg("ssid").length())  s_server.arg("ssid").toCharArray(c.wifi_ssid, sizeof(c.wifi_ssid));
    if (s_server.arg("pass").length())  s_server.arg("pass").toCharArray(c.wifi_pass, sizeof(c.wifi_pass));
    if (s_server.arg("qkey").length())  s_server.arg("qkey").toCharArray(c.qweather_key, sizeof(c.qweather_key));
    if (s_server.arg("qhost").length()) s_server.arg("qhost").toCharArray(c.qweather_host, sizeof(c.qweather_host));
    if (s_server.arg("ahost").length()) s_server.arg("ahost").toCharArray(c.adsb_host, sizeof(c.adsb_host));
    if (s_server.arg("vfkey").length()) s_server.arg("vfkey").toCharArray(c.vf_key, sizeof(c.vf_key));
    if (s_server.hasArg("lat"))   c.latitude  = s_server.arg("lat").toFloat();
    if (s_server.hasArg("lon"))   c.longitude = s_server.arg("lon").toFloat();
    if (s_server.hasArg("range")) c.range_nm  = s_server.arg("range").toInt();
    if (s_server.hasArg("fsrc"))  c.flight_src = s_server.arg("fsrc").toInt();
    cfg_save(&c);
    s_server.send(200, "text/html; charset=utf-8",
        "<meta charset='utf-8'><body style='font-family:sans-serif;text-align:center;padding:40px'>"
        "<h3>已保存，设备即将重启…</h3></body>");
    s_reboot = true;
}

void web_config_start(void)
{
    WiFi.mode(WIFI_AP);
    WiFi.softAP("HGH-RADAR");
    s_server.on("/", handle_root);
    s_server.on("/save", HTTP_POST, handle_save);
    s_server.onNotFound(handle_root);
    s_server.begin();
    s_active = true;
}

void web_config_loop(void)
{
    if (s_active) s_server.handleClient();
}

bool web_config_active(void) { return s_active; }
bool web_config_should_reboot(void) { return s_reboot; }

void web_config_status(char *ssid_out, char *ip_out)
{
    if (ssid_out) strncpy(ssid_out, WiFi.SSID().c_str(), 32);
    if (ip_out)   strncpy(ip_out, WiFi.localIP().toString().c_str(), 16);
}
#endif

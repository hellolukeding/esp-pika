#include "wifi_portal.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef ESP_PLATFORM
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#endif

namespace esp_pika {
namespace {

#ifdef ESP_PLATFORM
constexpr const char* kTag = "wifi_portal";
constexpr uint32_t kPortalIpHostOrder = 0xC0A80401U; // 192.168.4.1

constexpr const char kPortalHtml[] = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>ESP-Mosaico Wi-Fi 配置</title>
<style>
:root{color-scheme:dark;--bg:#0b0e16;--card:#141a26;--bd:#273247;--tx:#e8edf7;--mu:#8f9db8;--ac:#38bdf8;--ok:#34d399;--wn:#fbbf24;--er:#f87171}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font-family:-apple-system,BlinkMacSystemFont,sans-serif;padding:16px}
.wrap{max-width:460px;margin:0 auto}.card{background:var(--card);border:1px solid var(--bd);border-radius:14px;padding:14px;margin-bottom:12px}
h1{font-size:18px;margin:0 0 4px}h2{font-size:14px;margin:0 0 10px;color:var(--mu);font-weight:600}
.sub{font-size:12px;color:var(--mu);word-break:break-all}.pill{display:inline-block;padding:2px 8px;border-radius:999px;font-size:11px;border:1px solid var(--bd);margin-top:6px}
.pill.ok{color:var(--ok);border-color:rgba(52,211,153,.4)}.pill.wn{color:var(--wn);border-color:rgba(251,191,36,.4)}
.row{display:flex;gap:8px;align-items:center;margin-top:8px}.btn{background:var(--ac);color:#06121c;border:0;border-radius:9px;padding:9px 13px;font-weight:600;font-size:13px;cursor:pointer}
.btn.sec{background:#1c2434;color:var(--tx);border:1px solid var(--bd)}.btn.del{background:rgba(248,113,113,.15);color:var(--er);border:1px solid rgba(248,113,113,.35)}
.btn:disabled{opacity:.5}input{width:100%;background:#0c1019;color:var(--tx);border:1px solid var(--bd);border-radius:9px;padding:9px 11px;font-size:14px;margin-top:6px}
.ap{display:flex;justify-content:space-between;align-items:center;padding:9px 10px;border:1px solid var(--bd);border-radius:10px;margin-top:7px;background:#101521}
.ap-l{min-width:0;cursor:pointer;flex:1}.ap-s{font-size:14px;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.ap-m{font-size:11px;color:var(--mu);margin-top:2px}.msg{font-size:12px;margin-top:8px;min-height:16px;color:var(--ac)}
label{display:block;font-size:12px;color:var(--mu);margin-top:8px}
</style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <h1>ESP-Mosaico 配网控制台</h1>
    <div id="dev" class="sub">MAC: --</div>
    <div id="st" class="pill">读取中...</div>
    <div id="msg" class="msg"></div>
  </div>
  <div class="card">
    <div class="row" style="justify-content:space-between;margin-top:0">
      <h2 style="margin:0">附近 Wi-Fi 与已保存网络</h2>
      <button class="btn sec" id="scanBtn" onclick="doScan(true)">扫描网络</button>
    </div>
    <div id="aps">正在加载列表...</div>
  </div>
  <div class="card">
    <h2>连接或切换网络</h2>
    <label>Wi-Fi 名称 (SSID)<input id="ssid" placeholder="点击上方网络或手动输入隐藏网络"></label>
    <label>密码（开放网络或已保存网络可留空）
      <div class="row" style="margin-top:0">
        <input id="pwd" type="password" placeholder="输入 Wi-Fi 密码">
        <button class="btn sec" type="button" onclick="togglePwd()">显隐</button>
      </div>
    </label>
    <div class="row">
      <button class="btn" id="connBtn" style="flex:1" onclick="doConnect(false)">连接 Wi-Fi</button>
      <button class="btn sec" id="savedBtn" onclick="doConnect(true)">用已保存密码连</button>
    </div>
  </div>
  <div class="card">
    <h2>网关地址覆盖（可选）</h2>
    <div class="sub">默认通过局域网 mDNS 自动发现；如需跨网段直连可填写主机与端口，留空保存可恢复默认。</div>
    <div class="row">
      <input id="gwHost" placeholder="如 192.168.101.7（留空清空覆盖）">
      <input id="gwPort" type="number" value="8088" style="width:92px">
    </div>
    <div class="row"><button class="btn sec" onclick="saveGw()">保存网关设置</button></div>
  </div>
</div>
<script>
const $=id=>document.getElementById(id);
function esc(s){return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function togglePwd(){const e=$('pwd');e.type=e.type==='password'?'text':'password'}
async function api(u,b){const r=await fetch(u,b?{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)}:{});return r.json()}
async function refresh(){
  try{
    const s=await api('/api/wifi/status');
    $('dev').textContent=`MAC: ${s.mac||'--'} · 热点: ${s.ap_ssid||'--'}`;
    const p=$('st');
    if(s.mode==='connected'){p.textContent=`已连接 ${s.ssid} (${s.ip})`;p.className='pill ok'}
    else if(s.mode==='connecting'){p.textContent=`正在连接 ${s.ssid||''}...`;p.className='pill wn'}
    else{p.textContent=s.status_msg||'等待配置 Wi-Fi';p.className='pill wn'}
    if(s.gateway_host!==undefined&&document.activeElement!==$('gwHost'))$('gwHost').value=s.gateway_host||'';
    if(s.gateway_port&&document.activeElement!==$('gwPort'))$('gwPort').value=s.gateway_port;
    renderAps(s.aps||[]);
  }catch(e){$('msg').textContent='状态刷新失败，若刚切换成功请连回目标 Wi-Fi'}
}
function renderAps(aps){
  if(!aps.length){$('aps').innerHTML='<div class="sub" style="margin-top:8px">暂无扫描结果，请点击扫描</div>';return}
  $('aps').innerHTML=aps.map(a=>{
    const tags=[a.connected?'当前连接':'',a.saved?'已保存':'',a.secure?'加密':'开放',a.rssi>-99?`${a.rssi}dBm`:'离线'].filter(Boolean).join(' · ');
    const enc=encodeURIComponent(a.ssid);
    return `<div class="ap"><div class="ap-l" onclick="pick('${enc}')"><div class="ap-s">${esc(a.ssid)}</div><div class="ap-m">${esc(tags)}</div></div><div class="row" style="margin:0">${a.saved?`<button class="btn sec" onclick="quick('${enc}')">连</button><button class="btn del" onclick="forget('${enc}')">删</button>`:`<button class="btn sec" onclick="pick('${enc}')">选</button>`}</div></div>`;
  }).join('');
}
function pick(enc){$('ssid').value=decodeURIComponent(enc);$('pwd').focus()}
async function quick(enc){const ssid=decodeURIComponent(enc);$('ssid').value=ssid;await doConnect(true)}
async function doScan(force){
  $('scanBtn').disabled=true;$('msg').textContent='正在扫描 2.4GHz Wi-Fi...';
  try{const r=await api(force?'/api/wifi/scan':'/api/wifi/scan?cached=1');renderAps(r.aps||[]);$('msg').textContent=`已更新 ${r.aps?r.aps.length:0} 个网络`}
  finally{$('scanBtn').disabled=false}
}
async function doConnect(useSaved){
  const ssid=$('ssid').value.trim(),password=$('pwd').value;
  if(!ssid){$('msg').textContent='请先填写 SSID';return}
  $('msg').textContent=`正在下发连接 ${ssid}...`;
  const r=await api('/api/wifi/connect',{ssid,password,use_saved:useSaved});
  $('msg').textContent=r.message||(r.ok?'已发起连接，请等待状态更新':'连接请求失败');
  setTimeout(refresh,1200);
}
async function forget(enc){
  const ssid=decodeURIComponent(enc);
  const r=await api('/api/wifi/forget',{ssid});
  $('msg').textContent=r.message||'已移除';refresh();
}
async function saveGw(){
  const host=$('gwHost').value.trim(),port=Number($('gwPort').value)||8080;
  const r=await api('/api/wifi/gateway',{host,port});
  $('msg').textContent=r.message||'已保存网关设置';
}
refresh();setInterval(refresh,3000);
</script>
</body>
</html>)HTML";

std::string ReadHttpRequestBody(httpd_req_t* req, bool* ok) {
    *ok = false;
    if (req == nullptr || req->content_len == 0 || req->content_len > 1024) {
        return "";
    }
    std::string body(req->content_len, '\0');
    size_t offset = 0;
    while (offset < body.size()) {
        const int r = httpd_req_recv(req, body.data() + offset, body.size() - offset);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return "";
        }
        offset += static_cast<size_t>(r);
    }
    *ok = true;
    return body;
}

esp_err_t SendJsonResponse(httpd_req_t* req, const char* status, const std::string& json) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.data(), static_cast<ssize_t>(json.size()));
}

esp_err_t SendResultJson(httpd_req_t* req, bool ok, const std::string& msg) {
    std::string out = "{\"ok\":";
    out += ok ? "true" : "false";
    out += ",\"message\":\"" + EscapeJsonString(msg) + "\"}";
    return SendJsonResponse(req, ok ? "200 OK" : "400 Bad Request", out);
}
#endif

} // namespace

std::string BuildPortalApSsid(const std::string& mac) {
    std::string hex;
    hex.reserve(12);
    for (unsigned char c : mac) {
        if (std::isxdigit(c)) {
            hex.push_back(static_cast<char>(std::toupper(c)));
        }
    }
    if (hex.size() >= 4) {
        return "ESP-Mosaico-" + hex.substr(hex.size() - 4);
    }
    return "ESP-Mosaico-SETUP";
}

size_t BuildDnsWildcardReply(const uint8_t* query,
                             size_t query_len,
                             uint32_t ipv4_be,
                             uint8_t* out_reply,
                             size_t max_len) {
    if (query == nullptr || out_reply == nullptr || query_len < 12) {
        return 0;
    }
    // Require DNS standard query (QR == 0) with at least one question.
    if ((query[2] & 0x80U) != 0) {
        return 0;
    }
    const uint16_t qdcount = static_cast<uint16_t>((query[4] << 8) | query[5]);
    if (qdcount == 0) {
        return 0;
    }

    size_t pos = 12;
    while (pos < query_len) {
        const uint8_t len = query[pos++];
        if (len == 0) {
            break;
        }
        // Reject compressed QNAMEs or oversized labels in incoming queries.
        if (len > 63 || pos + len > query_len) {
            return 0;
        }
        pos += len;
    }
    if (pos + 4 > query_len) {
        return 0;
    }
    const size_t question_end = pos + 4; // Include QTYPE + QCLASS.
    constexpr size_t kAnswerLen = 16;
    if (question_end + kAnswerLen > max_len) {
        return 0;
    }

    std::memcpy(out_reply, query, question_end);
    // Header: keep Transaction ID, set QR=1, AA=1, preserve RD bit.
    out_reply[2] = static_cast<uint8_t>(0x84U | (query[2] & 0x01U));
    out_reply[3] = 0x00U;
    out_reply[4] = 0x00U;
    out_reply[5] = 0x01U; // QDCOUNT = 1
    out_reply[6] = 0x00U;
    out_reply[7] = 0x01U; // ANCOUNT = 1
    out_reply[8] = 0x00U;
    out_reply[9] = 0x00U; // NSCOUNT = 0
    out_reply[10] = 0x00U;
    out_reply[11] = 0x00U; // ARCOUNT = 0

    // Answer RR: pointer to offset 12 (0xC00C), TYPE=A (1), CLASS=IN (1), TTL=60, RDLENGTH=4.
    size_t w = question_end;
    out_reply[w++] = 0xC0U;
    out_reply[w++] = 0x0CU;
    out_reply[w++] = 0x00U;
    out_reply[w++] = 0x01U;
    out_reply[w++] = 0x00U;
    out_reply[w++] = 0x01U;
    out_reply[w++] = 0x00U;
    out_reply[w++] = 0x00U;
    out_reply[w++] = 0x00U;
    out_reply[w++] = 0x3CU;
    out_reply[w++] = 0x00U;
    out_reply[w++] = 0x04U;
    std::memcpy(out_reply + w, &ipv4_be, sizeof(ipv4_be));
    w += sizeof(ipv4_be);
    return w;
}

bool ParsePortalConnectRequest(const std::string& body,
                               std::string* ssid,
                               std::string* password,
                               bool* use_saved) {
    if (ssid == nullptr || password == nullptr || use_saved == nullptr) {
        return false;
    }
    if (!ExtractJsonStringField(body, "ssid", ssid) || ssid->empty() || ssid->size() > 32) {
        return false;
    }
    password->clear();
    std::string parsed_pwd;
    if (ExtractJsonStringField(body, "password", &parsed_pwd)) {
        if (parsed_pwd.size() > 64) {
            return false;
        }
        *password = std::move(parsed_pwd);
    }
    bool saved = false;
    (void)ExtractJsonBoolField(body, "use_saved", &saved);
    *use_saved = saved;
    return true;
}

bool ParsePortalForgetRequest(const std::string& body, std::string* ssid) {
    if (ssid == nullptr) {
        return false;
    }
    return ExtractJsonStringField(body, "ssid", ssid) && !ssid->empty() && ssid->size() <= 32;
}

bool ParsePortalGatewayRequest(const std::string& body, std::string* host, uint16_t* port) {
    if (host == nullptr || port == nullptr) {
        return false;
    }
    if (!ExtractJsonStringField(body, "host", host)) {
        return false;
    }
    int64_t parsed_port = 8080;
    if (ExtractJsonIntField(body, "port", &parsed_port)) {
        if (parsed_port < 1 || parsed_port > 65535) {
            return false;
        }
        *port = static_cast<uint16_t>(parsed_port);
    } else {
        *port = 8080;
    }
    return true;
}

WifiPortal::~WifiPortal() {
    Stop();
}

bool WifiPortal::Start(WifiPortalCallbacks callbacks) {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    callbacks_ = std::move(callbacks);
#ifdef ESP_PLATFORM
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 6144;
    cfg.core_id = 1;
    cfg.max_uri_handlers = 10;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&httpd_, &cfg) != ESP_OK) {
        ESP_LOGE(kTag, "httpd_start failed");
        httpd_ = nullptr;
        return false;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = &WifiPortal::HandleIndex, .user_ctx = this},
        {.uri = "/api/wifi/status", .method = HTTP_GET, .handler = &WifiPortal::HandleStatus, .user_ctx = this},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = &WifiPortal::HandleScan, .user_ctx = this},
        {.uri = "/api/wifi/connect", .method = HTTP_POST, .handler = &WifiPortal::HandleConnect, .user_ctx = this},
        {.uri = "/api/wifi/forget", .method = HTTP_POST, .handler = &WifiPortal::HandleForget, .user_ctx = this},
        {.uri = "/api/wifi/gateway", .method = HTTP_POST, .handler = &WifiPortal::HandleGateway, .user_ctx = this},
        {.uri = "/*", .method = HTTP_GET, .handler = &WifiPortal::HandleCaptiveRedirect, .user_ctx = this},
    };
    for (const auto& route : routes) {
        httpd_register_uri_handler(httpd_, &route);
    }

    dns_done_ = xSemaphoreCreateBinary();
    running_.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCore(&WifiPortal::DnsTaskEntry, "DnsPortalTask", 4096, this, 3,
                                &dns_task_, 1) != pdPASS) {
        ESP_LOGW(kTag, "DnsPortalTask creation failed; continuing with HTTP portal only");
        dns_task_ = nullptr;
        if (dns_done_ != nullptr) {
            vSemaphoreDelete(dns_done_);
            dns_done_ = nullptr;
        }
    }
    ESP_LOGI(kTag, "Captive Portal started at http://192.168.4.1/");
    return true;
#else
    running_.store(true, std::memory_order_release);
    return true;
#endif
}

void WifiPortal::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
#ifdef ESP_PLATFORM
    if (dns_task_ != nullptr) {
        if (dns_done_ != nullptr) {
            xSemaphoreTake(dns_done_, pdMS_TO_TICKS(1000));
            vSemaphoreDelete(dns_done_);
            dns_done_ = nullptr;
        }
        dns_task_ = nullptr;
    }
    if (httpd_ != nullptr) {
        httpd_stop(httpd_);
        httpd_ = nullptr;
    }
    ESP_LOGI(kTag, "Captive Portal stopped");
#endif
}

#ifdef ESP_PLATFORM
esp_err_t WifiPortal::HandleIndex(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kPortalHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WifiPortal::HandleStatus(httpd_req_t* req) {
    auto* self = static_cast<WifiPortal*>(req->user_ctx);
    std::string body = "{}";
    if (self != nullptr && self->callbacks_.get_status_json) {
        body = self->callbacks_.get_status_json();
    }
    return SendJsonResponse(req, "200 OK", body);
}

esp_err_t WifiPortal::HandleScan(httpd_req_t* req) {
    auto* self = static_cast<WifiPortal*>(req->user_ctx);
    char query[64] = {};
    bool trigger_scan = true;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16] = {};
        if (httpd_query_key_value(query, "cached", val, sizeof(val)) == ESP_OK &&
            std::strcmp(val, "1") == 0) {
            trigger_scan = false;
        }
    }
    std::string body = "{\"aps\":[]}";
    if (self != nullptr && self->callbacks_.get_scan_json) {
        body = self->callbacks_.get_scan_json(trigger_scan);
    }
    return SendJsonResponse(req, "200 OK", body);
}

esp_err_t WifiPortal::HandleConnect(httpd_req_t* req) {
    auto* self = static_cast<WifiPortal*>(req->user_ctx);
    bool body_ok = false;
    const std::string body = ReadHttpRequestBody(req, &body_ok);
    std::string ssid;
    std::string password;
    bool use_saved = false;
    if (!body_ok || !ParsePortalConnectRequest(body, &ssid, &password, &use_saved)) {
        return SendResultJson(req, false, "无效的 Wi-Fi 参数");
    }
    std::string msg = "OK";
    bool ok = false;
    if (self != nullptr && self->callbacks_.on_connect) {
        ok = self->callbacks_.on_connect(ssid, password, use_saved, &msg);
    }
    return SendResultJson(req, ok, msg);
}

esp_err_t WifiPortal::HandleForget(httpd_req_t* req) {
    auto* self = static_cast<WifiPortal*>(req->user_ctx);
    bool body_ok = false;
    const std::string body = ReadHttpRequestBody(req, &body_ok);
    std::string ssid;
    if (!body_ok || !ParsePortalForgetRequest(body, &ssid)) {
        return SendResultJson(req, false, "无效的 SSID");
    }
    std::string msg = "已移除";
    bool ok = false;
    if (self != nullptr && self->callbacks_.on_forget) {
        ok = self->callbacks_.on_forget(ssid, &msg);
    }
    return SendResultJson(req, ok, msg);
}

esp_err_t WifiPortal::HandleGateway(httpd_req_t* req) {
    auto* self = static_cast<WifiPortal*>(req->user_ctx);
    bool body_ok = false;
    const std::string body = ReadHttpRequestBody(req, &body_ok);
    std::string host;
    uint16_t port = 8080;
    if (!body_ok || !ParsePortalGatewayRequest(body, &host, &port)) {
        return SendResultJson(req, false, "无效的网关地址或端口");
    }
    std::string msg = "已保存";
    bool ok = false;
    if (self != nullptr && self->callbacks_.on_set_gateway) {
        ok = self->callbacks_.on_set_gateway(host, port, &msg);
    }
    return SendResultJson(req, ok, msg);
}

esp_err_t WifiPortal::HandleCaptiveRedirect(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, nullptr, 0);
}

void WifiPortal::DnsTaskEntry(void* arg) {
    auto* self = static_cast<WifiPortal*>(arg);
    if (self != nullptr) {
        self->RunDnsLoop();
        if (self->dns_done_ != nullptr) {
            xSemaphoreGive(self->dns_done_);
        }
    }
    vTaskDelete(nullptr);
}

void WifiPortal::RunDnsLoop() {
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(kTag, "DNS UDP socket creation failed");
        return;
    }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);
    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ESP_LOGW(kTag, "DNS UDP:53 bind failed");
        close(sock);
        return;
    }

    const uint32_t portal_ip_be = htonl(kPortalIpHostOrder);
    uint8_t rx_buf[256] = {};
    uint8_t tx_buf[300] = {};
    while (running_.load(std::memory_order_acquire)) {
        struct sockaddr_in client = {};
        socklen_t client_len = sizeof(client);
        const ssize_t n = recvfrom(sock, rx_buf, sizeof(rx_buf), 0,
                                   reinterpret_cast<struct sockaddr*>(&client), &client_len);
        if (n <= 0) {
            continue;
        }
        const size_t reply_len = BuildDnsWildcardReply(rx_buf, static_cast<size_t>(n),
                                                       portal_ip_be, tx_buf, sizeof(tx_buf));
        if (reply_len > 0) {
            sendto(sock, tx_buf, reply_len, 0,
                   reinterpret_cast<struct sockaddr*>(&client), client_len);
        }
    }
    close(sock);
}
#endif

} // namespace esp_pika

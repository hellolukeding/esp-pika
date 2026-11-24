#include "net_client.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "mac_identity.h"

#ifdef ESP_PLATFORM
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"
#endif

namespace esp_pika {
namespace {

constexpr size_t kMaxMergedAps = 20;

#ifdef ESP_PLATFORM
constexpr const char* kTag = "net_client";
constexpr int kWifiConnectedBit = BIT0;
constexpr int kWifiFailBit = BIT1;
constexpr int kScanDoneBit = BIT2;

int64_t NowMs() {
    return esp_timer_get_time() / 1000;
}
#endif

std::string ApsToJsonArray(const std::vector<WifiApInfo>& aps) {
    std::string out = "[";
    for (size_t i = 0; i < aps.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        const auto& ap = aps[i];
        out += "{\"ssid\":\"" + EscapeJsonString(ap.ssid) + "\"";
        out += ",\"rssi\":" + std::to_string(ap.rssi);
        out += ",\"secure\":" + std::string(ap.secure ? "true" : "false");
        out += ",\"saved\":" + std::string(ap.saved ? "true" : "false");
        out += ",\"connected\":" + std::string(ap.connected ? "true" : "false");
        out.push_back('}');
    }
    out.push_back(']');
    return out;
}

} // namespace

void ApplyMdnsTxtRecords(const std::vector<std::pair<std::string, std::string>>& txt,
                         GatewayEndpoint* endpoint) {
    if (endpoint == nullptr) {
        return;
    }
    for (const auto& [k, v] : txt) {
        if (k == "group_id" && !v.empty()) {
            endpoint->group_id = v;
        } else if (k == "tls") {
            endpoint->tls = (v == "true" || v == "1");
        } else if (k == "ws_path" && !v.empty()) {
            endpoint->ws_path = v;
        } else if (k == "media_path" && !v.empty()) {
            endpoint->media_path = v;
        }
    }
}

std::vector<WifiApInfo> MergeWifiLists(const std::vector<WifiCredential>& saved,
                                       const std::vector<WifiApInfo>& scanned,
                                       const std::string& connected_ssid) {
    struct MergedItem {
        WifiApInfo info;
        bool visible = false;
        size_t saved_order = 9999;
    };

    std::map<std::string, MergedItem> by_ssid;
    for (const auto& ap : scanned) {
        if (ap.ssid.empty()) {
            continue;
        }
        auto it = by_ssid.find(ap.ssid);
        if (it == by_ssid.end()) {
            MergedItem item;
            item.info = ap;
            item.info.saved = false;
            item.info.connected = (!connected_ssid.empty() && ap.ssid == connected_ssid);
            item.visible = true;
            by_ssid.emplace(ap.ssid, std::move(item));
        } else {
            if (ap.rssi > it->second.info.rssi) {
                it->second.info.rssi = ap.rssi;
                it->second.info.secure = ap.secure;
            } else if (ap.rssi == it->second.info.rssi) {
                it->second.info.secure = it->second.info.secure || ap.secure;
            }
        }
    }

    for (size_t i = 0; i < saved.size(); ++i) {
        const auto& cred = saved[i];
        if (cred.ssid.empty()) {
            continue;
        }
        auto it = by_ssid.find(cred.ssid);
        if (it == by_ssid.end()) {
            MergedItem item;
            item.info.ssid = cred.ssid;
            item.info.rssi = -100;
            item.info.secure = !cred.password.empty();
            item.info.saved = true;
            item.info.connected = (!connected_ssid.empty() && cred.ssid == connected_ssid);
            item.visible = false;
            item.saved_order = i;
            by_ssid.emplace(cred.ssid, std::move(item));
        } else {
            it->second.info.saved = true;
            it->second.saved_order = std::min(it->second.saved_order, i);
        }
    }

    if (!connected_ssid.empty() && by_ssid.find(connected_ssid) == by_ssid.end()) {
        MergedItem item;
        item.info.ssid = connected_ssid;
        item.info.rssi = -50;
        item.info.secure = true;
        item.info.connected = true;
        item.visible = true;
        by_ssid.emplace(connected_ssid, std::move(item));
    }

    std::vector<MergedItem> items;
    items.reserve(by_ssid.size());
    for (auto& kv : by_ssid) {
        items.push_back(std::move(kv.second));
    }

    auto rank_bucket = [](const MergedItem& item) -> int {
        if (item.info.connected) return 0;
        if (item.info.saved && item.visible) return 1;
        if (item.info.saved) return 2;
        return 3;
    };

    std::sort(items.begin(), items.end(), [&](const MergedItem& a, const MergedItem& b) {
        const int ba = rank_bucket(a);
        const int bb = rank_bucket(b);
        if (ba != bb) {
            return ba < bb;
        }
        if (ba == 1 || ba == 3) {
            if (a.info.rssi != b.info.rssi) {
                return a.info.rssi > b.info.rssi;
            }
        }
        if (ba == 1 || ba == 2) {
            if (a.saved_order != b.saved_order) {
                return a.saved_order < b.saved_order;
            }
        }
        return a.info.ssid < b.info.ssid;
    });

    std::vector<WifiApInfo> out;
    out.reserve(std::min(items.size(), kMaxMergedAps));
    for (size_t i = 0; i < items.size() && i < kMaxMergedAps; ++i) {
        out.push_back(std::move(items[i].info));
    }
    return out;
}

std::vector<WifiCredential> SelectAutoConnectCandidates(const std::vector<WifiCredential>& saved,
                                                        const std::vector<WifiApInfo>& scanned,
                                                        const std::string& exclude_ssid) {
    std::map<std::string, int> best_rssi;
    for (const auto& ap : scanned) {
        if (ap.ssid.empty()) {
            continue;
        }
        auto it = best_rssi.find(ap.ssid);
        if (it == best_rssi.end() || ap.rssi > it->second) {
            best_rssi[ap.ssid] = ap.rssi;
        }
    }

    struct Candidate {
        WifiCredential cred;
        int rssi;
    };
    std::vector<Candidate> list;
    for (const auto& cred : saved) {
        if (cred.ssid.empty() || cred.ssid == exclude_ssid) {
            continue;
        }
        auto it = best_rssi.find(cred.ssid);
        if (it != best_rssi.end()) {
            list.push_back({cred, it->second});
        }
    }

    std::sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) {
        if (a.rssi != b.rssi) {
            return a.rssi > b.rssi;
        }
        if (a.cred.last_used_seq != b.cred.last_used_seq) {
            return a.cred.last_used_seq > b.cred.last_used_seq;
        }
        return a.cred.ssid < b.cred.ssid;
    });

    std::vector<WifiCredential> out;
    out.reserve(list.size());
    for (auto& c : list) {
        out.push_back(std::move(c.cred));
    }
    return out;
}

const char* WifiReasonToMessage(int reason) {
    switch (reason) {
    case 15:  // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
    case 202: // WIFI_REASON_AUTH_FAIL
    case 204: // WIFI_REASON_HANDSHAKE_TIMEOUT
    case 205: // WIFI_REASON_MIC_FAILURE
        return "WRONG PASSWORD";
    case 201: // WIFI_REASON_NO_AP_FOUND
    case 210: // WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY
    case 211: // WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
    case 212: // WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD
        return "AP NOT FOUND";
    case 2:   // WIFI_REASON_AUTH_EXPIRE
    case 16:  // WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT
    case 206: // WIFI_REASON_CONNECTION_FAIL
        return "AUTH TIMEOUT";
    case 17:
    case 18:
    case 19:
    case 203: // WIFI_REASON_ASSOC_FAIL
        return "ASSOC FAILED";
    default:
        return "LINK LOST";
    }
}

NetClient::~NetClient() {
    portal_.Stop();
#ifdef ESP_PLATFORM
    StopWorker();
#endif
    DisconnectWebSocket();
#ifdef ESP_PLATFORM
    if (wifi_events_ != nullptr) {
        vEventGroupDelete(wifi_events_);
        wifi_events_ = nullptr;
    }
#endif
}

void NetClient::SetWifiStateCallback(WifiStateCallback cb) {
    std::lock_guard<std::mutex> lock(wifi_cb_mu_);
    on_wifi_state_ = std::move(cb);
}

void NetClient::RefreshStatusLocked() {
    const WifiGatewayOverride gw = wifi_store_.GatewayOverride();
    status_.mac = device_mac_;
    status_.ap_ssid = BuildPortalApSsid(device_mac_);
    status_.ap_ip = "192.168.4.1";
    status_.gateway_host = gw.enabled ? gw.host : "";
    status_.gateway_port = gw.port;
    status_.aps = MergeWifiLists(wifi_store_.List(), scanned_aps_,
                                 status_.wifi_connected ? status_.ssid : "");
}

void NetClient::EmitWifiStatus(bool bump_revision) {
    WifiStatusSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        RefreshStatusLocked();
        if (bump_revision) {
            ++status_.revision;
        }
        snap = status_;
    }
    WifiStateCallback cb;
    {
        std::lock_guard<std::mutex> lock(wifi_cb_mu_);
        cb = on_wifi_state_;
    }
    if (cb) {
        cb(snap);
    }
}

WifiStatusSnapshot NetClient::GetWifiStatus() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return status_;
}

std::string NetClient::BuildStatusJson() const {
    WifiStatusSnapshot s = GetWifiStatus();
    const char* mode_str = "offline";
    switch (s.state) {
    case WifiConnState::kConnected: mode_str = "connected"; break;
    case WifiConnState::kConnecting: mode_str = "connecting"; break;
    case WifiConnState::kPortal: mode_str = "portal"; break;
    case WifiConnState::kOffline:
    default:
        mode_str = "offline";
        break;
    }
    std::string out = "{";
    out += "\"mode\":\"" + std::string(mode_str) + "\"";
    out += ",\"wifi_connected\":" + std::string(s.wifi_connected ? "true" : "false");
    out += ",\"portal_active\":" + std::string(s.portal_active ? "true" : "false");
    out += ",\"scanning\":" + std::string(s.scanning ? "true" : "false");
    out += ",\"mac\":\"" + EscapeJsonString(s.mac) + "\"";
    out += ",\"ssid\":\"" + EscapeJsonString(s.ssid) + "\"";
    out += ",\"ip\":\"" + EscapeJsonString(s.ip) + "\"";
    out += ",\"rssi\":" + std::to_string(s.rssi);
    out += ",\"ap_ssid\":\"" + EscapeJsonString(s.ap_ssid) + "\"";
    out += ",\"ap_ip\":\"" + EscapeJsonString(s.ap_ip) + "\"";
    out += ",\"gateway_host\":\"" + EscapeJsonString(s.gateway_host) + "\"";
    out += ",\"gateway_port\":" + std::to_string(s.gateway_port);
    out += ",\"status_msg\":\"" + EscapeJsonString(s.status_msg) + "\"";
    out += ",\"aps\":" + ApsToJsonArray(s.aps);
    out.push_back('}');
    return out;
}

std::string NetClient::BuildScanJson(bool trigger_scan) {
#ifdef ESP_PLATFORM
    if (trigger_scan && wifi_events_ != nullptr) {
        xEventGroupClearBits(wifi_events_, kScanDoneBit);
        if (RequestScan()) {
            xEventGroupWaitBits(wifi_events_, kScanDoneBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(3500));
        }
    }
#else
    (void)trigger_scan;
#endif
    WifiStatusSnapshot s = GetWifiStatus();
    return "{\"ok\":true,\"aps\":" + ApsToJsonArray(s.aps) + "}";
}

void NetClient::EnqueueWorkerCmd(WorkerCmd cmd, bool urgent) {
    {
        std::lock_guard<std::mutex> lock(cmd_mu_);
        if (urgent && cmd_queue_.size() > 8) {
            std::queue<WorkerCmd> empty;
            std::swap(cmd_queue_, empty);
        }
        cmd_queue_.push(std::move(cmd));
    }
#ifdef ESP_PLATFORM
    if (worker_sem_ != nullptr) {
        xSemaphoreGive(worker_sem_);
    }
#endif
}

bool NetClient::PopWorkerCmd(WorkerCmd* out) {
    if (out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(cmd_mu_);
    if (cmd_queue_.empty()) {
        return false;
    }
    *out = std::move(cmd_queue_.front());
    cmd_queue_.pop();
    return true;
}

bool NetClient::HasPendingUrgentCmd() const {
    std::lock_guard<std::mutex> lock(cmd_mu_);
    return !cmd_queue_.empty();
}

bool NetClient::Init(const NetClientConfig& cfg) {
    cfg_ = cfg;
    if (!ReadFactoryEfuseMac(&device_mac_)) {
#ifdef ESP_PLATFORM
        ESP_LOGE(kTag, "failed to read factory eFuse MAC");
#endif
        return false;
    }
#ifdef ESP_PLATFORM
    ESP_LOGI(kTag, "device factory eFuse MAC=%s", device_mac_.c_str());
#endif

    {
        std::lock_guard<std::mutex> lock(state_mu_);
        wifi_store_.LoadFromNvs();
        if (wifi_store_.Empty() && !cfg_.wifi_ssid.empty()) {
            if (wifi_store_.SeedFromDefaults(cfg_.wifi_ssid, cfg_.wifi_password)) {
                wifi_store_.SaveToNvs();
            }
        }
        status_ = WifiStatusSnapshot{};
        RefreshStatusLocked();
    }

#ifdef ESP_PLATFORM
    if (!wifi_started_) {
        if (wifi_events_ == nullptr) {
            wifi_events_ = xEventGroupCreate();
            if (wifi_events_ == nullptr) {
                return false;
            }
        }

        esp_err_t netif_err = esp_netif_init();
        if (netif_err != ESP_OK && netif_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(kTag, "esp_netif_init failed: %s", esp_err_to_name(netif_err));
            return false;
        }
        esp_err_t evt_err = esp_event_loop_create_default();
        if (evt_err != ESP_OK && evt_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(kTag, "event loop create failed: %s", esp_err_to_name(evt_err));
            return false;
        }
        if (sta_netif_ == nullptr) {
            sta_netif_ = esp_netif_create_default_wifi_sta();
        }
        if (ap_netif_ == nullptr) {
            ap_netif_ = esp_netif_create_default_wifi_ap();
        }

        wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&wifi_cfg) != ESP_OK) {
            ESP_LOGE(kTag, "esp_wifi_init failed");
            return false;
        }

        if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &NetClient::WifiEventHandler, this,
                                                nullptr) != ESP_OK ||
            esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                &NetClient::WifiEventHandler, this,
                                                nullptr) != ESP_OK) {
            ESP_LOGE(kTag, "wifi/ip event registration failed");
            return false;
        }

        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
            esp_wifi_set_protocol(WIFI_IF_STA,
                                  WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N) != ESP_OK ||
            esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20) != ESP_OK ||
            esp_wifi_start() != ESP_OK) {
            ESP_LOGE(kTag, "esp_wifi_start failed");
            return false;
        }
        // Disable modem sleep to prevent beacon timeouts against phone hotspots.
        esp_wifi_set_ps(WIFI_PS_NONE);
        wifi_started_ = true;
    }

    EmitWifiStatus(true);
    StartWorker();
    WorkerCmd init_cmd;
    init_cmd.type = WorkerCmdType::kAutoConnect;
    EnqueueWorkerCmd(std::move(init_cmd));

    bool has_saved = false;
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        has_saved = !wifi_store_.Empty();
    }
    if (!has_saved) {
        ESP_LOGI(kTag, "no saved Wi-Fi credentials; waiting via portal/UI");
        return false;
    }
    return WaitForWifi(8000);
#else
    EmitWifiStatus(true);
    return false;
#endif
}

bool NetClient::WaitForWifi(uint32_t timeout_ms) {
#ifdef ESP_PLATFORM
    if (!wifi_started_ || wifi_events_ == nullptr) {
        return false;
    }
    if (timeout_ms > 0) {
        xEventGroupWaitBits(wifi_events_, kWifiConnectedBit, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(timeout_ms));
    }
    if (!wifi_connected_.load(std::memory_order_acquire)) {
        return false;
    }
    if (!mdns_started_) {
        if (mdns_init() == ESP_OK) {
            mdns_started_ = true;
        } else {
            ESP_LOGW(kTag, "mdns_init failed; fallback gateway remains available");
        }
    }
    return true;
#else
    (void)timeout_ms;
    return wifi_connected_.load(std::memory_order_acquire);
#endif
}

bool NetClient::RequestScan() {
    WorkerCmd cmd;
    cmd.type = WorkerCmdType::kScan;
    EnqueueWorkerCmd(std::move(cmd));
    return true;
}

bool NetClient::RequestConnect(const std::string& ssid,
                               const std::string& password,
                               bool use_saved) {
    if (ssid.empty() || ssid.size() > 32 || password.size() > 64) {
        return false;
    }
    WorkerCmd cmd;
    cmd.type = WorkerCmdType::kConnect;
    cmd.ssid = ssid;
    cmd.password = password;
    cmd.flag = use_saved;
    EnqueueWorkerCmd(std::move(cmd), true);
    return true;
}

bool NetClient::RequestForget(const std::string& ssid) {
    if (ssid.empty() || ssid.size() > 32) {
        return false;
    }
    WorkerCmd cmd;
    cmd.type = WorkerCmdType::kForget;
    cmd.ssid = ssid;
    EnqueueWorkerCmd(std::move(cmd), true);
    return true;
}

bool NetClient::RequestPortal(bool enable) {
    WorkerCmd cmd;
    cmd.type = WorkerCmdType::kTogglePortal;
    cmd.flag = enable;
    EnqueueWorkerCmd(std::move(cmd), true);
    return true;
}

bool NetClient::SetGatewayOverride(const std::string& host, uint16_t port) {
    WorkerCmd cmd;
    cmd.type = WorkerCmdType::kSetGateway;
    cmd.ssid = host;
    cmd.port = (port == 0) ? 8080 : port;
    EnqueueWorkerCmd(std::move(cmd), true);
    return true;
}

bool NetClient::DiscoverGateway(GatewayEndpoint* out_endpoint) {
    if (out_endpoint == nullptr) {
        return false;
    }
#ifdef ESP_PLATFORM
    if (mdns_started_) {
        mdns_result_t* results = nullptr;
        esp_err_t err = mdns_query_ptr("_esp-gateway", "_tcp", cfg_.mdns_timeout_ms, 4, &results);
        const bool had_results = (results != nullptr);
        if (err == ESP_OK && results != nullptr) {
            GatewayEndpoint found;
            for (mdns_result_t* r = results; r != nullptr; r = r->next) {
                if (r->port == 0) {
                    continue;
                }
                for (mdns_ip_addr_t* a = r->addr; a != nullptr; a = a->next) {
                    if (a->addr.type != ESP_IPADDR_TYPE_V4) {
                        continue;
                    }
                    char ip_buf[16] = {};
                    esp_ip4addr_ntoa(&a->addr.u_addr.ip4, ip_buf, sizeof(ip_buf));
                    if (ip_buf[0] == '\0') {
                        continue;
                    }
                    found = GatewayEndpoint{};
                    found.port = r->port;
                    found.host = ip_buf;
                    std::vector<std::pair<std::string, std::string>> txt;
                    for (size_t i = 0; i < r->txt_count; ++i) {
                        if (r->txt[i].key != nullptr) {
                            txt.emplace_back(r->txt[i].key,
                                             r->txt[i].value != nullptr ? r->txt[i].value : "");
                        }
                    }
                    ApplyMdnsTxtRecords(txt, &found);
                    break;
                }
                if (!found.host.empty() && found.port > 0) {
                    break;
                }
            }
            if (!found.host.empty() && found.port > 0) {
                mdns_query_results_free(results);
                *out_endpoint = found;
                endpoint_ = found;
                ESP_LOGI(kTag, "discovered gateway via mDNS: %s:%u group=%s",
                         found.host.c_str(), found.port, found.group_id.c_str());
                return true;
            }
        }
        if (results != nullptr) {
            mdns_query_results_free(results);
        }
        ESP_LOGI(kTag, "mDNS lookup yielded no IPv4 gateway (err=%s results=%s)",
                 esp_err_to_name(err), had_results ? "present" : "empty");
    }
#endif
    WifiGatewayOverride ov;
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        ov = wifi_store_.GatewayOverride();
    }
    if (ov.enabled) {
        if (ov.host.empty()) {
            return false;
        }
        GatewayEndpoint custom;
        custom.host = ov.host;
        custom.port = ov.port;
        *out_endpoint = custom;
        endpoint_ = custom;
#ifdef ESP_PLATFORM
        ESP_LOGI(kTag, "using NVS override gateway: %s:%u", custom.host.c_str(), custom.port);
#endif
        return true;
    }
    if (!cfg_.fallback_host.empty()) {
        GatewayEndpoint fb;
        fb.host = cfg_.fallback_host;
        fb.port = cfg_.fallback_port;
        *out_endpoint = fb;
        endpoint_ = fb;
#ifdef ESP_PLATFORM
        ESP_LOGI(kTag, "using fallback gateway: %s:%u", fb.host.c_str(), fb.port);
#endif
        return true;
    }
    return false;
}

bool NetClient::ConnectWebSocket(const GatewayEndpoint& endpoint,
                                 TextCallback on_text,
                                 BinaryCallback on_binary,
                                 ConnStateCallback on_state) {
    if (device_mac_.empty() || endpoint.host.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> ws_lock(ws_lifecycle_mu_);
    DisconnectWebSocketLocked();

    endpoint_ = endpoint;
    on_text_ = std::move(on_text);
    on_binary_ = std::move(on_binary);
    on_state_ = std::move(on_state);

#ifdef ESP_PLATFORM
    char uri_buf[256] = {};
    std::snprintf(uri_buf, sizeof(uri_buf), "%s://%s:%u%s",
                  endpoint.tls ? "wss" : "ws",
                  endpoint.host.c_str(),
                  endpoint.port,
                  endpoint.ws_path.c_str());

    std::string mac_hdr = "X-Device-MAC: " + device_mac_ + "\r\n";
    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = uri_buf;
    ws_cfg.headers = mac_hdr.c_str();
    ws_cfg.buffer_size = 4096;
    ws_cfg.task_stack = 8192;
    ws_cfg.task_core_id_set = true;
    ws_cfg.task_core_id = 1;
    ws_cfg.task_prio = 6;
    ws_cfg.reconnect_timeout_ms = 5000;
    ws_cfg.network_timeout_ms = 10000;
    ws_cfg.ping_interval_sec = 20;

    ws_handle_ = esp_websocket_client_init(&ws_cfg);
    if (ws_handle_ == nullptr) {
        return false;
    }
    esp_websocket_register_events(ws_handle_, WEBSOCKET_EVENT_ANY, &NetClient::WebSocketEventHandler, this);
    if (esp_websocket_client_start(ws_handle_) != ESP_OK) {
        esp_websocket_client_destroy(ws_handle_);
        ws_handle_ = nullptr;
        return false;
    }
    return true;
#else
    return false;
#endif
}

void NetClient::DisconnectWebSocketLocked() {
#ifdef ESP_PLATFORM
    ws_closing_.store(true, std::memory_order_release);
    ws_connected_.store(false, std::memory_order_release);
    for (int i = 0; i < 100 && ws_in_flight_.load(std::memory_order_acquire) > 0; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    esp_websocket_client_handle_t handle = ws_handle_;
    ws_handle_ = nullptr;
    if (handle != nullptr) {
        esp_websocket_client_stop(handle);
        esp_websocket_client_destroy(handle);
    }
    ws_closing_.store(false, std::memory_order_release);
#else
    ws_connected_.store(false, std::memory_order_release);
#endif
}

void NetClient::DisconnectWebSocket() {
    std::lock_guard<std::mutex> ws_lock(ws_lifecycle_mu_);
    DisconnectWebSocketLocked();
}

bool NetClient::SendText(const std::string& json_text) {
#ifdef ESP_PLATFORM
    if (ws_closing_.load(std::memory_order_acquire)) {
        return false;
    }
    ws_in_flight_.fetch_add(1, std::memory_order_acq_rel);
    bool ok = false;
    esp_websocket_client_handle_t handle = ws_handle_;
    if (!ws_closing_.load(std::memory_order_acquire) && handle != nullptr &&
        esp_websocket_client_is_connected(handle)) {
        const int sent = esp_websocket_client_send_text(
            handle, json_text.data(), static_cast<int>(json_text.size()), pdMS_TO_TICKS(1000));
        ok = (sent == static_cast<int>(json_text.size()));
    }
    ws_in_flight_.fetch_sub(1, std::memory_order_acq_rel);
    return ok;
#else
    (void)json_text;
    return false;
#endif
}

bool NetClient::SendBinary(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return false;
    }
#ifdef ESP_PLATFORM
    if (ws_closing_.load(std::memory_order_acquire)) {
        return false;
    }
    ws_in_flight_.fetch_add(1, std::memory_order_acq_rel);
    bool ok = false;
    esp_websocket_client_handle_t handle = ws_handle_;
    if (!ws_closing_.load(std::memory_order_acquire) && handle != nullptr &&
        esp_websocket_client_is_connected(handle)) {
        const int sent = esp_websocket_client_send_bin(
            handle, reinterpret_cast<const char*>(data), static_cast<int>(len), pdMS_TO_TICKS(500));
        ok = (sent == static_cast<int>(len));
    }
    ws_in_flight_.fetch_sub(1, std::memory_order_acq_rel);
    return ok;
#else
    return false;
#endif
}

bool NetClient::UploadJpeg(const uint8_t* data, size_t len, std::string* out_media_url) {
    if (data == nullptr || len == 0 || out_media_url == nullptr || device_mac_.empty() ||
        endpoint_.host.empty()) {
        return false;
    }
#ifdef ESP_PLATFORM
    char url_buf[256] = {};
    std::snprintf(url_buf, sizeof(url_buf), "%s://%s:%u%s",
                  endpoint_.tls ? "https" : "http",
                  endpoint_.host.c_str(),
                  endpoint_.port,
                  endpoint_.media_path.c_str());

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url_buf;
    http_cfg.method = HTTP_METHOD_POST;
    http_cfg.timeout_ms = 8000;

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == nullptr) {
        return false;
    }
    esp_http_client_set_header(client, "X-Device-MAC", device_mac_.c_str());
    esp_http_client_set_header(client, "Content-Type", "image/jpeg");

    esp_err_t err = esp_http_client_open(client, static_cast<int>(len));
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    int written = esp_http_client_write(client, reinterpret_cast<const char*>(data), static_cast<int>(len));
    if (written != static_cast<int>(len)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    std::string resp_body;
    if (content_len > 0 && content_len < 4096) {
        resp_body.resize(static_cast<size_t>(content_len));
        int read_len = esp_http_client_read(client, resp_body.data(), content_len);
        if (read_len >= 0) {
            resp_body.resize(static_cast<size_t>(read_len));
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        ESP_LOGW(kTag, "media upload rejected HTTP %d", status);
        return false;
    }
    return ParseMediaUploadResponse(resp_body, out_media_url);
#else
    return false;
#endif
}

#ifdef ESP_PLATFORM
void NetClient::StartWorker() {
    if (worker_running_.load(std::memory_order_acquire)) {
        return;
    }
    worker_sem_ = xSemaphoreCreateBinary();
    worker_done_ = xSemaphoreCreateBinary();
    worker_running_.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCore(&NetClient::WorkerTaskEntry, "NetWorkerTask", 6144, this, 4,
                                &worker_task_, 1) != pdPASS) {
        ESP_LOGE(kTag, "failed to create NetWorkerTask");
        worker_running_.store(false, std::memory_order_release);
        if (worker_sem_ != nullptr) {
            vSemaphoreDelete(worker_sem_);
            worker_sem_ = nullptr;
        }
        if (worker_done_ != nullptr) {
            vSemaphoreDelete(worker_done_);
            worker_done_ = nullptr;
        }
        worker_task_ = nullptr;
    }
}

void NetClient::StopWorker() {
    if (!worker_running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (worker_sem_ != nullptr) {
        xSemaphoreGive(worker_sem_);
    }
    if (worker_done_ != nullptr) {
        xSemaphoreTake(worker_done_, pdMS_TO_TICKS(2000));
        vSemaphoreDelete(worker_done_);
        worker_done_ = nullptr;
    }
    if (worker_sem_ != nullptr) {
        vSemaphoreDelete(worker_sem_);
        worker_sem_ = nullptr;
    }
    worker_task_ = nullptr;
}

void NetClient::WorkerTaskEntry(void* arg) {
    auto* self = static_cast<NetClient*>(arg);
    if (self != nullptr) {
        self->RunWorkerLoop();
        if (self->worker_done_ != nullptr) {
            xSemaphoreGive(self->worker_done_);
        }
    }
    vTaskDelete(nullptr);
}

void NetClient::RunWorkerLoop() {
    int64_t last_auto_scan_ms = NowMs();
    while (worker_running_.load(std::memory_order_acquire)) {
        WorkerCmd cmd;
        if (PopWorkerCmd(&cmd)) {
            HandleWorkerCmd(cmd);
            last_auto_scan_ms = NowMs();
            continue;
        }

        const int64_t now = NowMs();
        bool do_auto_stop_portal = false;
        bool do_periodic_portal_rescan = false;
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            if (portal_active_.load(std::memory_order_acquire) && portal_auto_stop_ms_ > 0 &&
                now >= portal_auto_stop_ms_) {
                do_auto_stop_portal = true;
            } else if (portal_active_.load(std::memory_order_acquire) && !portal_manual_ &&
                       !wifi_connected_.load(std::memory_order_acquire) &&
                       !wifi_store_.Empty() && (now - last_auto_scan_ms) >= 45000) {
                do_periodic_portal_rescan = true;
            }
        }

        if (do_auto_stop_portal) {
            DisablePortalInternal();
        } else if (do_periodic_portal_rescan) {
            last_auto_scan_ms = now;
            wifi_sta_list_t sta_list = {};
            if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK && sta_list.num == 0) {
                if (RunScanInternal()) {
                    std::vector<WifiCredential> saved;
                    std::vector<WifiApInfo> scanned;
                    {
                        std::lock_guard<std::mutex> lock(state_mu_);
                        saved = wifi_store_.List();
                        scanned = scanned_aps_;
                    }
                    const auto candidates = SelectAutoConnectCandidates(saved, scanned);
                    for (const auto& cred : candidates) {
                        int reason = 0;
                        if (ConnectSingleNetwork(cred.ssid, cred.password, 10000, &reason)) {
                            break;
                        }
                        if (HasPendingUrgentCmd() || !worker_running_.load(std::memory_order_acquire)) {
                            break;
                        }
                    }
                }
            }
        }

        if (worker_sem_ != nullptr) {
            xSemaphoreTake(worker_sem_, pdMS_TO_TICKS(500));
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void NetClient::HandleWorkerCmd(const WorkerCmd& cmd) {
    switch (cmd.type) {
    case WorkerCmdType::kAutoConnect:
        RunAutoConnect();
        break;
    case WorkerCmdType::kScan:
        RunScanInternal();
        break;
    case WorkerCmdType::kConnect: {
        std::string password = cmd.password;
        if (cmd.flag) {
            WifiCredential cred;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(state_mu_);
                found = wifi_store_.Find(cmd.ssid, &cred);
            }
            if (!found) {
                {
                    std::lock_guard<std::mutex> lock(state_mu_);
                    status_.status_msg = "NO SAVED KEY";
                }
                EmitWifiStatus(true);
                break;
            }
            password = cred.password;
        }
        int reason = 0;
        if (!ConnectSingleNetwork(cmd.ssid, password, 12000, &reason)) {
            if (!HasPendingUrgentCmd() && !wifi_connected_.load(std::memory_order_acquire) &&
                !portal_active_.load(std::memory_order_acquire)) {
                EnablePortalInternal(false);
            }
        }
        break;
    }
    case WorkerCmdType::kForget: {
        bool was_connected = false;
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            wifi_store_.Remove(cmd.ssid);
            wifi_store_.SaveToNvs();
            was_connected = (status_.wifi_connected && status_.ssid == cmd.ssid);
            if (!was_connected) {
                status_.status_msg = "FORGOT " + cmd.ssid;
            }
        }
        if (was_connected) {
            DisconnectWebSocket();
            intentional_disconnect_.store(true, std::memory_order_release);
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(60));
            intentional_disconnect_.store(false, std::memory_order_release);
            wifi_connected_.store(false, std::memory_order_release);
            xEventGroupClearBits(wifi_events_, kWifiConnectedBit);
            {
                std::lock_guard<std::mutex> lock(state_mu_);
                status_.wifi_connected = false;
                status_.ssid.clear();
                status_.ip.clear();
                status_.rssi = -100;
                status_.state = portal_active_.load(std::memory_order_acquire)
                                    ? WifiConnState::kPortal
                                    : WifiConnState::kOffline;
                status_.status_msg = "FORGOT " + cmd.ssid;
            }
            EmitWifiStatus(true);
            RunAutoConnect(cmd.ssid);
        } else {
            EmitWifiStatus(true);
        }
        break;
    }
    case WorkerCmdType::kTogglePortal:
        if (cmd.flag) {
            EnablePortalInternal(true);
        } else {
            DisablePortalInternal();
        }
        break;
    case WorkerCmdType::kSetGateway:
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            if (cmd.ssid.empty()) {
                wifi_store_.ClearGatewayOverride();
            } else {
                wifi_store_.SetGatewayOverride(cmd.ssid, cmd.port);
            }
            wifi_store_.SaveToNvs();
            status_.status_msg = cmd.ssid.empty() ? "GW DEFAULT" : "GW SAVED";
        }
        EmitWifiStatus(true);
        break;
    case WorkerCmdType::kReconnectRuntime:
        RunRuntimeReconnect();
        break;
    }
}

bool NetClient::RunScanInternal() {
    if (!wifi_started_) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        status_.scanning = true;
    }
    EmitWifiStatus(true);

    if (!wifi_connected_.load(std::memory_order_acquire) &&
        !connect_attempt_active_.load(std::memory_order_acquire)) {
        intentional_disconnect_.store(true, std::memory_order_release);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(50));
        intentional_disconnect_.store(false, std::memory_order_release);
    }

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    bool ok = (err == ESP_OK);
    std::vector<WifiApInfo> found;
    if (ok) {
        uint16_t ap_count = 0;
        if (esp_wifi_scan_get_ap_num(&ap_count) == ESP_OK && ap_count > 0) {
            uint16_t limit = std::min<uint16_t>(ap_count, 24);
            std::vector<wifi_ap_record_t> records(limit);
            if (esp_wifi_scan_get_ap_records(&limit, records.data()) == ESP_OK) {
                found.reserve(limit);
                for (uint16_t i = 0; i < limit; ++i) {
                    const char* raw_ssid = reinterpret_cast<const char*>(records[i].ssid);
                    const size_t len = strnlen(raw_ssid, sizeof(records[i].ssid));
                    if (len == 0) {
                        continue;
                    }
                    WifiApInfo info;
                    info.ssid.assign(raw_ssid, len);
                    info.rssi = records[i].rssi;
                    info.secure = (records[i].authmode != WIFI_AUTH_OPEN);
                    found.push_back(std::move(info));
                }
            }
        }
    } else {
        ESP_LOGW(kTag, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
    }

    {
        std::lock_guard<std::mutex> lock(state_mu_);
        status_.scanning = false;
        if (ok) {
            scanned_aps_ = std::move(found);
        }
    }
    if (wifi_events_ != nullptr) {
        xEventGroupSetBits(wifi_events_, kScanDoneBit);
    }
    EmitWifiStatus(true);
    return ok;
}

bool NetClient::ConnectSingleNetwork(const std::string& ssid,
                                     const std::string& password,
                                     uint32_t timeout_ms,
                                     int* out_reason) {
    if (out_reason != nullptr) {
        *out_reason = 0;
    }
    if (!wifi_started_ || ssid.empty() || ssid.size() > 32 || password.size() > 64) {
        return false;
    }

    ESP_LOGI(kTag, "connecting Wi-Fi SSID=%s", ssid.c_str());
    DisconnectWebSocket();

    connect_attempt_active_.store(true, std::memory_order_release);
    intentional_disconnect_.store(true, std::memory_order_release);
    wifi_connected_.store(false, std::memory_order_release);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(60));
    intentional_disconnect_.store(false, std::memory_order_release);
    last_disconnect_reason_.store(0, std::memory_order_release);
    xEventGroupClearBits(wifi_events_, kWifiConnectedBit | kWifiFailBit);

    {
        std::lock_guard<std::mutex> lock(state_mu_);
        status_.state = WifiConnState::kConnecting;
        status_.wifi_connected = false;
        status_.ssid = ssid;
        status_.ip.clear();
        status_.rssi = -100;
        status_.status_msg = "CONNECTING...";
    }
    EmitWifiStatus(true);

    const wifi_mode_t target_mode =
        portal_active_.load(std::memory_order_acquire) ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&current_mode) != ESP_OK || current_mode != target_mode) {
        esp_wifi_set_mode(target_mode);
    }

    wifi_config_t sta_cfg = {};
    std::memcpy(sta_cfg.sta.ssid, ssid.data(), ssid.size());
    if (!password.empty()) {
        std::memcpy(sta_cfg.sta.password, password.data(), password.size());
    }
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    if (esp_wifi_set_config(WIFI_IF_STA, &sta_cfg) != ESP_OK || esp_wifi_connect() != ESP_OK) {
        connect_attempt_active_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            status_.wifi_connected = false;
            status_.state = portal_active_.load(std::memory_order_acquire)
                                ? WifiConnState::kPortal
                                : WifiConnState::kOffline;
            status_.status_msg = "ASSOC FAILED";
        }
        EmitWifiStatus(true);
        return false;
    }

    const int64_t deadline = NowMs() + static_cast<int64_t>(timeout_ms);
    bool connected = false;
    bool failed = false;
    while (NowMs() < deadline && worker_running_.load(std::memory_order_acquire)) {
        const EventBits_t bits = xEventGroupWaitBits(
            wifi_events_, kWifiConnectedBit | kWifiFailBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(200));
        if ((bits & kWifiConnectedBit) != 0) {
            connected = true;
            break;
        }
        if ((bits & kWifiFailBit) != 0) {
            failed = true;
            break;
        }
        if (HasPendingUrgentCmd()) {
            break;
        }
    }
    connect_attempt_active_.store(false, std::memory_order_release);

    if (connected) {
        if (!mdns_started_) {
            if (mdns_init() == ESP_OK) {
                mdns_started_ = true;
            } else {
                ESP_LOGW(kTag, "mdns_init failed; fallback gateway remains available");
            }
        }
        int rssi = -55;
        wifi_ap_record_t ap_info = {};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            wifi_store_.Upsert(ssid, password, true);
            wifi_store_.SaveToNvs();
            status_.state = WifiConnState::kConnected;
            status_.wifi_connected = true;
            status_.ssid = ssid;
            status_.rssi = rssi;
            status_.status_msg.clear();
            if (portal_active_.load(std::memory_order_acquire)) {
                portal_auto_stop_ms_ = NowMs() + 4000;
                portal_manual_ = false;
            }
        }
        EmitWifiStatus(true);
        ESP_LOGI(kTag, "Wi-Fi connected SSID=%s rssi=%d", ssid.c_str(), rssi);
        return true;
    }

    const int reason = last_disconnect_reason_.load(std::memory_order_acquire);
    if (out_reason != nullptr) {
        *out_reason = reason;
    }
    if (!failed) {
        intentional_disconnect_.store(true, std::memory_order_release);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(40));
        intentional_disconnect_.store(false, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        status_.wifi_connected = false;
        status_.ip.clear();
        status_.rssi = -100;
        status_.state = portal_active_.load(std::memory_order_acquire) ? WifiConnState::kPortal
                                                                     : WifiConnState::kOffline;
        status_.status_msg = HasPendingUrgentCmd()
                                 ? "SWITCHING..."
                                 : (reason != 0 ? WifiReasonToMessage(reason) : "CONNECT TIMEOUT");
    }
    EmitWifiStatus(true);
    return false;
}

bool NetClient::RunAutoConnect(const std::string& exclude_ssid) {
    std::vector<WifiCredential> saved;
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        saved = wifi_store_.List();
    }
    if (saved.empty()) {
        RunScanInternal();
        EnablePortalInternal(false);
        return false;
    }

    // Fast path: try most-recently-used saved network first before blocking on full scan.
    const WifiCredential* first_cred = nullptr;
    for (const auto& cred : saved) {
        if (cred.ssid != exclude_ssid) {
            first_cred = &cred;
            break;
        }
    }
    if (first_cred != nullptr) {
        int reason = 0;
        if (ConnectSingleNetwork(first_cred->ssid, first_cred->password, 8000, &reason)) {
            return true;
        }
        if (HasPendingUrgentCmd() || !worker_running_.load(std::memory_order_acquire)) {
            return false;
        }
    }

    // Fallback: scan 2.4GHz channels and try visible saved networks strongest-first.
    RunScanInternal();
    std::vector<WifiApInfo> scanned;
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        scanned = scanned_aps_;
    }
    const auto candidates = SelectAutoConnectCandidates(saved, scanned, exclude_ssid);
    for (const auto& cred : candidates) {
        if (first_cred != nullptr && cred.ssid == first_cred->ssid) {
            continue;
        }
        int reason = 0;
        if (ConnectSingleNetwork(cred.ssid, cred.password, 10000, &reason)) {
            return true;
        }
        if (HasPendingUrgentCmd() || !worker_running_.load(std::memory_order_acquire)) {
            return false;
        }
    }

    if (!wifi_connected_.load(std::memory_order_acquire) && !HasPendingUrgentCmd()) {
        EnablePortalInternal(false);
    }
    return false;
}

void NetClient::RunRuntimeReconnect() {
    DisconnectWebSocket();
    if (wifi_connected_.load(std::memory_order_acquire) || HasPendingUrgentCmd()) {
        return;
    }
    std::string last_ssid;
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        last_ssid = status_.ssid;
    }
    WifiCredential cred;
    bool has_cred = false;
    if (!last_ssid.empty()) {
        std::lock_guard<std::mutex> lock(state_mu_);
        has_cred = wifi_store_.Find(last_ssid, &cred);
    }

    if (has_cred) {
        constexpr uint32_t kBackoffMs[3] = {2000, 4000, 6000};
        for (int attempt = 0; attempt < 3; ++attempt) {
            for (uint32_t waited = 0; waited < kBackoffMs[attempt]; waited += 200) {
                if (HasPendingUrgentCmd() || !worker_running_.load(std::memory_order_acquire)) {
                    return;
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            int reason = 0;
            if (ConnectSingleNetwork(cred.ssid, cred.password, 10000, &reason)) {
                return;
            }
            if (HasPendingUrgentCmd() || !worker_running_.load(std::memory_order_acquire)) {
                return;
            }
        }
    }

    RunAutoConnect(last_ssid);
}

bool NetClient::EnablePortalInternal(bool manual) {
    const std::string ap_ssid = BuildPortalApSsid(device_mac_);
    if (!wifi_started_) {
        return false;
    }
    if (portal_active_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(state_mu_);
        portal_manual_ = portal_manual_ || manual;
        portal_auto_stop_ms_ = 0;
        return true;
    }

    wifi_config_t ap_cfg = {};
    const size_t ssid_len = std::min<size_t>(ap_ssid.size(), sizeof(ap_cfg.ap.ssid));
    std::memcpy(ap_cfg.ap.ssid, ap_ssid.data(), ssid_len);
    ap_cfg.ap.ssid_len = static_cast<uint8_t>(ssid_len);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    // Ensure STA is idle before switching to APSTA so SoftAP channel doesn't collide mid-scan.
    if (!wifi_connected_.load(std::memory_order_acquire)) {
        intentional_disconnect_.store(true, std::memory_order_release);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(50));
        intentional_disconnect_.store(false, std::memory_order_release);
    }

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg) != ESP_OK) {
        ESP_LOGE(kTag, "failed to configure WIFI_MODE_APSTA");
        return false;
    }

    WifiPortalCallbacks cbs;
    cbs.get_status_json = [this]() { return BuildStatusJson(); };
    cbs.get_scan_json = [this](bool trigger_scan) { return BuildScanJson(trigger_scan); };
    cbs.on_connect = [this](const std::string& ssid,
                            const std::string& password,
                            bool use_saved,
                            std::string* out_msg) {
        if (use_saved) {
            WifiCredential cred;
            std::lock_guard<std::mutex> lock(state_mu_);
            if (!wifi_store_.Find(ssid, &cred)) {
                if (out_msg != nullptr) {
                    *out_msg = "未找到该网络的已保存密码";
                }
                return false;
            }
        }
        const bool queued = RequestConnect(ssid, password, use_saved);
        if (out_msg != nullptr) {
            *out_msg = queued ? ("正在连接 " + ssid + "...") : "Wi-Fi 参数无效";
        }
        return queued;
    };
    cbs.on_forget = [this](const std::string& ssid, std::string* out_msg) {
        const bool queued = RequestForget(ssid);
        if (out_msg != nullptr) {
            *out_msg = queued ? ("已移除 " + ssid) : "无效的 SSID";
        }
        return queued;
    };
    cbs.on_set_gateway = [this](const std::string& host, uint16_t port, std::string* out_msg) {
        const bool queued = SetGatewayOverride(host, port);
        if (out_msg != nullptr) {
            *out_msg = queued ? "网关设置已更新，重连时生效" : "保存失败";
        }
        return queued;
    };

    if (!portal_.Start(std::move(cbs))) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        return false;
    }

    portal_active_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        portal_manual_ = manual;
        portal_auto_stop_ms_ = 0;
        if (!status_.wifi_connected) {
            status_.state = WifiConnState::kPortal;
            if (status_.status_msg.empty()) {
                status_.status_msg = "AP READY";
            }
        }
    }
    EmitWifiStatus(true);
    ESP_LOGI(kTag, "SoftAP portal active SSID=%s", ap_ssid.c_str());
    return true;
}

void NetClient::DisablePortalInternal() {
    if (!portal_active_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    portal_.Stop();
    if (wifi_started_) {
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        portal_manual_ = false;
        portal_auto_stop_ms_ = 0;
        status_.state = status_.wifi_connected ? WifiConnState::kConnected : WifiConnState::kOffline;
    }
    EmitWifiStatus(true);
}

void NetClient::WifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<NetClient*>(arg);
    if (self == nullptr) {
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* disc = static_cast<const wifi_event_sta_disconnected_t*>(data);
        const int reason = (disc != nullptr) ? static_cast<int>(disc->reason) : 0;
        self->last_disconnect_reason_.store(reason, std::memory_order_release);
        if (self->wifi_events_ != nullptr) {
            xEventGroupClearBits(self->wifi_events_, kWifiConnectedBit);
        }
        if (self->intentional_disconnect_.load(std::memory_order_acquire)) {
            return;
        }
        ESP_LOGW(kTag, "Wi-Fi disconnected reason=%d", reason);
        if (self->connect_attempt_active_.load(std::memory_order_acquire)) {
            if (self->wifi_events_ != nullptr) {
                xEventGroupSetBits(self->wifi_events_, kWifiFailBit);
            }
            return;
        }
        const bool was_connected = self->wifi_connected_.exchange(false, std::memory_order_acq_rel);
        if (was_connected) {
            {
                std::lock_guard<std::mutex> lock(self->state_mu_);
                self->status_.wifi_connected = false;
                self->status_.ip.clear();
                self->status_.rssi = -100;
                self->status_.state = self->portal_active_.load(std::memory_order_acquire)
                                          ? WifiConnState::kPortal
                                          : WifiConnState::kOffline;
                self->status_.status_msg = WifiReasonToMessage(reason);
            }
            self->EmitWifiStatus(true);
            WorkerCmd rec_cmd;
            rec_cmd.type = WorkerCmdType::kReconnectRuntime;
            self->EnqueueWorkerCmd(std::move(rec_cmd));
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* got = static_cast<const ip_event_got_ip_t*>(data);
        char ip_buf[16] = {};
        if (got != nullptr) {
            esp_ip4addr_ntoa(&got->ip_info.ip, ip_buf, sizeof(ip_buf));
            ESP_LOGI(kTag, "Wi-Fi got IP=" IPSTR " gw=" IPSTR,
                     IP2STR(&got->ip_info.ip), IP2STR(&got->ip_info.gw));
        }
        self->wifi_connected_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(self->state_mu_);
            self->status_.wifi_connected = true;
            self->status_.ip = ip_buf;
            if (!self->connect_attempt_active_.load(std::memory_order_acquire)) {
                self->status_.state = WifiConnState::kConnected;
            }
        }
        if (self->wifi_events_ != nullptr) {
            xEventGroupSetBits(self->wifi_events_, kWifiConnectedBit);
        }
        if (!self->connect_attempt_active_.load(std::memory_order_acquire)) {
            self->EmitWifiStatus(true);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(kTag, "Wi-Fi STA lost IP");
        if (self->wifi_events_ != nullptr) {
            xEventGroupClearBits(self->wifi_events_, kWifiConnectedBit);
        }
    }
}

void NetClient::WebSocketEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)base;
    auto* self = static_cast<NetClient*>(arg);
    auto* evt = static_cast<esp_websocket_event_data_t*>(data);

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(kTag, "WebSocket connected to %s:%u",
                 self->endpoint_.host.c_str(), self->endpoint_.port);
        self->ws_connected_.store(true, std::memory_order_release);
        if (self->on_state_) {
            self->on_state_(true);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(kTag, "WebSocket error type=%d http_status=%d",
                 evt != nullptr ? static_cast<int>(evt->error_handle.error_type) : -1,
                 evt != nullptr ? evt->error_handle.esp_ws_handshake_status_code : -1);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(kTag, "WebSocket disconnected id=%ld", static_cast<long>(id));
        self->ws_connected_.store(false, std::memory_order_release);
        if (self->on_state_) {
            self->on_state_(false);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (evt->op_code == 0x01 || (evt->op_code == 0x00 && !self->rx_text_buf_.empty())) {
            if (evt->op_code == 0x01 && evt->payload_offset == 0) {
                self->rx_text_buf_.clear();
            }
            if (evt->data_len > 0 && self->rx_text_buf_.size() < 16384) {
                const size_t room = 16384 - self->rx_text_buf_.size();
                self->rx_text_buf_.append(
                    evt->data_ptr, std::min<size_t>(static_cast<size_t>(evt->data_len), room));
            }
            const bool frame_complete =
                (evt->payload_len <= 0 || evt->payload_offset + evt->data_len >= evt->payload_len);
            if (frame_complete && !self->rx_text_buf_.empty()) {
                if (self->on_text_) {
                    self->on_text_(self->rx_text_buf_);
                }
                self->rx_text_buf_.clear();
            }
        } else if (evt->op_code == 0x02 && evt->data_len > 0) {
            if (self->on_binary_) {
                self->on_binary_(reinterpret_cast<const uint8_t*>(evt->data_ptr),
                                 static_cast<size_t>(evt->data_len));
            }
        }
        break;
    default:
        break;
    }
}
#endif

} // namespace esp_pika

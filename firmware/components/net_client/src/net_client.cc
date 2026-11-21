#include "net_client.h"

#include <cstdio>
#include <cstring>

#include "mac_identity.h"

#ifdef ESP_PLATFORM
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"
#endif

namespace esp_pika {
namespace {

#ifdef ESP_PLATFORM
constexpr const char* kTag = "net_client";
constexpr int kWifiConnectedBit = BIT0;
#endif

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

NetClient::~NetClient() {
    DisconnectWebSocket();
#ifdef ESP_PLATFORM
    if (wifi_events_ != nullptr) {
        vEventGroupDelete(wifi_events_);
        wifi_events_ = nullptr;
    }
#endif
}

bool NetClient::Init(const NetClientConfig& cfg) {
    cfg_ = cfg;
    if (!ReadFactoryEfuseMac(&device_mac_)) {
#ifdef ESP_PLATFORM
        ESP_LOGE(kTag, "failed to read factory eFuse MAC");
        return false;
#else
        return false;
#endif
    }
#ifdef ESP_PLATFORM
    ESP_LOGI(kTag, "device factory eFuse MAC=%s", device_mac_.c_str());
#endif

#ifdef ESP_PLATFORM
    if (cfg_.wifi_ssid.empty()) {
        ESP_LOGW(kTag, "Wi-Fi SSID empty; staying offline");
        return false;
    }

    if (!wifi_started_) {
        if (wifi_events_ == nullptr) {
            wifi_events_ = xEventGroupCreate();
            if (wifi_events_ == nullptr) {
                return false;
            }
        }

        if (esp_netif_init() != ESP_OK) {
            ESP_LOGE(kTag, "esp_netif_init failed");
            return false;
        }
        esp_err_t evt_err = esp_event_loop_create_default();
        if (evt_err != ESP_OK && evt_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(kTag, "event loop create failed: %s", esp_err_to_name(evt_err));
            return false;
        }
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&wifi_cfg) != ESP_OK) {
            ESP_LOGE(kTag, "esp_wifi_init failed");
            return false;
        }

        if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &NetClient::WifiEventHandler, this,
                                                nullptr) != ESP_OK ||
            esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &NetClient::WifiEventHandler, this,
                                                nullptr) != ESP_OK) {
            ESP_LOGE(kTag, "wifi/ip event registration failed");
            return false;
        }

        wifi_config_t sta_cfg = {};
        std::snprintf(reinterpret_cast<char*>(sta_cfg.sta.ssid), sizeof(sta_cfg.sta.ssid), "%s",
                      cfg_.wifi_ssid.c_str());
        std::snprintf(reinterpret_cast<char*>(sta_cfg.sta.password), sizeof(sta_cfg.sta.password), "%s",
                      cfg_.wifi_password.c_str());
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;

        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
            esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N) != ESP_OK ||
            esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20) != ESP_OK ||
            esp_wifi_set_config(WIFI_IF_STA, &sta_cfg) != ESP_OK ||
            esp_wifi_start() != ESP_OK) {
            ESP_LOGE(kTag, "esp_wifi_start failed");
            return false;
        }
        // Disable modem sleep to prevent beacon timeouts against phone hotspots.
        esp_wifi_set_ps(WIFI_PS_NONE);
        wifi_started_ = true;
        ESP_LOGI(kTag, "Wi-Fi STA started, connecting to SSID=%s", cfg_.wifi_ssid.c_str());
    }

    return WaitForWifi(20000);
#else
    return false;
#endif
}

bool NetClient::WaitForWifi(uint32_t timeout_ms) {
#ifdef ESP_PLATFORM
    if (!wifi_started_ || wifi_events_ == nullptr) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(wifi_events_, kWifiConnectedBit, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if ((bits & kWifiConnectedBit) == 0) {
        ESP_LOGW(kTag, "Wi-Fi association/DHCP pending");
        return false;
    }
    if (cfg_.fallback_host.empty() && !mdns_started_) {
        if (mdns_init() == ESP_OK) {
            mdns_started_ = true;
        } else {
            ESP_LOGW(kTag, "mdns_init failed; fallback gateway remains available");
        }
    }
    ESP_LOGI(kTag, "Wi-Fi connected, factory MAC=%s", device_mac_.c_str());
    return true;
#else
    (void)timeout_ms;
    return false;
#endif
}

bool NetClient::DiscoverGateway(GatewayEndpoint* out_endpoint) {
    if (out_endpoint == nullptr) {
        return false;
    }
    if (!cfg_.fallback_host.empty()) {
        GatewayEndpoint fb;
        fb.host = cfg_.fallback_host;
        fb.port = cfg_.fallback_port;
        *out_endpoint = fb;
        endpoint_ = fb;
#ifdef ESP_PLATFORM
        ESP_LOGI(kTag, "using configured gateway endpoint: %s:%u", fb.host.c_str(), fb.port);
#endif
        return true;
    }
#ifdef ESP_PLATFORM
    mdns_result_t* results = nullptr;
    esp_err_t err = mdns_query_ptr("_esp-gateway", "_tcp", cfg_.mdns_timeout_ms, 4, &results);
    if (err == ESP_OK && results != nullptr) {
        GatewayEndpoint found;
        found.port = results->port;
        if (results->addr != nullptr && results->addr->addr.type == ESP_IPADDR_TYPE_V4) {
            char ip_buf[16] = {};
            esp_ip4addr_ntoa(&results->addr->addr.u_addr.ip4, ip_buf, sizeof(ip_buf));
            found.host = ip_buf;
        }
        std::vector<std::pair<std::string, std::string>> txt;
        for (size_t i = 0; i < results->txt_count; ++i) {
            if (results->txt[i].key != nullptr) {
                txt.emplace_back(results->txt[i].key,
                                 results->txt[i].value != nullptr ? results->txt[i].value : "");
            }
        }
        ApplyMdnsTxtRecords(txt, &found);
        mdns_query_results_free(results);
        if (!found.host.empty() && found.port > 0) {
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
#endif
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
    DisconnectWebSocket();
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
    ws_cfg.task_stack = 8192;
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

void NetClient::DisconnectWebSocket() {
#ifdef ESP_PLATFORM
    if (ws_handle_ != nullptr) {
        esp_websocket_client_stop(ws_handle_);
        esp_websocket_client_destroy(ws_handle_);
        ws_handle_ = nullptr;
    }
#endif
    ws_connected_ = false;
}

bool NetClient::SendText(const std::string& json_text) {
#ifdef ESP_PLATFORM
    if (ws_handle_ == nullptr || !esp_websocket_client_is_connected(ws_handle_)) {
        return false;
    }
    int sent = esp_websocket_client_send_text(ws_handle_, json_text.data(), static_cast<int>(json_text.size()),
                                              pdMS_TO_TICKS(2000));
    return sent == static_cast<int>(json_text.size());
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
    if (ws_handle_ == nullptr || !esp_websocket_client_is_connected(ws_handle_)) {
        return false;
    }
    int sent = esp_websocket_client_send_bin(ws_handle_, reinterpret_cast<const char*>(data),
                                             static_cast<int>(len), pdMS_TO_TICKS(1000));
    return sent == static_cast<int>(len);
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
void NetClient::WifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<NetClient*>(arg);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* disc = static_cast<const wifi_event_sta_disconnected_t*>(data);
        ESP_LOGW(kTag, "Wi-Fi disconnected reason=%d; reconnecting",
                 disc != nullptr ? static_cast<int>(disc->reason) : -1);
        xEventGroupClearBits(self->wifi_events_, kWifiConnectedBit);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* got = static_cast<const ip_event_got_ip_t*>(data);
        if (got != nullptr) {
            ESP_LOGI(kTag, "Wi-Fi got IP=" IPSTR " gw=" IPSTR,
                     IP2STR(&got->ip_info.ip), IP2STR(&got->ip_info.gw));
        }
        xEventGroupSetBits(self->wifi_events_, kWifiConnectedBit);
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
        self->ws_connected_ = true;
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
        self->ws_connected_ = false;
        if (self->on_state_) {
            self->on_state_(false);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (evt->op_code == 0x01 && evt->data_len > 0) { // Text frame
            self->rx_text_buf_.assign(evt->data_ptr, static_cast<size_t>(evt->data_len));
            if (self->on_text_) {
                self->on_text_(self->rx_text_buf_);
            }
        } else if (evt->op_code == 0x02 && evt->data_len > 0) { // Binary Opus frame
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

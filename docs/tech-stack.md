# ESP32-S31 智能语音与多模态 Agent 终端技术选型文档

> **版本**：v1.2  
> **关联架构**：[architecture.md](./architecture.md)（v1.4）  
> **状态**：固件层（支持 **EZ Assistant 原生直连** 与 **Go Kratos 网关集群** 双模）、网关与微服务层（**Go Kratos 原生 gRPC + NATS 事件总线**）、数据存储层已定型；**Agent 大脑层待定（后续接入自研 Agent 代码，已预留语言无关的 gRPC / NATS + MCP 接入契约）**。

---

## 1. 终端固件层（ESP32-S31）

| 模块 | 选定方案 | 备选方案 | 选型理由 |
| :--- | :--- | :--- | :--- |
| **SDK 与语言** | **ESP-IDF v6.x + C++20 / C 混合** | 纯 C | 底层驱动与 ISR 用 C，Board 抽象 / AudioService / 状态机用 C++（兼容 `xiaozhi-esp32` 风格，便于多板型扩展） |
| **构建与环境** | **EIM + CMake + Ninja + `idf.py`** | — | ESP-IDF v6.x 官方推荐工作流，跨 macOS / Linux / Windows 一致 |
| **音频前处理 / VAD / 唤醒** | **`espressif/esp-sr`**（AFE: AEC + NS/BSS + VADNet + WakeNet / MultiNet） | Silero VAD (TFLM) | 官方量化优化流水线，统一处理回声消除、降噪、VAD 与离线唤醒 |
| **端侧声纹识别** | **`espressif/esp-dsp`**（FBank/MFCC）+ **`espressif/esp-dl`**（INT8 轻量模型：ECAPA-TDNN Lite / Tiny-CAM++） | `esp-tflite-micro` (ESP-NN) | ESP-DL 针对乐鑫芯片优化；先以唤醒词文本相关（1.0~1.5 s PCM）模式快速落地 |
| **音频编解码** | **`espressif/esp_audio_codec`**（Opus + 重采样）+ **`AudioTransportCodec::kPcm16_20ms`** | `libopus` | 网关模式走上行 16 kHz / 下行 24 kHz Opus；EZ Assistant 直连模式采用 20 ms 16 kHz Mono PCM16 零转码直传，降本降延迟 |
| **局域网服务发现** | **`espressif/mdns`**（双向发现：`_ez-device._tcp` 广播 + `_ez-assistant._tcp` / `_esp-gateway._tcp` 解析） | UDP Broadcast | 终端未配对时自动向桌面端可见；配对后按 `installation_id` 自动追踪桌面端动态端口 |
| **Wi-Fi 管理与双模配网** | **NVS 多网络凭据（`wifi_store`）+ SoftAP Captive Portal（`esp_http_server` + UDP:53 DNS）+ Mosaico 原生触摸屏 Wi-Fi/软键盘 UI** | 仅 Kconfig 静态 SSID 或仅 BLE 配网 | 支持最多 8 组 Wi-Fi 记忆与按信号强弱自动故障转移；同时提供设备屏内直接切网/输密码与零 App 手机 Captive Portal 配网，配网成功后自动关闭热点 |
| **设备身份与认证** | **RFC 8032 Ed25519（`ed25519_lite` + TLS 证书 SHA-256 指纹钉扎）** + **出厂 eFuse 硬件 MAC** | 后续可叠加 `ECDSA_DS`/`HMAC` 签名 | 直连模式走 Device Protocol 1.0 `direct_confirm` 免码一键绑定与加密 NVS（`ez_pair`）持久化；网关模式强制校验 eFuse MAC 白名单 |
| **通信接入协议** | **`EzDeviceClient`（Device Protocol 1.0 直连 WSS `/device`）** + `NetClient`（网关 `/ws/device` + HTTP JPEG 上传） | Phase 2+ 可扩展 `esp-mqtt` + UDP | `net_client` 组件按是否配置网关覆盖地址自动切换直连与集群网关双模，大图走独立 HTTP POST 不阻塞音频 |
| **显示与视觉** | **`esp_lcd` + LVGL 9**（ESP32-S31 PPA 加速）+ **`esp_video` / 硬件 JPEG** | OLED (SSD1306) | 利用 ESP32-S31 硬件 JPEG 编解码和 PPA 卸载 CPU 负载 |
| **本地存储** | **加密 NVS** + 只读模型分区（SPIFFS / Raw Partition） | LittleFS | 加密 NVS 保护 Wi-Fi 凭据与端侧声纹模板库 |

---

## 2. 语音与设备网关层（Voice & Device Gateway）

| 模块 | 选定方案 | 备选方案 | 选型理由 |
| :--- | :--- | :--- | :--- |
| **开发语言与框架** | **Go（1.23+）+ `go-kratos/kratos` v2** | — | Kratos 提供标准微服务工程骨架（`api/cmd/internal/{biz,data,service,server}`）、Wire 依赖注入、Protobuf 代码生成、统一 `kratos.App` 多 Server 生命周期管理与 OpenTelemetry 观测性 |
| **内部点对点 RPC** | **Kratos 原生 gRPC（Protobuf v3）** | — | 开箱即用享受 Kratos 全套中间件（鉴权/限流/Recovery/Tracing/Metrics），零自定义 RPC Transport 胶水代码 |
| **事件 / 广播总线与状态** | **NATS Server（启用 JetStream + KV）** | Redis Streams | 专职负责多机唤醒仲裁事件、多终端声纹群组广播（`voiceprint.sync.<group_id>`）、设备上下线事件及轻量 Device Shadow KV 缓存 |
| **局域网 mDNS 广播** | **`grandcat/zeroconf`**（封装为 Kratos `transport.Server`） | `hashicorp/mdns` | 纯 Go 无 CGo 依赖，随 `kratos.App` 自动启停广播 `_esp-gateway._tcp.local` |
| **HTTP / WebSocket 接入** | **Kratos `http.Server`** + 挂载原生 **`coder/websocket` Handler** | `gorilla/websocket` | Kratos HTTP 承载管理后台 REST API 与摄像头媒体上传；ESP32 WebSocket 音频长连接通过 `srv.Handle("/ws/device", ...)` 挂载，在握手阶段严格校验 `X-Device-MAC` 白名单 |
| **Device MCP Server** | **Go MCP SDK（`modelcontextprotocol/go-sdk` 或 `mark3labs/mcp-go`）**，挂载至 Kratos HTTP（**Streamable HTTP** `/mcp`）+ 可选 **stdio** | 独立 Python MCP 代理 | 内嵌于网关，将在线设备池封装为标准 MCP Tools（含 `device.fleet_list` 及 `target` 寻址）与 `fleet://topology` Resources，并在 `DeviceHello.mcp_servers` 中向已配对 EZ Assistant 桌面端自动声明 |

---

## 3. 语音 AI、声纹复核与数据存储

| 模块 | 推荐方案（按场景二选一或混合） | 说明 |
| :--- | :--- | :--- |
| **流式 ASR** | **DeepSeek ASR**（`POST /v1/tools/asr`，Ogg Opus / WAV）、**OpenAI Whisper** 或 **本地离线**（FunASR / `sherpa-onnx`） | 通过统一 `ASRProvider` 接口抽象，支持开箱切换 `deepseek` / `openai` / `http` / `mock` |
| **流式 TTS** | **DeepSeek 流式 WebSocket TTS**（`wss://api.deepseek.com/v1/tools/tts`，裸 Opus 下行，`stella` / `tide`）、**OpenAI TTS** 或本地 | 通过统一 `TTSProvider` 接口抽象，支持按说话人 `SpeakerGroup` 动态切换音色 |
| **服务端声纹复核** | **独立 `voice-server`**（`sherpa-onnx` 3D-Speaker 192 维 + **TEN-VAD**） | 复核端侧声纹灰区样本 & 支持 `LiteAudioEngine` 设备，提供 HTTP 注册与 WS `/api/v1/speaker/stream` 流式识别 |
| **声纹向量库** | **Qdrant**（192 维，Cosine 距离） | 按 `(uid, agent_id)` 隔离，单机 Docker 轻量运行 |
| **关系元数据存储** | **SQLite（单机边缘首选）** / **PostgreSQL（多节点云端）** | 存储 `allowed_macs` 白名单、设备拓扑（`group_id` / `zone`）、`speaker_groups`（`prompt` / `voice` / `role`）、Agent ACL 矩阵与 `gateway_settings` 动态系统提示词 |

---

## 4. Agent 大脑层（`Device Protocol 1.0` + `agent/ez-bridge` 双通道接入）

> **当前状态**：网关通过 `RoutingAgentAdapter` 统一支持两条对接通道：
> 1. **局域网 EZ Assistant 桌面端直连桥接（`Device Protocol 1.0`，推荐）**：在 [`backend/gateway/internal/data/ezdevice_bridge.go`](../backend/gateway/internal/data/ezdevice_bridge.go) 中实现 `grandcat/zeroconf` `_ez-assistant._tcp.local.` 发现（解析 `pairing_modes`）、TLS 叶证书 SHA-256 钉扎 WSS、免码一键确认（`direct_confirm`，Ed25519 公钥指纹确认）与自动回退 SPAKE2 P-256 6 位码配对、Ed25519 长期身份认证；在认证 `DeviceHello.mcp_servers` 中自动向桌面端声明网关 Streamable HTTP `/mcp`（对端 IP 钉扎、默认 `default-ez-assistant-owner` ACL 种子及 `device.fleet_list` 工具），以纯文字虚拟终端身份接入桌面端主控会话，并在 Web 控制台提供扫描、配对模式/状态展示与 `auto` / `ez_device` / `grpc` 路由切换。
> 2. **gRPC 双向流桥接（`agent/ez-bridge`）**：已在 [`agent/ez-bridge`](../agent/ez-bridge/) 实现 `proto/agent/v1` gRPC 双向流桥接，支持接入本机 `ez-assistant-runtime`（`--provider ez-assistant --runtime-home ~/.ez-assistant`）与直接接入 DeepSeek Chat Completions（`--provider deepseek`），并通过 Web 控制台 `GET/PUT /api/v1/settings/prompt` 支持全局/分说话人提示词逐轮实时热更新。

为保证后续零改造成本切换任意 Agent 实现，网关层继续保持以下**语言无关的标准对接契约**：

1. **会话与事件契约（gRPC 双向流 / NATS 事件）**：
   * 网关 → Agent：`SendTurn(session_id, text, speaker_id, confidence, source_mac, zone, shadow_snapshot, system_prompt)`、`SendEvent(event_type, payload)`、`Cancel(session_id)`。
   * Agent → 网关（流式事件）：`OnStatus(state, msg)`、`OnTextDelta(chunk)`、`OnUIUpdate(target, display)`、`OnDone()`。
2. **硬件能力调用契约（Device MCP Server）**：
   * 无论自研 Agent 使用 Python、Go、TypeScript 还是 Rust，均可直接通过标准 **MCP（Streamable HTTP / stdio）** 发现并调用 `fleet://topology` 及所有终端的摄像头、传感器、执行器与声纹管理工具。
3. **EZ Assistant Desktop 控制面板代理与双向语音中继（Device Protocol 1.0 扩展）**：
   * 网关向已配对 `ez-assistant` Host 的 `DeviceHello` 声明 `console` 与 `speech_relay: true`；Host 提供固定 `peer_ip` 的 `/devices/{device_id}/console/{*path}` 受控反代（禁重定向、`settings/deepseek` 屏蔽、10 MiB 上限），并在 WSS 上扩展 `speech_asr_*`（kind=3 变长 PCM16 上行）与 `speech_tts_*`（kind=4 变长 PCM16 下行）中继帧，网关侧结合纯 Go WAV 重采样与 `ffmpeg` Opus 编解码复用 Desktop 语音服务并保留本地回退。

---

## 5. 本地开发与部署环境

* **基础设施编排**：**Docker Compose**（`nats` + `qdrant` + 可选 `voice-server`）。
* **macOS 本地联调注意**：Docker Desktop 桥接网络不支持向局域网广播 mDNS，因此在 macOS 上开发联调时，**`voice-gateway`（Kratos 应用）直接在宿主机原生运行**（利用 macOS Bonjour 广播 `_esp-gateway._tcp.local`），Linux 生产环境容器则配置 `network_mode: host`。
* **可观测性**：Kratos 内置 `log` + **OpenTelemetry Tracing / Metrics**，覆盖端到端语音延迟埋点（`wake -> arbiter -> asr_first_text -> agent_ttft -> tts_first_frame`）。

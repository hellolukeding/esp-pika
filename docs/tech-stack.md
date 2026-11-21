# ESP32-S31 智能语音与多模态 Agent 终端技术选型文档

> **版本**：v1.1  
> **关联架构**：[architecture.md](./architecture.md)（v1.3）  
> **状态**：固件层、网关与微服务层（**Go Kratos 原生 gRPC + NATS 事件总线**）、数据存储层已定型；**Agent 大脑层待定（后续接入自研 Agent 代码，已预留语言无关的 gRPC / NATS + MCP 接入契约）**。

---

## 1. 终端固件层（ESP32-S31）

| 模块 | 选定方案 | 备选方案 | 选型理由 |
| :--- | :--- | :--- | :--- |
| **SDK 与语言** | **ESP-IDF v6.x + C++20 / C 混合** | 纯 C | 底层驱动与 ISR 用 C，Board 抽象 / AudioService / 状态机用 C++（兼容 `xiaozhi-esp32` 风格，便于多板型扩展） |
| **构建与环境** | **EIM + CMake + Ninja + `idf.py`** | — | ESP-IDF v6.x 官方推荐工作流，跨 macOS / Linux / Windows 一致 |
| **音频前处理 / VAD / 唤醒** | **`espressif/esp-sr`**（AFE: AEC + NS/BSS + VADNet + WakeNet / MultiNet） | Silero VAD (TFLM) | 官方量化优化流水线，统一处理回声消除、降噪、VAD 与离线唤醒 |
| **端侧声纹识别** | **`espressif/esp-dsp`**（FBank/MFCC）+ **`espressif/esp-dl`**（INT8 轻量模型：ECAPA-TDNN Lite / Tiny-CAM++） | `esp-tflite-micro` (ESP-NN) | ESP-DL 针对乐鑫芯片优化；先以唤醒词文本相关（1.0~1.5 s PCM）模式快速落地 |
| **音频编解码** | **`espressif/esp_audio_codec`**（Opus + 重采样） | `libopus` | 官方维护组件，支持上行 16 kHz Opus 编码、下行 24 kHz Opus 解码与采样率对齐 |
| **局域网服务发现** | **`espressif/mdns`** | UDP Broadcast | 自动发现局域网网关 `_esp-gateway._tcp.local`，免硬编码 IP |
| **设备身份与认证** | **出厂 eFuse 硬件 MAC**（`esp_efuse_mac_get_default()`） | 后续可叠加 `ECDSA_DS`/`HMAC` 签名 | 全局唯一、免烧录凭证；所有通道强制校验 MAC 白名单 |
| **通信接入协议** | **Phase 1: `esp_websocket_client`（WSS）** + **`esp_http_client`（JPEG 上传）** | Phase 2+ 可扩展 `esp-mqtt` + UDP | WS 单连接同时跑控制 JSON 与 Opus 二进制帧，联调成本最低；大图走独立 HTTP POST 不阻塞音频 |
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
| **Device MCP Server** | **Go MCP SDK（`modelcontextprotocol/go-sdk` 或 `mark3labs/mcp-go`）**，挂载至 Kratos HTTP（**Streamable HTTP**）+ 可选 **stdio** | 独立 Python MCP 代理 | 内嵌于网关，零额外跳数将在线设备池封装为标准 MCP Tools（`target` 寻址）与 `fleet://topology` Resources |

---

## 3. 语音 AI、声纹复核与数据存储

| 模块 | 推荐方案（按场景二选一或混合） | 说明 |
| :--- | :--- | :--- |
| **流式 ASR** | **云端 API**（阿里云 Paraformer / 火山豆包 / Deepgram）或 **本地离线**（FunASR / `sherpa-onnx`） | 通过统一 `ASRProvider` 接口抽象，Phase 1 可先接云 API 快速跑通，后续无缝切本地 |
| **流式 TTS** | **云端 API**（火山 / 阿里云 CosyVoice / OpenAI TTS）或 **本地离线**（CosyVoice 2 / Kokoro） | 通过统一 `TTSProvider` 接口抽象，支持按说话人 `SpeakerGroup` 动态切换音色 |
| **服务端声纹复核** | **独立 `voice-server`**（`sherpa-onnx` 3D-Speaker 192 维 + **TEN-VAD**） | 复核端侧声纹灰区样本 & 支持 `LiteAudioEngine` 设备，提供 HTTP 注册与 WS `/api/v1/speaker/stream` 流式识别 |
| **声纹向量库** | **Qdrant**（192 维，Cosine 距离） | 按 `(uid, agent_id)` 隔离，单机 Docker 轻量运行 |
| **关系元数据存储** | **SQLite（单机边缘首选）** / **PostgreSQL（多节点云端）** | 存储 `allowed_macs` 白名单、设备拓扑（`group_id` / `zone`）、`speaker_groups`（`prompt` / `voice` / `role`）、Agent ACL 矩阵 |

---

## 4. Agent 大脑层（待定 — 后续接入自研 Agent）

> **当前状态**：**待定（TBD）**，待接入用户自研 Agent 代码后再细化内部选型。

为保证后续零改造成本接入自研 Agent，网关层预先锁定以下**语言无关的标准对接契约**：

1. **会话与事件契约（gRPC 双向流 / NATS 事件）**：
   * 网关 → Agent：`SendTurn(session_id, text, speaker_id, confidence, source_mac, zone, shadow_snapshot)`、`SendEvent(event_type, payload)`、`Cancel(session_id)`。
   * Agent → 网关（流式事件）：`OnStatus(state, msg)`、`OnTextDelta(chunk)`、`OnUIUpdate(target, display)`、`OnDone()`。
2. **硬件能力调用契约（Device MCP Server）**：
   * 无论自研 Agent 使用 Python、Go、TypeScript 还是 Rust，均可直接通过标准 **MCP（Streamable HTTP / stdio）** 发现并调用 `fleet://topology` 及所有终端的摄像头、传感器、执行器与声纹管理工具。

---

## 5. 本地开发与部署环境

* **基础设施编排**：**Docker Compose**（`nats` + `qdrant` + 可选 `voice-server`）。
* **macOS 本地联调注意**：Docker Desktop 桥接网络不支持向局域网广播 mDNS，因此在 macOS 上开发联调时，**`voice-gateway`（Kratos 应用）直接在宿主机原生运行**（利用 macOS Bonjour 广播 `_esp-gateway._tcp.local`），Linux 生产环境容器则配置 `network_mode: host`。
* **可观测性**：Kratos 内置 `log` + **OpenTelemetry Tracing / Metrics**，覆盖端到端语音延迟埋点（`wake -> arbiter -> asr_first_text -> agent_ttft -> tts_first_frame`）。

# esp-pika

基于 **ESP32-S31（ESP-IDF v6.x）** + **Go Kratos v2（gRPC / HTTP）** + **NATS JetStream** + **MCP** 的多终端智能语音与多模态 AI Agent 硬件系统，并预接自研 Agent Runtime（[ez-assistant](./ez-assistant)）。

## 核心文档

* [工程与代码规范（AGENTS.md）](./AGENTS.md)
* [系统架构设计文档（v1.3）](./docs/architecture.md)
* [技术选型文档（v1.1）](./docs/tech-stack.md)

## 仓库目录结构

| 目录 | 说明 |
| :--- | :--- |
| [docs/](./docs/) | 架构设计、技术选型与协议文档 |
| [proto/](./proto/) | 跨服务 Protobuf v3 协议定义（单一事实源） |
| [firmware/](./firmware/) | ESP32-S31 终端固件（ESP-IDF v6.x，含双板型 HAL、mDNS/eFuse MAC、Core 0 AFE/Pre-roll/Opus 与 Core 1 端侧声纹/加密 NVS） |
| [backend/gateway/](./backend/gateway/) | Voice & Device Gateway（Go Kratos v2 + Wire + SQLite + NATS JetStream + `/ws/device` + `/mcp` + 流式 ASR/TTS 管线） |
| [backend/voice-server/](./backend/voice-server/) | 云端灰区声纹复核服务（192 维 Cosine Embedding + Qdrant + HTTP / WS `/api/v1/speaker/stream`） |
| [agent/ez-bridge/](./agent/ez-bridge/) | `AgentSessionService` gRPC ↔ `ez-assistant-runtime` HTTP/SSE 桥接适配服务 |
| [ez-assistant/](./ez-assistant/) | 自研 Agent Runtime（Rust Workspace） |
| [deploy/](./deploy/) | 本地开发与边缘部署编排（Docker Compose / NATS / Qdrant + `ez-assistant.mcp.example.json`） |

## 常用构建与验证命令

```bash
# 1. 启动本地 NATS JetStream 与 Qdrant
make infra-up

# 2. 生成 Protobuf 与 Wire 依赖注入代码
make proto
make wire

# 3. 编译与运行测试（网关 / 声纹服务 / Agent 桥接 / 固件宿主机 C++20 单测）
make gateway-build && cd backend/gateway && go test -count=1 -v ./...
make voice-server-build && make voice-server-test
make agent-build && make agent-test
make firmware-host-test
```

## 本地全栈联调启动顺序

1. 在 `~/.ez-assistant/mcp.json` 中填入 [deploy/local/ez-assistant.mcp.example.json](./deploy/local/ez-assistant.mcp.example.json)，并启动已配置模型的 `ez-assistant-runtime serve`；
2. 启动桥接与网关：
   ```bash
   ./agent/ez-bridge/bin/ez-bridge --listen :9100 --mcp-server-key esp-pika
   ./backend/gateway/bin/gateway -conf backend/gateway/configs/config.example.yaml
   ```
3. 在激活的 ESP-IDF v6.x 环境中烧录固件：
   ```bash
   cd firmware && idf.py set-target esp32s31 && idf.py build flash monitor
   ```

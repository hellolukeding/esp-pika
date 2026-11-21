# AGENTS.md — `esp-pika` 工程与代码规范

本文件为所有开发者与 AI 编程助手在 `esp-pika` 仓库中工作的**最高权威规范**。任何架构调整、目录新增或代码编写都必须严格遵守本文件。

---

## 1. 权威设计文档（改动前必读）

* 系统整体架构（端-网-脑三层、端侧 VAD/声纹、M:N 多终端拓扑、mDNS + MAC 认证）：[docs/architecture.md](./docs/architecture.md)
* 全栈技术选型（ESP-IDF v6.x、Go Kratos v2 gRPC + NATS JetStream、MCP）：[docs/tech-stack.md](./docs/tech-stack.md)

> **铁律**：涉及跨端协议、架构边界或技术选型的变更，必须**先更新 `docs/` 与 `proto/`，再改实现代码**。

---

## 2. Monorepo 目录规范与职责边界

```text
esp-pika/
├── AGENTS.md                         # 本规范文件
├── README.md                         # 项目总览与快速入口
├── .editorconfig                     # 跨语言编辑器格式规范
├── .gitignore                        # Git 忽略规则
├── docs/                             # 架构、技术选型、协议与运维文档
├── proto/                            # 跨服务 Protobuf v3 协议单一事实源
│   ├── device/v1/                    # 设备注册、能力清单、状态影子、控制指令
│   ├── gateway/v1/                   # 网关管理 API（MAC 白名单、拓扑、声纹组）
│   ├── agent/v1/                     # 网关 <-> 自研 Agent 双向流式会话契约
│   └── events/v1/                    # NATS JetStream 事件与广播消息
├── firmware/                         # ESP32-S31 终端固件（ESP-IDF v6.x 标准布局）
│   ├── main/                         # 仅放 app_main 与顶层应用组装/状态机
│   ├── components/                   # 项目自定义组件（严禁把驱动/网络塞进 main/）
│   │   ├── board/                    # 开发板硬件抽象（CoreBoard-1 / Korvo-1）
│   │   ├── audio_engine/             # AFE / VADNet / WakeNet / Opus 音频流水线
│   │   ├── voiceprint/               # 端侧声纹（esp-dsp + esp-dl INT8 + 加密 NVS）
│   │   ├── net_client/               # mDNS 发现 + MAC 认证 + WS/HTTP 媒体上传
│   │   └── local_executor/           # 本地外设执行器（摄像头/传感器/GPIO/LCD）
│   ├── partitions/                   # 自定义 Flash 分区表 CSV
│   └── configs/                      # sdkconfig.defaults 与芯片特定默认配置
├── backend/                          # Go Kratos v2 后端服务
│   └── gateway/                      # Voice & Device Gateway（标准 Kratos 布局）
│       ├── api/                      # 由 proto/ 生成的 Go/gRPC/HTTP 代码（禁止手改）
│       ├── cmd/gateway/              # 入口 main.go 与 Wire 依赖注入
│       ├── configs/                  # config.yaml 配置模板
│       └── internal/
│           ├── biz/                  # 核心领域逻辑与仓储接口（零外部框架耦合）
│           ├── data/                 # 数据层实现（SQLite/PG, NATS KV, Qdrant）
│           ├── service/              # gRPC / HTTP Service 入口（仅做 DTO 转换与校验）
│           ├── server/               # Transport 层（Kratos HTTP/gRPC + WS + MCP + mDNS）
│           └── conf/                 # Kratos Proto 配置结构体
├── agent/                            # 自研 Agent 大脑层（待接入，独立模块）
└── deploy/                           # 本地与边缘部署编排（Docker Compose / NATS / Qdrant）
```

### 目录红线
1. **禁止跨层污染**：
   * `firmware/main/` 只做初始化组装与顶层状态机，**严禁**直接堆砌 I2S/摄像头/网络协议实现，必须按职责拆入 `firmware/components/<module>/`。
   * `backend/gateway/` 严格遵守 Kratos 洁净分层：`server` → `service` → `biz` → `data`。`biz` 层**严禁** import `data`、`server` 或具体数据库/网络驱动。
2. **禁止手改生成文件**：`*.pb.go`、`*_grpc.pb.go`、`*_http.pb.go`、`wire_gen.go` 均由工具生成，严禁手工编辑。
3. **单一事实源**：所有后端 gRPC、管理 REST API 与 NATS 事件结构统一定义在根目录 `proto/` 下。

---

## 3. 分语言代码规范

### 3.1 ESP32-S31 固件（C / C++20 / ESP-IDF v6.x）

* **文件与命名**：
  * C 源文件用 `.c`/`.h`，C++ 源文件用 `.cc`/`.h`；所有头文件统一使用 `#pragma once`。
  * 目录与文件名一律 `snake_case`（如 `audio_engine.cc`、`mac_auth.h`）。
  * C++ 类与结构体用 `PascalCase`，成员函数用 `PascalCase`，私有成员变量用 `snake_case_`（尾下划线），常量用 `kPascalCase` 或 `UPPER_SNAKE_CASE`。
  * C 导出接口用 `snake_case` 且带模块前缀（如 `vp_engine_init()`）。
* **错误处理**：
  * 可恢复错误统一返回 `esp_err_t`，使用 `ESP_RETURN_ON_ERROR`、`ESP_GOTO_ON_ERROR`、`ESP_RETURN_ON_FALSE` 宏处理；
  * 仅启动阶段不可恢复的致命初始化失败才允许使用 `ESP_ERROR_CHECK`。
* **硬件抽象与分层**：
  * 业务组件统一调用 ESP-IDF `driver/` 公共 API，**严禁**直接包含 `soc/*_reg.h` 或 `hal/*_ll.h` 私有头文件（除非经评审的极少数底层组件）。
  * 所有配置结构体使用 C99/C++ 指定初始化器或官方 `*_DEFAULT_CONFIG()` 宏，防止未初始化字段。
* **内存、DMA 与双核 SMP 约束**：
  * I2S / 外设 DMA 缓冲区必须显式分配在片内 DRAM（`MALLOC_CAP_DMA` / `DMA_ATTR`）；
  * Pre-roll 缓冲、Opus 队列、声纹 Tensor Arena、相机帧缓冲、LCD 显存放入外挂 PSRAM（`MALLOC_CAP_SPIRAM`）；
  * **严禁**在栈位于 PSRAM 的 FreeRTOS 任务中直接触发 Flash 写/擦除操作（写 Flash 会临时禁用 Cache）；
  * 双核隔离：**Core 0** 专跑强实时音频（`AudioInputTask` / AFE / Opus / `AudioOutputTask`），**Core 1** 跑声纹推理、网络、状态机与 UI；
  * 关闭 C++ 异常与 RTTI，采用 RAII 管理资源句柄。
* **组件依赖**：
  * 在 `CMakeLists.txt` 中明确区分 `REQUIRES`（公开头文件依赖）与 `PRIV_REQUIRES`（仅实现依赖），避免头文件污染与编译膨胀。

### 3.2 Go 后端（Go 1.23+ / Kratos v2）

* **格式与静态检查**：所有代码必须通过 `gofmt`、`goimports` 与 `golangci-lint`。
* **命名与包结构**：
  * 包名全小写单词，不带下划线（如 `arbiter`、`shadow`、`mcphub`）；
  * 接口定义在使用方（`internal/biz`），实现放在提供方（`internal/data` 或 `internal/server`）；
  * 导出符号必须有规范的 GoDoc 注释。
* **并发与生命周期**：
  * 所有 I/O 与 RPC 函数第一个参数必须是 `ctx context.Context`；
  * **严禁**起不受控的野 `goroutine`：所有长驻后台协程（WS 读写循环、NATS 订阅、mDNS 广播、唤醒仲裁定时器）必须绑定到实现了 Kratos `transport.Server` 的组件中，随 `Stop(ctx)` 优雅退出且无泄漏；
  * WebSocket 音频长连接通过 `http.Server.Handle("/ws/device", ...)` 挂载，严禁硬塞进 Proto HTTP 编解码路由。
* **日志与错误**：
  * 统一使用 Kratos `log.NewHelper(logger)` 输出结构化日志（`WithContext(ctx)`），禁止 `fmt.Println` / `log.Printf`；
  * 错误包装使用 `fmt.Errorf("...: %w", err)` 或 Kratos `errors`（`.proto` 定义的业务错误码），禁止吞掉错误。

### 3.3 Protobuf 规范（`proto/`）

* 遵循 Google AIP / Buf 规范：
  * `syntax = "proto3";`
  * 包名带版本号：`package esp_pika.device.v1;`
  * 文件名 `snake_case.proto`，Message `PascalCase`，字段 `snake_case`，枚举值 `UPPER_SNAKE_CASE` 且必须定义 `*_UNSPECIFIED = 0`。
  * 禁止复用已删除字段编号，删除字段必须标记 `reserved`。

---

## 4. 安全与架构硬性红线

1. **严格 MAC 唯一认证**：
   * 终端设备只能以出厂 eFuse MAC（`esp_efuse_mac_get_default()`）作为唯一 `device_id`；
   * 控制面（WS/MQTT）与媒体面（HTTP POST）全部强制校验网关 `allowed_macs` 白名单与同 MAC 单连接互斥，**严禁**预留免认证调试后门。
2. **三平面严格分离**：
   * 禁止将摄像头 JPEG 大图或高频传感器波形塞入控制面 JSON 或实时 Opus 音频帧，大图必须走独立 HTTP POST 媒体面。
3. **声纹与密钥安全**：
   * 严禁将真实 API Key、TLS 私钥、真实声纹样本/数据库文件提交到 Git；配置文件仅提交 `*.example.yaml` 模板；
   * 设备端声纹模板必须存入加密 NVS。
4. **多终端 ACL 护栏**：
   * 任何 Agent 调用 Device MCP 工具前，必须经过网关 `Agent → Device ACL` 校验；高危外设操作须校验 `speaker_role == owner` 并支持语音二次确认。

---

## 5. Git 提交与验证规范

* **Commit 规范**：采用 Conventional Commits，作用域与顶层目录对齐：
  * `feat(firmware): ...` / `fix(gateway): ...` / `refactor(proto): ...` / `docs: ...` / `chore(deploy): ...`
* **提交前自检**：
  * Go 代码变更：`go test ./...` 与 `go vet ./...` 必须通过；
  * Proto 变更：重新生成代码并确认无手改差异；
  * 固件变更：在激活的 ESP-IDF 环境中确认 `idf.py build` 通过（或主机端单元测试通过）。

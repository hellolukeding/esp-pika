# backend/gateway — Voice & Device Gateway（Go Kratos v2）

遵循 `go-kratos/kratos` v2 标准分层架构：

* `api/`：由根目录 `proto/` 自动生成的 Go / gRPC / HTTP 代码（禁止手改）
* `cmd/gateway/`：程序入口 `main.go` 与 Wire 依赖注入配置
* `configs/`：运行配置模板（仅提交 `config.example.yaml`，真实配置忽略）
* `internal/conf/`：Kratos Proto 配置结构体
* `internal/server/`：Transport 接入层（Kratos HTTP/gRPC + 原生 WebSocket Handler + Device MCP Server + `zeroconf` mDNS Server）
* `internal/service/`：gRPC / HTTP Service 实现（DTO 转换与参数校验）
* `internal/biz/`：核心领域逻辑（MAC 白名单鉴权、多机唤醒仲裁、M:N 会话路由、ACL 权限矩阵、声纹管理），不依赖具体存储或框架实现
* `internal/data/`：数据与外部客户端实现（SQLite/PostgreSQL、NATS JetStream KV/PubSub、Qdrant、ASR/TTS Provider）

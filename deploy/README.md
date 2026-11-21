# deploy — 本地开发与边缘部署编排

* `local/`：Docker Compose 编排（NATS JetStream、Qdrant、可选 `voice-server`）及服务配置模板。

> **macOS 本地联调提示**：Docker Desktop 桥接网络不支持局域网 mDNS 广播，macOS 开发时请将 `backend/gateway` 直接在宿主机原生运行，NATS / Qdrant 跑在 Docker 中；Linux 生产环境可为网关容器开启 `network_mode: host`。

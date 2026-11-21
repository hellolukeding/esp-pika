# proto — 跨服务 Protobuf v3 协议单一事实源

所有 Kratos gRPC/HTTP API 与 NATS JetStream 事件结构均定义在此目录，严禁手改生成的 `*.pb.go` 文件。

## 目录说明

* `device/v1/`：设备注册（`device.hello`）、能力清单、状态影子、设备控制指令
* `gateway/v1/`：网关管理 API（MAC 白名单、设备群拓扑、声纹组管理）
* `agent/v1/`：网关 ↔ 自研 Agent 的双向流式会话与事件契约
* `events/v1/`：NATS JetStream 广播与事件消息（多机唤醒仲裁、声纹增量同步、传感器 Push）

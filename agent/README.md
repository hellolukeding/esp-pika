# agent — Agent 大脑接入层

本目录放置 Agent 大脑适配层，通过 `proto/agent/v1/` (`AgentSessionService`) 与 Gateway 解耦。

## 支持模式 (`agent/ez-bridge`)

1. **DeepSeek 直连模式 (`--provider deepseek`)**：直接调用 DeepSeek Chat Completions API（`POST https://api.deepseek.com/chat/completions`，`stream: true` 流式响应 + 多轮 `tools` Function Calling），并将工具调用转发到网关 `/mcp`（自动映射 `device.*` ↔ `device__*` 且透传 `X-Agent-ID` / `X-Speaker-Role`）。通过 `DEEPSEEK_API_KEY`（或 `--deepseek-api-key`）启用。
2. **ez-assistant 桥接模式 (`--provider ez-assistant`)**：对接本地 `ez-assistant-runtime` HTTP/SSE 与 MCP Server。

## 协议契约

1. **会话与事件契约**：通过 `proto/agent/v1/` 定义的 gRPC 双向流接入网关，接收 `SendTurn` 与 `SendEvent`，流式返回 `OnStatus` / `OnTextDelta` / `OnUIUpdate` / `OnDone`。
2. **硬件能力契约**：通过网关暴露的 **Device MCP Server（Streamable HTTP `/mcp`）** 调用 `fleet://topology` 与各终端的外设及声纹工具。

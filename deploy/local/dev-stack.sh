#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

MODE="${1:-smoke}" # "smoke" (start, verify, stop) or "up" (run until Ctrl+C)
DEFAULT_GW_ADDR="127.0.0.1:8080"
if [ "$MODE" = "up" ]; then
  DEFAULT_GW_ADDR="0.0.0.0:8080"
fi
GATEWAY_HTTP_ADDR="${GATEWAY_HTTP_ADDR:-$DEFAULT_GW_ADDR}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/esp-pika-stack.XXXXXX")"
PIDS=()

cleanup() {
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
  done
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

echo "==> Checking NATS & Qdrant containers"
docker compose -f deploy/local/docker-compose.yml up -d

mkdir -p "$TMP_DIR/media"
ASR_PROVIDER="mock"
TTS_PROVIDER="mock"
DEFAULT_TTS_VOICE="zh_female_calm"
SPEECH_API_KEY_ENV="OPENAI_API_KEY"
if [ "$MODE" != "smoke" ] && [ -n "${DEEPSEEK_API_KEY:-}" ]; then
  ASR_PROVIDER="deepseek"
  TTS_PROVIDER="deepseek"
  DEFAULT_TTS_VOICE="stella"
  SPEECH_API_KEY_ENV="DEEPSEEK_API_KEY"
fi
cat > "$TMP_DIR/gateway.yaml" <<EOF
server:
  http:
    network: tcp
    addr: $GATEWAY_HTTP_ADDR
    timeout: 30s
  grpc:
    network: tcp
    addr: 127.0.0.1:9000
    timeout: 30s
  media_dir: $TMP_DIR/media
data:
  database:
    driver: sqlite3
    source: $TMP_DIR/gateway.db
  nats:
    url: nats://127.0.0.1:4222
    shadow_bucket: esp_pika_shadow
  qdrant:
    addr: 127.0.0.1:6334
    collection: speaker_embeddings
discovery:
  mdns_enabled: true
  service_name: esp-pika-gateway
  service_type: _esp-gateway._tcp
  domain: local.
  port: 8080
  group_id: home-default
  tls: false
arbiter:
  window: 0.12s
voice:
  agent_grpc_addr: 127.0.0.1:9100
  voice_server_url: http://127.0.0.1:8081
  speaker_threshold: 0.6
  asr_provider: $ASR_PROVIDER
  tts_provider: $TTS_PROVIDER
  default_tts_voice: $DEFAULT_TTS_VOICE
  speech_api_key_env: $SPEECH_API_KEY_ENV
EOF

echo "==> Starting voice-server on :8081 (Qdrant http://127.0.0.1:6333)"
./backend/voice-server/bin/voice-server --listen 127.0.0.1:8081 --qdrant http://127.0.0.1:6333 >"$TMP_DIR/voice-server.log" 2>&1 &
PIDS+=($!)

if [ "$MODE" = "smoke" ]; then
  if curl -sf http://127.0.0.1:7240/health >/dev/null 2>&1; then
    echo "==> Launching fleet-smoke against existing ez-assistant on :7240"
    ./agent/ez-bridge/bin/fleet-smoke \
      --gateway http://127.0.0.1:8080 \
      --voice-server http://127.0.0.1:8081 \
      --qdrant http://127.0.0.1:6333 \
      --nats-mon http://127.0.0.1:8222 &
    SMOKE_PID=$!
    PIDS+=("$SMOKE_PID")
  else
    echo "==> Launching fleet-smoke (with dev ez-assistant HTTP/SSE stub on :7240)"
    ./agent/ez-bridge/bin/fleet-smoke \
      --mock-ez-listen 127.0.0.1:7240 \
      --gateway http://127.0.0.1:8080 \
      --voice-server http://127.0.0.1:8081 \
      --qdrant http://127.0.0.1:6333 \
      --nats-mon http://127.0.0.1:8222 &
    SMOKE_PID=$!
    PIDS+=("$SMOKE_PID")
    sleep 0.3
  fi
fi

echo "==> Starting ez-bridge on :9100"
AGENT_PROVIDER="${AGENT_PROVIDER:-auto}"
EZ_RUNTIME_HOME="${EZ_RUNTIME_HOME:-$HOME/.ez-assistant}"
RESOLVED_PROVIDER="$AGENT_PROVIDER"
if [ "$MODE" = "smoke" ]; then
  RESOLVED_PROVIDER="ez-assistant"
elif [ "$AGENT_PROVIDER" = "auto" ]; then
  if [ -f "$EZ_RUNTIME_HOME/run/runtime.json" ]; then
    RESOLVED_PROVIDER="ez-assistant"
  elif [ -n "${DEEPSEEK_API_KEY:-}" ]; then
    RESOLVED_PROVIDER="deepseek"
  else
    RESOLVED_PROVIDER="ez-assistant"
  fi
fi

if [ "$RESOLVED_PROVIDER" = "deepseek" ]; then
  DEEPSEEK_MODEL="${DEEPSEEK_MODEL:-deepseek-flash}"
  DEEPSEEK_BASE_URL="${DEEPSEEK_BASE_URL:-https://api.deepseek.com}"
  ./agent/ez-bridge/bin/ez-bridge \
    --listen 127.0.0.1:9100 \
    --provider deepseek \
    --deepseek-url "$DEEPSEEK_BASE_URL" \
    --deepseek-model "$DEEPSEEK_MODEL" \
    --gateway-mcp-url http://127.0.0.1:8080/mcp >"$TMP_DIR/ez-bridge.log" 2>&1 &
  PIDS+=($!)
else
  if [ -n "${EZ_TOKEN:-}" ] || [ "$MODE" = "smoke" ]; then
    EZ_URL="${EZ_URL:-http://127.0.0.1:7240}"
    EZ_TOKEN="${EZ_TOKEN:-dev-token}"
    ./agent/ez-bridge/bin/ez-bridge \
      --listen 127.0.0.1:9100 \
      --provider ez-assistant \
      --ez-url "$EZ_URL" \
      --ez-token "$EZ_TOKEN" \
      --mcp-server-key esp-pika >"$TMP_DIR/ez-bridge.log" 2>&1 &
  else
    ./agent/ez-bridge/bin/ez-bridge \
      --listen 127.0.0.1:9100 \
      --provider ez-assistant \
      --runtime-home "$EZ_RUNTIME_HOME" \
      --mcp-server-key esp-pika >"$TMP_DIR/ez-bridge.log" 2>&1 &
  fi
  PIDS+=($!)
fi

echo "==> Starting gateway on :8080 (HTTP/WS/MCP) & :9000 (gRPC)"
./backend/gateway/bin/gateway -conf "$TMP_DIR/gateway.yaml" >"$TMP_DIR/gateway.log" 2>&1 &
PIDS+=($!)

if [ "$MODE" = "smoke" ]; then
  wait "$SMOKE_PID"
  echo "==> Smoke run complete (all temporary processes stopped cleanly)"
else
  echo "==> Stack running. Press Ctrl+C to stop."
  wait
fi

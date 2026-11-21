PROTOC ?= protoc
GOPATH ?= $(shell go env GOPATH)
PATH := $(GOPATH)/bin:$(PATH)

PROTO_ROOT := proto
PROTO_OUT := backend/gateway/api
GATEWAY_DIR := backend/gateway

PROTO_FILES := \
	$(PROTO_ROOT)/device/v1/device.proto \
	$(PROTO_ROOT)/gateway/v1/gateway.proto \
	$(PROTO_ROOT)/agent/v1/agent.proto \
	$(PROTO_ROOT)/events/v1/events.proto

.PHONY: proto wire gateway-test gateway-vet gateway-build voice-server-test voice-server-build agent-test agent-build firmware-host-test stack-smoke infra-up infra-down

proto:
	@mkdir -p $(PROTO_OUT)
	$(PROTOC) \
		-I $(PROTO_ROOT) \
		-I $(PROTO_ROOT)/third_party \
		--go_out=$(PROTO_OUT) --go_opt=paths=source_relative \
		--go-grpc_out=$(PROTO_OUT) --go-grpc_opt=paths=source_relative \
		--go-http_out=$(PROTO_OUT) --go-http_opt=paths=source_relative \
		$(PROTO_FILES)
	$(PROTOC) \
		-I $(GATEWAY_DIR)/internal/conf \
		-I $(PROTO_ROOT)/third_party \
		--go_out=$(GATEWAY_DIR)/internal/conf --go_opt=paths=source_relative \
		$(GATEWAY_DIR)/internal/conf/conf.proto

wire:
	cd $(GATEWAY_DIR)/cmd/gateway && PATH="$(PATH)" wire

gateway-test:
	cd $(GATEWAY_DIR) && go test -v ./...

gateway-vet:
	cd $(GATEWAY_DIR) && go vet ./...

gateway-build:
	cd $(GATEWAY_DIR) && go build -o bin/gateway ./cmd/gateway

voice-server-test:
	cd backend/voice-server && go test -count=1 -v ./...

voice-server-build:
	cd backend/voice-server && go build -o bin/voice-server ./cmd/voice-server

agent-test:
	cd agent/ez-bridge && go test -count=1 -v ./...

agent-build:
	cd agent/ez-bridge && go build -o bin/ez-bridge ./cmd/ez-bridge && go build -o bin/fleet-smoke ./cmd/fleet-smoke

stack-smoke: gateway-build voice-server-build agent-build
	bash deploy/local/dev-stack.sh smoke

firmware-host-test:
	cmake -S firmware/tests/host -B firmware/build_host
	cmake --build firmware/build_host
	ctest --test-dir firmware/build_host --output-on-failure

infra-up:
	docker compose -f deploy/local/docker-compose.yml up -d

infra-down:
	docker compose -f deploy/local/docker-compose.yml down

package main

import (
	"context"
	"flag"
	"fmt"
	"net"
	"os"
	"os/signal"
	"strings"
	"syscall"

	"esp-pika/agent/ez-bridge/bridge"
	"esp-pika/agent/ez-bridge/deepseek"
	"esp-pika/agent/ez-bridge/ezruntime"
	agentv1 "esp-pika/backend/gateway/api/agent/v1"

	"google.golang.org/grpc"
)

func main() {
	listenAddr := flag.String("listen", ":9100", "gRPC listen address for gateway AgentSessionService")
	provider := flag.String("provider", "auto", "agent backend: auto, deepseek, or ez-assistant")
	agentID := flag.String("agent-id", "ez-assistant", "agent_id identity passed to gateway MCP and ACL checks")

	// Direct DeepSeek Chat Completions API options.
	deepseekURL := flag.String("deepseek-url", "https://api.deepseek.com", "DeepSeek API base URL")
	deepseekKey := flag.String("deepseek-api-key", "", "DeepSeek API key (defaults to DEEPSEEK_API_KEY env var)")
	deepseekModel := flag.String("deepseek-model", "deepseek-flash", "DeepSeek chat completion model")
	gatewayMCPURL := flag.String("gateway-mcp-url", "http://127.0.0.1:8080/mcp", "gateway Streamable HTTP MCP endpoint for tool calling")

	// ez-assistant-runtime bridge options.
	runtimeHome := flag.String("runtime-home", "", "ez-assistant runtime_home (reads run/runtime.json when url/token omitted)")
	ezURL := flag.String("ez-url", "", "explicit ez-assistant-runtime base URL (e.g. http://127.0.0.1:7240)")
	ezToken := flag.String("ez-token", "", "explicit ez-assistant-runtime Bearer token")
	mcpKey := flag.String("mcp-server-key", "esp-pika", "configured MCP server key in ez-assistant mcp.json")
	autoApprove := flag.Bool("auto-approve", true, "automatically set ez-assistant session approval mode to auto")
	flag.Parse()

	apiKey := strings.TrimSpace(*deepseekKey)
	if apiKey == "" {
		apiKey = strings.TrimSpace(os.Getenv("DEEPSEEK_API_KEY"))
	}

	selected := strings.ToLower(strings.TrimSpace(*provider))
	if selected == "" || selected == "auto" {
		if apiKey != "" {
			selected = "deepseek"
		} else {
			selected = "ez-assistant"
		}
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	lis, err := net.Listen("tcp", *listenAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "listen %s: %v\n", *listenAddr, err)
		os.Exit(1)
	}

	grpcSrv := grpc.NewServer()
	switch selected {
	case "deepseek":
		srv, err := deepseek.NewServer(deepseek.Config{
			APIKey:        apiKey,
			BaseURL:       *deepseekURL,
			Model:         *deepseekModel,
			GatewayMCPURL: *gatewayMCPURL,
			AgentID:       *agentID,
		})
		if err != nil {
			fmt.Fprintf(os.Stderr, "init deepseek server: %v\n", err)
			os.Exit(1)
		}
		agentv1.RegisterAgentSessionServiceServer(grpcSrv, srv)
		fmt.Printf("ez-bridge listening gRPC on %s (provider=deepseek model=%s mcp=%s)\n",
			*listenAddr, *deepseekModel, *gatewayMCPURL)
	case "ez-assistant":
		client, err := ezruntime.NewClient(ezruntime.Config{
			BaseURL:     *ezURL,
			AccessToken: *ezToken,
			RuntimeHome: *runtimeHome,
		})
		if err != nil {
			fmt.Fprintf(os.Stderr, "init ezruntime client: %v\n", err)
			os.Exit(1)
		}
		client.StartEventStream(ctx)
		defer client.StopEventStream()

		srv := bridge.NewServer(client, bridge.Options{
			AgentID:      *agentID,
			MCPServerKey: *mcpKey,
			AutoApprove:  *autoApprove,
		})
		agentv1.RegisterAgentSessionServiceServer(grpcSrv, srv)
		fmt.Printf("ez-bridge listening gRPC on %s (provider=ez-assistant mcp_server_key=%s)\n",
			*listenAddr, *mcpKey)
	default:
		fmt.Fprintf(os.Stderr, "unsupported provider %q (expected auto, deepseek, or ez-assistant)\n", *provider)
		os.Exit(1)
	}

	go func() {
		<-ctx.Done()
		grpcSrv.GracefulStop()
	}()

	if err := grpcSrv.Serve(lis); err != nil {
		fmt.Fprintf(os.Stderr, "grpc serve: %v\n", err)
		os.Exit(1)
	}
}

package server

import (
	"encoding/json"
	stdhttp "net/http"

	gatewayv1 "esp-pika/backend/gateway/api/gateway/v1"
	"esp-pika/backend/gateway/internal/conf"
	"esp-pika/backend/gateway/internal/service"

	"github.com/go-kratos/kratos/v2/log"
	"github.com/go-kratos/kratos/v2/middleware/recovery"
	khttp "github.com/go-kratos/kratos/v2/transport/http"
)

// NewHTTPServer constructs the Kratos HTTP server and mounts REST, WebSocket, Media, and MCP routes.
func NewHTTPServer(
	c *conf.Server,
	admin *service.GatewayAdminService,
	ws *DeviceWSHandler,
	voiceDbg *VoiceDebugHandler,
	ezDevice *EZDeviceServer,
	mcp *MCPServer,
	logger log.Logger,
) *khttp.Server {
	var opts = []khttp.ServerOption{
		khttp.Middleware(
			recovery.Recovery(),
		),
	}
	if c != nil && c.Http != nil {
		if c.Http.Network != "" {
			opts = append(opts, khttp.Network(c.Http.Network))
		}
		if c.Http.Addr != "" {
			opts = append(opts, khttp.Address(c.Http.Addr))
		}
		if c.Http.Timeout != nil {
			opts = append(opts, khttp.Timeout(c.Http.Timeout.AsDuration()))
		}
	}

	srv := khttp.NewServer(opts...)
	gatewayv1.RegisterGatewayAdminServiceHTTPServer(srv, admin)

	// Native handlers bypass Protobuf HTTP codec per AGENTS.md.
	srv.HandleFunc("/ws/device", ws.ServeWS)
	srv.HandleFunc("/api/v1/media/upload", ws.HandleMediaUpload)
	if voiceDbg != nil {
		srv.HandleFunc("/api/v1/voice/enroll", voiceDbg.HandleEnroll)
		srv.HandleFunc("/api/v1/voice/identify", voiceDbg.HandleIdentify)
		srv.HandleFunc("/api/v1/voice/transcribe", voiceDbg.HandleTranscribe)
		srv.HandleFunc("/api/v1/settings/prompt", voiceDbg.HandleSystemPrompt)
		srv.HandleFunc("/api/v1/settings/deepseek", voiceDbg.HandleDeepSeekSettings)
	}
	if ezDevice != nil {
		srv.HandleFunc("/api/v1/ezdevice/status", ezDevice.HandleStatus)
		srv.HandleFunc("/api/v1/ezdevice/discover", ezDevice.HandleDiscover)
		srv.HandleFunc("/api/v1/ezdevice/mode", ezDevice.HandleMode)
		srv.HandleFunc("/api/v1/ezdevice/pair", ezDevice.HandlePair)
		srv.HandleFunc("/api/v1/ezdevice/reset", ezDevice.HandleReset)
		srv.HandleFunc("/api/v1/ezdevice/test", ezDevice.HandleTestTurn)
	}
	srv.HandlePrefix("/media/", stdhttp.StripPrefix("/media/", stdhttp.FileServer(stdhttp.Dir(ws.MediaDir()))))
	srv.Handle("/mcp", mcp)
	srv.HandlePrefix("/ui/", NewWebUIHandler())
	srv.HandleFunc("/", func(w stdhttp.ResponseWriter, r *stdhttp.Request) {
		if r.URL.Path != "/" {
			stdhttp.NotFound(w, r)
			return
		}
		stdhttp.Redirect(w, r, "/ui/", stdhttp.StatusFound)
	})
	srv.HandleFunc("/healthz", func(w stdhttp.ResponseWriter, _ *stdhttp.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
	})
	return srv
}

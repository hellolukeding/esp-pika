package main

import (
	"flag"
	"os"

	"esp-pika/backend/gateway/internal/conf"
	"esp-pika/backend/gateway/internal/server"

	"github.com/go-kratos/kratos/v2"
	"github.com/go-kratos/kratos/v2/config"
	"github.com/go-kratos/kratos/v2/config/file"
	"github.com/go-kratos/kratos/v2/log"
	"github.com/go-kratos/kratos/v2/transport/grpc"
	"github.com/go-kratos/kratos/v2/transport/http"
)

var flagconf string

func init() {
	flag.StringVar(&flagconf, "conf", "../../configs/config.yaml", "config path, eg: -conf config.yaml")
}

func newApp(logger log.Logger, gs *grpc.Server, hs *http.Server, ms *server.MDNSServer, ezs *server.EZDeviceServer) *kratos.App {
	return kratos.New(
		kratos.Name("esp-pika-gateway"),
		kratos.Version("v0.1.0"),
		kratos.Logger(logger),
		kratos.Server(gs, hs, ms, ezs),
	)
}

func main() {
	flag.Parse()

	logger := log.With(log.NewStdLogger(os.Stdout),
		"ts", log.DefaultTimestamp,
		"caller", log.DefaultCaller,
		"service.name", "esp-pika-gateway",
	)
	helper := log.NewHelper(logger)

	c := config.New(
		config.WithSource(file.NewSource(flagconf)),
	)
	defer c.Close()

	if err := c.Load(); err != nil {
		helper.Fatalf("load config %s: %v", flagconf, err)
	}

	var bc conf.Bootstrap
	if err := c.Scan(&bc); err != nil {
		helper.Fatalf("scan bootstrap config: %v", err)
	}

	app, cleanup, err := wireApp(&bc, logger)
	if err != nil {
		helper.Fatalf("wire app: %v", err)
	}
	defer cleanup()

	if err := app.Run(); err != nil {
		helper.Fatalf("run app: %v", err)
	}
}

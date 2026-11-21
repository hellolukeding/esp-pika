package server

import (
	gatewayv1 "esp-pika/backend/gateway/api/gateway/v1"
	"esp-pika/backend/gateway/internal/conf"
	"esp-pika/backend/gateway/internal/service"

	"github.com/go-kratos/kratos/v2/log"
	"github.com/go-kratos/kratos/v2/middleware/recovery"
	"github.com/go-kratos/kratos/v2/transport/grpc"
)

// NewGRPCServer constructs the Kratos gRPC server.
func NewGRPCServer(c *conf.Server, admin *service.GatewayAdminService, logger log.Logger) *grpc.Server {
	var opts = []grpc.ServerOption{
		grpc.Middleware(
			recovery.Recovery(),
		),
	}
	if c != nil && c.Grpc != nil {
		if c.Grpc.Network != "" {
			opts = append(opts, grpc.Network(c.Grpc.Network))
		}
		if c.Grpc.Addr != "" {
			opts = append(opts, grpc.Address(c.Grpc.Addr))
		}
		if c.Grpc.Timeout != nil {
			opts = append(opts, grpc.Timeout(c.Grpc.Timeout.AsDuration()))
		}
	}
	srv := grpc.NewServer(opts...)
	gatewayv1.RegisterGatewayAdminServiceServer(srv, admin)
	return srv
}

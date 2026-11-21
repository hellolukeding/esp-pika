//go:build wireinject

package main

import (
	"esp-pika/backend/gateway/internal/biz"
	"esp-pika/backend/gateway/internal/conf"
	"esp-pika/backend/gateway/internal/data"
	"esp-pika/backend/gateway/internal/server"
	"esp-pika/backend/gateway/internal/service"

	"github.com/go-kratos/kratos/v2"
	"github.com/go-kratos/kratos/v2/log"
	"github.com/google/wire"
)

func wireApp(bc *conf.Bootstrap, logger log.Logger) (*kratos.App, func(), error) {
	panic(wire.Build(
		wire.FieldsOf(new(*conf.Bootstrap), "Server", "Data", "Discovery", "Arbiter", "Voice"),
		data.ProviderSet,
		biz.ProviderSet,
		service.ProviderSet,
		server.ProviderSet,
		newApp,
	))
}

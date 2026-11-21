package service

import "github.com/google/wire"

// ProviderSet provides service layer implementations.
var ProviderSet = wire.NewSet(NewGatewayAdminService)

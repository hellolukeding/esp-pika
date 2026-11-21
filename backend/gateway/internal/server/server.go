package server

import "github.com/google/wire"

// ProviderSet provides transport servers and handlers.
var ProviderSet = wire.NewSet(
	NewGRPCServer,
	NewHTTPServer,
	NewDeviceWSHandler,
	NewVoiceDebugHandler,
	NewMCPServer,
	NewMDNSServer,
)

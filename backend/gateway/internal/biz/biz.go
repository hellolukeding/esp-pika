package biz

import "github.com/google/wire"

// ProviderSet provides all domain usecases.
var ProviderSet = wire.NewSet(
	NewMacAuthUsecase,
	NewACLUsecase,
	NewDeviceUsecase,
	NewSpeakerUsecase,
	NewSettingsUsecase,
	NewSessionRouter,
	NewSpeechPipeline,
)

#pragma once

#include "board.h"
#include "gateway_protocol.h"
#include "net_client.h"
#include "voiceprint_store.h"

namespace esp_pika {

/// Executes inbound gateway MCP/device control commands against local Board HALs.
class LocalExecutor {
public:
    LocalExecutor(Board* board, MediaUploader* uploader, VoiceprintStore* vp_store = nullptr);

    /// Dispatches a parsed GatewayCommand and returns a complete CommandResultPayload.
    CommandResultPayload Execute(const GatewayCommand& cmd);

private:
    CommandResultPayload ExecuteCameraCapture(const GatewayCommand& cmd);
    CommandResultPayload ExecuteSetVolume(const GatewayCommand& cmd);
    CommandResultPayload ExecuteActuatorSet(const GatewayCommand& cmd);
    CommandResultPayload ExecuteDisplayShow(const GatewayCommand& cmd);
    CommandResultPayload ExecuteSensorRead(const GatewayCommand& cmd);
    CommandResultPayload ExecuteVoiceprintList(const GatewayCommand& cmd);
    CommandResultPayload ExecuteVoiceprintDelete(const GatewayCommand& cmd);

    Board* board_;
    MediaUploader* uploader_;
    VoiceprintStore* vp_store_;
};

} // namespace esp_pika

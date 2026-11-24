#pragma once

#include <string>

namespace esp_pika {

/// Top-level device lifecycle & conversation state machine.
enum class DeviceState {
    kIdle = 0,
    kDiscovering,
    kConnecting,
    kConnectedIdle,
    kWakePending,
    kListening,
    kSpeaking,
    kOffline,
    kProvisioning,
};

const char* DeviceStateName(DeviceState state);

/// Deterministic state machine separating protocol/lifecycle transitions from FreeRTOS/HAL side effects.
class AppStateMachine {
public:
    DeviceState State() const { return state_; }
    bool IsUplinkActive() const { return state_ == DeviceState::kListening; }

    DeviceState OnBoot();
    DeviceState OnWifiConnected();
    DeviceState OnWifiLost();
    DeviceState OnPortalStarted();
    DeviceState OnPortalStopped(bool wifi_connected);
    DeviceState OnGatewayDiscovered();
    DeviceState OnWsConnected();
    DeviceState OnDeviceWelcome();
    DeviceState OnWsDisconnected();
    DeviceState OnLocalWakeTriggered();
    DeviceState OnWakeAccept();
    DeviceState OnWakeSuppress();
    DeviceState OnSpeechStopped();
    DeviceState OnDownlinkAudioStart();
    DeviceState OnPlaybackFinished();

private:
    DeviceState state_ = DeviceState::kIdle;
};

} // namespace esp_pika

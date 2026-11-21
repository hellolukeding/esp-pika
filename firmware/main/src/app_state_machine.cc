#include "app_state_machine.h"

namespace esp_pika {

const char* DeviceStateName(DeviceState state) {
    switch (state) {
    case DeviceState::kIdle: return "Idle";
    case DeviceState::kDiscovering: return "Discovering";
    case DeviceState::kConnecting: return "Connecting";
    case DeviceState::kConnectedIdle: return "ConnectedIdle";
    case DeviceState::kWakePending: return "WakePending";
    case DeviceState::kListening: return "Listening";
    case DeviceState::kSpeaking: return "Speaking";
    case DeviceState::kOffline: return "Offline";
    }
    return "Unknown";
}

DeviceState AppStateMachine::OnBoot() {
    state_ = DeviceState::kIdle;
    return state_;
}

DeviceState AppStateMachine::OnWifiConnected() {
    state_ = DeviceState::kDiscovering;
    return state_;
}

DeviceState AppStateMachine::OnWifiLost() {
    state_ = DeviceState::kOffline;
    return state_;
}

DeviceState AppStateMachine::OnGatewayDiscovered() {
    if (state_ == DeviceState::kDiscovering || state_ == DeviceState::kOffline ||
        state_ == DeviceState::kIdle) {
        state_ = DeviceState::kConnecting;
    }
    return state_;
}

DeviceState AppStateMachine::OnWsConnected() {
    if (state_ == DeviceState::kConnecting || state_ == DeviceState::kDiscovering) {
        state_ = DeviceState::kConnecting;
    }
    return state_;
}

DeviceState AppStateMachine::OnDeviceWelcome() {
    state_ = DeviceState::kConnectedIdle;
    return state_;
}

DeviceState AppStateMachine::OnWsDisconnected() {
    if (state_ != DeviceState::kOffline) {
        state_ = DeviceState::kConnecting;
    }
    return state_;
}

DeviceState AppStateMachine::OnLocalWakeTriggered() {
    if (state_ == DeviceState::kConnectedIdle || state_ == DeviceState::kSpeaking) {
        state_ = DeviceState::kWakePending;
    }
    return state_;
}

DeviceState AppStateMachine::OnWakeAccept() {
    if (state_ == DeviceState::kWakePending || state_ == DeviceState::kConnectedIdle) {
        state_ = DeviceState::kListening;
    }
    return state_;
}

DeviceState AppStateMachine::OnWakeSuppress() {
    if (state_ == DeviceState::kWakePending || state_ == DeviceState::kListening) {
        state_ = DeviceState::kConnectedIdle;
    }
    return state_;
}

DeviceState AppStateMachine::OnDownlinkAudioStart() {
    if (state_ == DeviceState::kListening || state_ == DeviceState::kConnectedIdle) {
        state_ = DeviceState::kSpeaking;
    }
    return state_;
}

DeviceState AppStateMachine::OnPlaybackFinished() {
    if (state_ == DeviceState::kSpeaking) {
        state_ = DeviceState::kConnectedIdle;
    }
    return state_;
}

} // namespace esp_pika

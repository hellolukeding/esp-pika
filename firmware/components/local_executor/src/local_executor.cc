#include "local_executor.h"

#include <cstdio>

namespace esp_pika {
namespace {

CommandResultPayload MakeError(const std::string& cmd_id,
                               const std::string& code,
                               const std::string& msg) {
    CommandResultPayload res;
    res.command_id = cmd_id;
    res.ok = false;
    res.error_code = code;
    res.error_message = msg;
    return res;
}

} // namespace

LocalExecutor::LocalExecutor(Board* board, MediaUploader* uploader, VoiceprintStore* vp_store)
    : board_(board), uploader_(uploader), vp_store_(vp_store) {}

CommandResultPayload LocalExecutor::Execute(const GatewayCommand& cmd) {
    if (board_ == nullptr) {
        return MakeError(cmd.command_id, "BOARD_UNAVAILABLE", "board HAL is null");
    }

    if (cmd.capability == "device.camera_capture" || cmd.capability == "camera.capture") {
        return ExecuteCameraCapture(cmd);
    }
    if (cmd.capability == "device.audio_set_volume" || cmd.capability == "audio.set_volume" ||
        cmd.capability == "audio.output") {
        return ExecuteSetVolume(cmd);
    }
    if (cmd.capability == "device.actuator_set" || cmd.capability == "actuator.led" ||
        cmd.capability == "actuator.set") {
        return ExecuteActuatorSet(cmd);
    }
    if (cmd.capability == "device.display_show" || cmd.capability == "display.lcd" ||
        cmd.capability == "display.status") {
        return ExecuteDisplayShow(cmd);
    }
    if (cmd.capability == "device.sensor_read" || cmd.capability == "sensor.env") {
        return ExecuteSensorRead(cmd);
    }
    if (cmd.capability == "device.voiceprint_list" || cmd.capability == "voiceprint.list") {
        return ExecuteVoiceprintList(cmd);
    }
    if (cmd.capability == "device.voiceprint_delete" || cmd.capability == "voiceprint.delete") {
        return ExecuteVoiceprintDelete(cmd);
    }
    return MakeError(cmd.command_id, "UNSUPPORTED_CAPABILITY",
                     "unsupported capability: " + cmd.capability);
}

CommandResultPayload LocalExecutor::ExecuteCameraCapture(const GatewayCommand& cmd) {
    CameraHal* cam = board_->Camera();
    if (cam == nullptr) {
        return MakeError(cmd.command_id, "CAMERA_UNSUPPORTED", "board has no camera");
    }
    if (uploader_ == nullptr) {
        return MakeError(cmd.command_id, "UPLOADER_UNAVAILABLE", "media uploader unavailable");
    }

    std::string resolution = "qvga";
    ExtractJsonStringField(cmd.params_json, "resolution", &resolution);

    JpegFrame frame;
    if (!cam->CaptureJpeg(resolution, &frame) || frame.data.empty()) {
        return MakeError(cmd.command_id, "CAPTURE_FAILED", "camera JPEG capture failed");
    }

    std::string media_url;
    if (!uploader_->UploadJpeg(frame.data.data(), frame.data.size(), &media_url)) {
        return MakeError(cmd.command_id, "MEDIA_UPLOAD_FAILED", "HTTP media upload failed");
    }

    char buf[160] = {};
    std::snprintf(buf, sizeof(buf), "{\"width\":%u,\"height\":%u,\"bytes\":%zu}",
                  frame.width, frame.height, frame.data.size());

    CommandResultPayload res;
    res.command_id = cmd.command_id;
    res.ok = true;
    res.media_url = media_url;
    res.result_json = std::string(buf);
    return res;
}

CommandResultPayload LocalExecutor::ExecuteSetVolume(const GatewayCommand& cmd) {
    AudioCodecHal* audio = board_->Audio();
    if (audio == nullptr) {
        return MakeError(cmd.command_id, "AUDIO_UNAVAILABLE", "audio codec HAL unavailable");
    }
    int64_t level = 0;
    if (!ExtractJsonIntField(cmd.params_json, "level", &level) || level < 0 || level > 100) {
        return MakeError(cmd.command_id, "INVALID_ARGUMENT", "volume level must be in [0, 100]");
    }
    if (!audio->SetVolume(static_cast<int>(level))) {
        return MakeError(cmd.command_id, "VOLUME_FAILED", "failed to apply speaker volume");
    }
    char buf[48] = {};
    std::snprintf(buf, sizeof(buf), "{\"level\":%d}", audio->Volume());
    CommandResultPayload res;
    res.command_id = cmd.command_id;
    res.ok = true;
    res.result_json = std::string(buf);
    return res;
}

CommandResultPayload LocalExecutor::ExecuteActuatorSet(const GatewayCommand& cmd) {
    GpioHal* gpio = board_->Gpio();
    if (gpio == nullptr) {
        return MakeError(cmd.command_id, "GPIO_UNAVAILABLE", "gpio HAL unavailable");
    }
    std::string actuator = "led";
    std::string action = cmd.action;
    ExtractJsonStringField(cmd.params_json, "actuator", &actuator);
    if (action.empty()) {
        ExtractJsonStringField(cmd.params_json, "action", &action);
    }

    std::string state_json;
    if (!gpio->SetActuator(actuator, action, cmd.params_json, &state_json)) {
        return MakeError(cmd.command_id, "ACTUATOR_FAILED", "unsupported actuator or action");
    }
    CommandResultPayload res;
    res.command_id = cmd.command_id;
    res.ok = true;
    res.result_json = state_json;
    return res;
}

CommandResultPayload LocalExecutor::ExecuteDisplayShow(const GatewayCommand& cmd) {
    DisplayHal* disp = board_->Display();
    if (disp == nullptr) {
        return MakeError(cmd.command_id, "DISPLAY_UNAVAILABLE", "display HAL unavailable");
    }
    std::string emotion = "neutral";
    std::string text;
    ExtractJsonStringField(cmd.params_json, "emotion", &emotion);
    ExtractJsonStringField(cmd.params_json, "text", &text);

    if (!disp->Show(emotion, text)) {
        return MakeError(cmd.command_id, "DISPLAY_FAILED", "failed to render display update");
    }
    CommandResultPayload res;
    res.command_id = cmd.command_id;
    res.ok = true;
    res.result_json = "{\"ok\":true}";
    return res;
}

CommandResultPayload LocalExecutor::ExecuteSensorRead(const GatewayCommand& cmd) {
    SensorHal* sensors = board_->Sensors();
    if (sensors == nullptr) {
        return MakeError(cmd.command_id, "SENSOR_UNAVAILABLE", "sensor HAL unavailable");
    }
    std::string sensor_type;
    if (!ExtractJsonStringField(cmd.params_json, "sensor_type", &sensor_type)) {
        sensor_type = cmd.action;
    }
    SensorReading reading = sensors->Read(sensor_type);
    if (!reading.ok) {
        return MakeError(cmd.command_id, "SENSOR_NOT_FOUND", "unknown sensor: " + sensor_type);
    }
    char buf[160] = {};
    std::snprintf(buf, sizeof(buf),
                  "{\"sensor_type\":\"%s\",\"value\":%.2f,\"unit\":\"%s\",\"timestamp_ms\":%lld}",
                  EscapeJsonString(reading.sensor_type).c_str(),
                  reading.value,
                  EscapeJsonString(reading.unit).c_str(),
                  static_cast<long long>(reading.timestamp_ms));
    CommandResultPayload res;
    res.command_id = cmd.command_id;
    res.ok = true;
    res.result_json = std::string(buf);
    return res;
}

CommandResultPayload LocalExecutor::ExecuteVoiceprintList(const GatewayCommand& cmd) {
    if (vp_store_ == nullptr) {
        return MakeError(cmd.command_id, "VOICEPRINT_UNAVAILABLE", "voiceprint store unavailable");
    }
    const auto list = vp_store_->Snapshot();
    std::string json = "{\"vp_version\":" + std::to_string(vp_store_->Version()) + ",\"users\":[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i > 0) {
            json.push_back(',');
        }
        json += "{\"speaker_id\":\"" + EscapeJsonString(list[i].speaker_id) + "\"";
        json += ",\"display_name\":\"" + EscapeJsonString(list[i].display_name) + "\"";
        json += ",\"role\":\"" + std::string(SpeakerRoleName(list[i].role)) + "\"";
        json += ",\"version\":" + std::to_string(list[i].version) + "}";
    }
    json += "]}";

    CommandResultPayload res;
    res.command_id = cmd.command_id;
    res.ok = true;
    res.result_json = std::move(json);
    return res;
}

CommandResultPayload LocalExecutor::ExecuteVoiceprintDelete(const GatewayCommand& cmd) {
    if (vp_store_ == nullptr) {
        return MakeError(cmd.command_id, "VOICEPRINT_UNAVAILABLE", "voiceprint store unavailable");
    }
    std::string speaker_id;
    if (!ExtractJsonStringField(cmd.params_json, "speaker_id", &speaker_id) || speaker_id.empty()) {
        return MakeError(cmd.command_id, "INVALID_ARGUMENT", "speaker_id is required");
    }
    if (!vp_store_->Delete(speaker_id)) {
        return MakeError(cmd.command_id, "SPEAKER_NOT_FOUND", "unknown speaker_id: " + speaker_id);
    }
    vp_store_->SaveToNvs();
    CommandResultPayload res;
    res.command_id = cmd.command_id;
    res.ok = true;
    res.result_json = "{\"deleted\":true}";
    return res;
}

} // namespace esp_pika

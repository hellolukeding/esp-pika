#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "afe_pipeline.h"
#include "app_state_machine.h"
#include "audio_engine.h"
#include "board.h"
#include "fbank_extractor.h"
#include "gateway_protocol.h"
#include "local_executor.h"
#include "mac_identity.h"
#include "mosaico_ui.h"
#include "net_client.h"
#include "ring_frame_queue.h"
#include "ui_service.h"
#include "voiceprint_engine.h"
#include "voiceprint_store.h"

using namespace esp_pika;

namespace {

class MockMediaUploader final : public MediaUploader {
public:
    bool should_succeed = true;
    size_t last_len = 0;

    bool UploadJpeg(const uint8_t* data, size_t len, std::string* out_media_url) override {
        if (!should_succeed || data == nullptr || len == 0 || out_media_url == nullptr) {
            return false;
        }
        last_len = len;
        *out_media_url = "/media/30-ed-a0-f3-dc-70/test.jpg";
        return true;
    }
};

void TestMacIdentity() {
    const uint8_t raw[6] = {0x30, 0xED, 0xA0, 0xF3, 0xDC, 0x70};
    assert(FormatMacAddress(raw) == "30:ed:a0:f3:dc:70");

    std::string norm;
    assert(NormalizeMacAddress("30-ED-A0-F3-DC-70", &norm));
    assert(norm == "30:ed:a0:f3:dc:70");
    assert(!NormalizeMacAddress("invalid-mac", &norm));
}

void TestMdnsTxtAndGatewayProtocol() {
    GatewayEndpoint ep;
    ep.host = "192.168.1.20";
    ep.port = 8080;
    ApplyMdnsTxtRecords({
        {"group_id", "lab-alpha"},
        {"tls", "true"},
        {"ws_path", "/ws/device"},
        {"media_path", "/api/v1/media/upload"},
    }, &ep);
    assert(ep.group_id == "lab-alpha");
    assert(ep.tls == true);
    assert(ep.ws_path == "/ws/device");
    assert(ep.media_path == "/api/v1/media/upload");

    auto korvo = CreateKorvo1Board();
    assert(korvo->Init());
    DeviceHelloPayload hello;
    hello.device_id = "30:ed:a0:f3:dc:70";
    hello.mac = "30:ed:a0:f3:dc:70";
    hello.group_id = "lab-alpha";
    hello.zone = "entrance";
    hello.chip = korvo->ChipName();
    hello.vp_version = 7;
    hello.firmware_version = "0.1.0-m3";
    hello.capabilities = korvo->Capabilities();

    std::string hello_json = BuildDeviceHelloJson(hello);
    assert(hello_json.find("\"type\":\"device.hello\"") != std::string::npos);
    assert(hello_json.find("\"upload_channel\":\"http_post\"") != std::string::npos);
    assert(hello_json.find("\"vp_version\":7") != std::string::npos);

    ParsedGatewayMessage msg;
    const std::string cmd_json =
        "{\"type\":\"device.command\",\"command_id\":\"cmd_42\","
        "\"capability\":\"device.camera_capture\",\"action\":\"capture\","
        "\"params_json\":\"{\\\"resolution\\\":\\\"vga\\\"}\",\"timeout_ms\":3000}";
    assert(ParseGatewayMessage(cmd_json, &msg));
    assert(msg.type == GatewayMsgType::kDeviceCommand);
    assert(msg.command.command_id == "cmd_42");
    assert(msg.command.capability == "device.camera_capture");
    assert(msg.command.timeout_ms == 3000);

    std::string resolution;
    assert(ExtractJsonStringField(msg.command.params_json, "resolution", &resolution));
    assert(resolution == "vga");

    std::string media_url;
    assert(ParseMediaUploadResponse("{\"ok\":true,\"media_url\":\"/media/30-ed/pic.jpg\"}", &media_url));
    assert(media_url == "/media/30-ed/pic.jpg");
}

void TestBoundedFrameQueueBackpressure() {
    BoundedFrameQueue q(3);
    assert(q.PushDropOldest({1}));
    assert(q.PushDropOldest({2}));
    assert(q.PushDropOldest({3}));
    assert(!q.PushDropOldest({4})); // drops {1}
    assert(!q.PushDropOldest({5})); // drops {2}
    assert(q.Size() == 3);
    assert(q.DroppedCount() == 2);

    std::vector<uint8_t> out;
    assert(q.Pop(&out) && out[0] == 3);
    assert(q.Pop(&out) && out[0] == 4);
    assert(q.Pop(&out) && out[0] == 5);
    assert(!q.Pop(&out));
}

void TestLocalExecutor() {
    auto core = CreateCoreBoard1();
    assert(core->Init());
    auto korvo = CreateKorvo1Board();
    assert(korvo->Init());
    MockMediaUploader uploader;

    // CoreBoard has no camera -> must report CAMERA_UNSUPPORTED cleanly.
    LocalExecutor core_exec(core.get(), &uploader);
    GatewayCommand cam_cmd{"c1", "device.camera_capture", "capture", "{\"resolution\":\"vga\"}", 5000};
    auto res_no_cam = core_exec.Execute(cam_cmd);
    assert(!res_no_cam.ok && res_no_cam.error_code == "CAMERA_UNSUPPORTED");

    // Korvo-1 captures JPEGStub and uploads via MediaUploader.
    LocalExecutor korvo_exec(korvo.get(), &uploader);
    auto res_cam = korvo_exec.Execute(cam_cmd);
    assert(res_cam.ok);
    assert(res_cam.media_url == "/media/30-ed-a0-f3-dc-70/test.jpg");
    assert(uploader.last_len > 4);

    // Volume control clamping / validation.
    auto res_vol = korvo_exec.Execute({"c2", "device.audio_set_volume", "set_volume", "{\"level\":65}", 2000});
    assert(res_vol.ok && korvo->Audio()->Volume() == 65);
    auto res_vol_bad = korvo_exec.Execute({"c3", "device.audio_set_volume", "set_volume", "{\"level\":150}", 2000});
    assert(!res_vol_bad.ok && res_vol_bad.error_code == "INVALID_ARGUMENT");

    // Sensor read & actuator toggle.
    auto res_sensor = korvo_exec.Execute({"c4", "device.sensor_read", "temp_c", "{\"sensor_type\":\"temp_c\"}", 2000});
    assert(res_sensor.ok && res_sensor.result_json.find("\"temp_c\"") != std::string::npos);

    auto res_led = korvo_exec.Execute({"c5", "device.actuator_set", "on", "{\"actuator\":\"fill_light\"}", 2000});
    assert(res_led.ok && res_led.result_json.find("\"on\"") != std::string::npos);

    auto res_disp = korvo_exec.Execute({"c6", "device.display_show", "show", "{\"emotion\":\"happy\",\"text\":\"Hi\"}", 2000});
    assert(res_disp.ok);
}

void TestStateMachineAndAudioEngine() {
    AppStateMachine sm;
    assert(sm.OnBoot() == DeviceState::kIdle);
    assert(sm.OnWifiConnected() == DeviceState::kDiscovering);
    assert(sm.OnGatewayDiscovered() == DeviceState::kConnecting);
    assert(sm.OnDeviceWelcome() == DeviceState::kConnectedIdle);
    assert(!sm.IsUplinkActive());

    assert(sm.OnLocalWakeTriggered() == DeviceState::kWakePending);
    assert(sm.OnWakeAccept() == DeviceState::kListening);
    assert(sm.IsUplinkActive());

    assert(sm.OnDownlinkAudioStart() == DeviceState::kSpeaking);
    assert(!sm.IsUplinkActive());
    assert(sm.OnPlaybackFinished() == DeviceState::kConnectedIdle);

    assert(sm.OnLocalWakeTriggered() == DeviceState::kWakePending);
    assert(sm.OnWakeSuppress() == DeviceState::kConnectedIdle);
    assert(sm.OnWifiLost() == DeviceState::kOffline);

    auto korvo = CreateKorvo1Board();
    assert(korvo->Init());
    AudioEngine engine;
    AudioEngineConfig cfg;
    assert(engine.Init(korvo->Audio(), cfg));
    assert(engine.EnqueueDownlinkOpus(reinterpret_cast<const uint8_t*>("opus"), 4));
    assert(engine.DownlinkQueueSize() == 1);
    engine.ClearQueues();
    assert(engine.DownlinkQueueSize() == 0);
}

std::vector<int16_t> MakeHarmonicUtterance(float f0_hz, float amp, size_t samples, float phase_offset) {
    constexpr float kTwoPi = 6.283185307179586f;
    std::vector<int16_t> out(samples);
    for (size_t i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / 16000.0f;
        const float env = 0.7f + 0.3f * std::sin(kTwoPi * 3.0f * t + phase_offset);
        const float sig = 0.50f * std::sin(kTwoPi * f0_hz * t + phase_offset) +
                          0.35f * std::sin(kTwoPi * (f0_hz * 4.0f) * t) +
                          0.15f * std::sin(kTwoPi * (f0_hz * 9.0f) * t);
        out[i] = static_cast<int16_t>(amp * env * sig);
    }
    return out;
}

void TestAfeAndPreRoll() {
    auto korvo = CreateKorvo1Board();
    assert(korvo->Init());

    AudioEngine engine;
    AudioEngineConfig cfg;
    assert(engine.Init(korvo->Audio(), cfg));

    bool got_wake = false;
    WakeEvent captured_wake;
    int vad_changes = 0;
    bool last_vad_speaking = false;
    assert(engine.Start(
        nullptr,
        [&](const WakeEvent& evt) {
            got_wake = true;
            captured_wake = evt;
        },
        [&](bool speaking, float snr_db) {
            (void)snr_db;
            ++vad_changes;
            last_vad_speaking = speaking;
        }));

    // 1. Feed 500ms low-energy background (five 100ms chunks = 1600 samples each).
    auto noise = MakeHarmonicUtterance(90.0f, 60.0f, 1600, 0.0f);
    AfeStepResult step;
    for (int i = 0; i < 5; ++i) {
        assert(engine.ProcessCapturedPcm(noise.data(), noise.size(), &step));
        assert(!step.vad_speaking);
    }

    // 2. Feed speech frame (600ms speech) -> triggers vad_started with 400ms pre-roll snapshot.
    auto speech = MakeHarmonicUtterance(160.0f, 7000.0f, 1600, 0.1f);
    assert(engine.ProcessCapturedPcm(speech.data(), speech.size(), &step));
    assert(step.vad_started);
    assert(step.vad_speaking);
    assert(step.snr_db > 15.0f);
    assert(step.pre_roll_pcm.size() == 6400); // 400ms @ 16kHz
    assert(vad_changes == 1 && last_vad_speaking);

    // 3. Trigger wake and confirm 1.5s pre-roll PCM captured cleanly.
    assert(engine.TriggerWakeForTest(0.94f, step.snr_db));
    assert(got_wake);
    assert(captured_wake.wake_score >= 0.90f);
    assert(captured_wake.wake_pcm_16k.size() >= 9600);

    // 4. Feed >= 400ms quiet hangover -> vad_ended fires.
    for (int i = 0; i < 5; ++i) {
        assert(engine.ProcessCapturedPcm(noise.data(), noise.size(), &step));
    }
    assert(!step.vad_speaking);
    assert(!last_vad_speaking);
    engine.Stop();
}

void TestFbankAndVoiceprintEngine() {
    FbankExtractor extractor;
    assert(extractor.Init());
    auto utt_a1 = MakeHarmonicUtterance(140.0f, 6500.0f, 19200, 0.0f); // 1.2s @ 16kHz
    std::vector<float> fbank;
    size_t frames = 0;
    assert(extractor.Extract(utt_a1.data(), utt_a1.size(), &fbank, &frames));
    assert(frames == 118); // 1 + (19200 - 400) / 160
    assert(fbank.size() == frames * 40);
    for (float v : fbank) {
        assert(!std::isnan(v) && !std::isinf(v));
    }

    VoiceprintStore store;
    VoiceprintEngine engine;
    assert(engine.Init(&store));

    auto utt_a2 = MakeHarmonicUtterance(142.0f, 6200.0f, 19200, 0.3f);
    auto utt_a3 = MakeHarmonicUtterance(138.0f, 6800.0f, 19200, 0.6f);
    assert(engine.EnrollFromSamples("spk_luke", "Luke", SpeakerRole::kOwner, {utt_a1, utt_a2, utt_a3}, 1));

    auto utt_b1 = MakeHarmonicUtterance(310.0f, 6500.0f, 19200, 0.1f);
    auto utt_b2 = MakeHarmonicUtterance(315.0f, 6100.0f, 19200, 0.4f);
    assert(engine.EnrollFromSamples("spk_guest_ref", "Alice", SpeakerRole::kMember, {utt_b1, utt_b2}, 2));

    // Query with held-out utterance of Speaker A -> high-confidence local accept.
    auto query_a = MakeHarmonicUtterance(141.0f, 6000.0f, 19200, 0.9f);
    VoiceprintMatch match_a;
    assert(engine.Identify(query_a.data(), query_a.size(), &match_a));
    assert(match_a.identified);
    assert(match_a.speaker_id == "spk_luke");
    assert(match_a.role == SpeakerRole::kOwner);
    assert(match_a.tier == ConfidenceTier::kAcceptLocal);
    assert(match_a.score >= 0.85f);

    // Async Core 1 queue worker verification.
    bool got_async = false;
    assert(engine.StartAsyncWorker([&](const WakeVoiceprintOutcome& out) {
        got_async = true;
        assert(out.match.speaker_id == "spk_luke");
        assert(out.inference_us >= 0);
    }));
    assert(engine.SubmitWakeUtteranceAsync({"hi_esp", 0.95f, 20.0f, query_a}));
    assert(engine.ProcessOnePendingForTest());
    assert(got_async);
    engine.StopAsyncWorker();
}

void TestVoiceprintSerializationSyncAndExecutor() {
    VoiceprintStore store;
    const std::string sync_json =
        "{\"type\":\"voiceprint.sync\",\"group_id\":\"home-default\",\"vp_version\":5,"
        "\"full_sync\":true,\"upserts\":[{\"speaker_id\":\"spk_1\",\"display_name\":\"Luke\","
        "\"role\":1,\"embedding\":[0.6,0.8,0.0],\"version\":5}]}";
    VoiceprintSyncPayload sync;
    assert(ParseVoiceprintSyncJson(sync_json, &sync));
    assert(sync.vp_version == 5);
    assert(sync.full_sync == true);
    assert(sync.upserts.size() == 1);
    assert(sync.upserts[0].role == SpeakerRole::kOwner);
    assert(store.ApplySync(sync));
    assert(store.Count() == 1);
    assert(store.Version() == 5);

    std::vector<uint8_t> blob;
    assert(store.Serialize(&blob));
    VoiceprintStore restored;
    assert(restored.Deserialize(blob.data(), blob.size()));
    assert(restored.Count() == 1 && restored.Version() == 5);

    // Flipping a byte in serialized NVS blob must fail checksum verification.
    blob[blob.size() / 2] ^= 0x5A;
    assert(!restored.Deserialize(blob.data(), blob.size()));

    // Verify LocalExecutor voiceprint MCP handlers + incremental deletion sync.
    auto korvo = CreateKorvo1Board();
    assert(korvo->Init());
    LocalExecutor exec(korvo.get(), nullptr, &store);
    auto list_res = exec.Execute({"vp1", "device.voiceprint_list", "list", "{}", 2000});
    assert(list_res.ok && list_res.result_json.find("spk_1") != std::string::npos);

    auto del_res = exec.Execute({"vp2", "device.voiceprint_delete", "delete", "{\"speaker_id\":\"spk_1\"}", 2000});
    assert(del_res.ok && store.Count() == 0);
}

void TestMosaicoUiAnd3DAvatar() {
    assert(ParseAvatarEmotion("happy") == AvatarEmotion::kHappy);
    assert(ParseAvatarEmotion("THINKING") == AvatarEmotion::kThinking);
    assert(ParseAvatarEmotion("curious") == AvatarEmotion::kCurious);
    assert(ParseAvatarEmotion("surprised") == AvatarEmotion::kSurprised);
    assert(ParseAvatarEmotion("sleep") == AvatarEmotion::kSleep);
    assert(ParseAvatarEmotion("unknown_x") == AvatarEmotion::kIdle);

    MosaicoUi ui;
    int cb_volume = -1;
    uint8_t cb_brightness = 0;
    UiCallbacks cbs;
    cbs.on_volume_change = [&](int v) { cb_volume = v; };
    cbs.on_brightness_change = [&](uint8_t b) { cb_brightness = b; };
    assert(ui.Init(cbs));

    UiSnapshot snap;
    snap.wifi_connected = true;
    snap.gateway_connected = true;
    snap.battery_valid = true;
    snap.battery_percent = 92;

    // Verify all 12 expressions render cleanly; corners stay pure OLED black;
    // open eyes in Curious/Surprised/Happy contain bright rainbow chroma (zero black pupil hole).
    auto sample_luma_and_chroma = [&](int px, int py, int* out_luma, int* out_chroma) {
        const uint16_t c = ui.Framebuffer()[py * MosaicoUi::kWidth + px];
        const int r = ((c >> 11) & 0x1F) * 255 / 31;
        const int g = ((c >> 5) & 0x3F) * 255 / 63;
        const int b = (c & 0x1F) * 255 / 31;
        *out_luma = (r + g + b) / 3;
        *out_chroma = std::max({r, g, b}) - std::min({r, g, b});
    };

    const AvatarEmotion open_eyes[] = {AvatarEmotion::kIdle, AvatarEmotion::kCurious,
                                       AvatarEmotion::kSurprised, AvatarEmotion::kHappy};
    for (AvatarEmotion e : open_eyes) {
        ui.RenderPreviewExpression(e, snap);
        assert(ui.Framebuffer()[0] == 0x0000);
        assert(ui.Framebuffer()[479 * 480 + 479] == 0x0000);
        // Scan central eye row region (roughly y in [205..245], x in [175..220]) for luminous glass.
        int max_luma = 0;
        for (int y = 205; y <= 245; y += 2) {
            for (int x = 175; x <= 225; x += 2) {
                int luma = 0;
                int chroma = 0;
                sample_luma_and_chroma(x, y, &luma, &chroma);
                max_luma = std::max(max_luma, luma);
            }
        }
        assert(max_luma > 95);
    }

    // Closed eye state should be substantially dimmer in the eye interior than open idle.
    ui.RenderPreviewExpression(AvatarEmotion::kSleep, snap);
    int sleep_max_luma = 0;
    for (int y = 225; y <= 250; ++y) {
        for (int x = 185; x <= 215; ++x) {
            int luma = 0;
            int chroma = 0;
            sample_luma_and_chroma(x, y, &luma, &chroma);
            sleep_max_luma = std::max(sleep_max_luma, luma);
        }
    }
    assert(sleep_max_luma < 85);

    // Verify swipe-down opens drawer, slider drag invokes callbacks, swipe-up closes drawer.
    std::vector<UiRect> dirty;
    ui.OnTouch({true, 240, 20, 1}, 100, &snap);
    ui.OnTouch({true, 240, 95, 1}, 130, &snap);
    ui.OnTouch({false, 240, 95, 0}, 150, &snap);
    assert(ui.IsDrawerOpen());
    for (int i = 0; i < 15; ++i) {
        ui.StepAndRender(0.05f, snap, &dirty);
    }
    assert(!dirty.empty());

    // Drag volume & brightness sliders.
    ui.OnTouch({true, 266, 244, 1}, 1000, &snap);
    ui.OnTouch({false, 266, 244, 0}, 1020, &snap);
    assert(cb_volume >= 45 && cb_volume <= 55);

    ui.OnTouch({true, 340, 298, 1}, 1100, &snap);
    ui.OnTouch({false, 340, 298, 0}, 1120, &snap);
    assert(cb_brightness >= 65);

    // Swipe up closes drawer.
    ui.OnTouch({true, 240, 200, 1}, 1200, &snap);
    ui.OnTouch({true, 240, 110, 1}, 1230, &snap);
    ui.OnTouch({false, 240, 110, 0}, 1250, &snap);
    assert(!ui.IsDrawerOpen());

    // Verify Mosaico board Expose Display/Touch dimensions cleanly on host.
    auto mosaico = CreateMosaicoBoard();
    assert(mosaico->Init());
    assert(mosaico->Display()->Width() == 480);
    assert(mosaico->Display()->Height() == 480);
    assert(mosaico->Touch() != nullptr);
    UiService svc;
    AudioEngine ae;
    assert(ae.Init(mosaico->Audio(), AudioEngineConfig{}));
    assert(svc.Start(mosaico.get(), &ae));
    svc.Stop();
}

} // namespace

int main() {
    TestMacIdentity();
    TestMdnsTxtAndGatewayProtocol();
    TestBoundedFrameQueueBackpressure();
    TestLocalExecutor();
    TestStateMachineAndAudioEngine();
    TestAfeAndPreRoll();
    TestFbankAndVoiceprintEngine();
    TestVoiceprintSerializationSyncAndExecutor();
    TestMosaicoUiAnd3DAvatar();
    std::printf("ALL FIRMWARE HOST TESTS PASSED\n");
    return 0;
}

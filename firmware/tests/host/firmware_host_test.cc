#include <arpa/inet.h>
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
#include "chime_pcm.h"
#include "ed25519_lite.h"
#include "ez_device_client.h"
#include "ez_protocol.h"
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
#include "wifi_portal.h"
#include "wifi_store.h"

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

    std::string speech_start = BuildSpeechStartJson("spk_luke", "ez-assistant", 1, 0.9125f);
    assert(speech_start.find("\"speaker_id\":\"spk_luke\"") != std::string::npos);
    assert(speech_start.find("\"agent_id\":\"ez-assistant\"") != std::string::npos);
    assert(speech_start.find("\"speaker_role\":1") != std::string::npos);
    assert(speech_start.find("\"confidence\":0.9125") != std::string::npos);

    std::string speech_stop = BuildSpeechStopJson();
    assert(speech_stop.find("\"type\":\"speech.stop\"") != std::string::npos);
    std::string speech_stop_turn = BuildSpeechStopJson("turn_7");
    assert(speech_stop_turn.find("\"turn_id\":\"turn_7\"") != std::string::npos);
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
    assert(sm.OnSpeechStopped() == DeviceState::kConnectedIdle);
    assert(!sm.IsUplinkActive());
    assert(sm.OnDownlinkAudioStart() == DeviceState::kSpeaking);
    assert(!sm.IsUplinkActive());
    assert(sm.OnPlaybackFinished() == DeviceState::kConnectedIdle);

    // Also verify double-wake during kListening safely transitions to kWakePending.
    assert(sm.OnLocalWakeTriggered() == DeviceState::kWakePending);
    assert(sm.OnWakeAccept() == DeviceState::kListening);
    assert(sm.OnLocalWakeTriggered() == DeviceState::kWakePending);

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

    size_t chime_count = 0;
    const int16_t* chime_pcm = GetSystemChimePcm16k(&chime_count);
    assert(chime_pcm != nullptr);
    assert(chime_count == 21952);
    int16_t chime_peak = 0;
    for (size_t i = 0; i < chime_count; ++i) {
        const int16_t a = (chime_pcm[i] < 0) ? static_cast<int16_t>(-chime_pcm[i]) : chime_pcm[i];
        if (a > chime_peak) {
            chime_peak = a;
        }
    }
    assert(chime_peak >= 14000 && chime_peak <= 15000);
    for (size_t i = chime_count - 64; i < chime_count; ++i) {
        assert(chime_pcm[i] == 0);
    }
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
        // Scan central eye region (covers diagonal almond eye domes at 3/4 hero angle) for luminous glass.
        int max_luma = 0;
        for (int y = 210; y <= 280; y += 2) {
            for (int x = 210; x <= 310; x += 2) {
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
    for (int y = 238; y <= 268; ++y) {
        for (int x = 236; x <= 276; ++x) {
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

void TestWifiStoreAndSelection() {
    WifiStore store;
    assert(store.Empty());
    assert(!store.Upsert("", "pw"));
    assert(!store.SeedFromDefaults("", "pw"));

    assert(store.SeedFromDefaults("SeedNet", "seedpass"));
    assert(store.Count() == 1);
    assert(!store.SeedFromDefaults("OtherNet", "otherpass"));
    assert(store.Count() == 1);

    // Fill past kMaxCredentials (8) and confirm lowest last_used_seq ("SeedNet") gets evicted.
    for (int i = 1; i <= 8; ++i) {
        assert(store.Upsert("Net" + std::to_string(i), "pass" + std::to_string(i)));
    }
    assert(store.Count() == WifiStore::kMaxCredentials);
    WifiCredential cred;
    assert(!store.Find("SeedNet", &cred));
    assert(store.Find("Net1", &cred));
    assert(store.Find("Net8", &cred));

    // Mark Net1 connected -> moves to MRU top.
    assert(store.MarkConnected("Net1"));
    auto list = store.List();
    assert(!list.empty() && list[0].ssid == "Net1");

    // Gateway override + serialization roundtrip + checksum validation.
    store.SetGatewayOverride("192.168.10.8", 8088);
    assert(store.GatewayOverride().enabled);
    assert(store.GatewayOverride().host == "192.168.10.8");
    assert(store.GatewayOverride().port == 8088);

    std::vector<uint8_t> blob;
    assert(store.Serialize(&blob));
    WifiStore restored;
    assert(restored.Deserialize(blob.data(), blob.size()));
    assert(restored.Count() == WifiStore::kMaxCredentials);
    assert(restored.GatewayOverride().enabled);
    assert(restored.GatewayOverride().host == "192.168.10.8");
    assert(restored.GatewayOverride().port == 8088);
    assert(restored.List()[0].ssid == "Net1");

    blob[blob.size() / 2] ^= 0x3C;
    assert(!restored.Deserialize(blob.data(), blob.size()));

    assert(store.Remove("Net1"));
    assert(store.Count() == 7);
    store.ClearGatewayOverride();
    assert(!store.GatewayOverride().enabled);

    // MergeWifiLists dedup & priority ordering.
    std::vector<WifiCredential> saved_creds = {
        {"Office", "opw", 10},
        {"Home", "hpw", 8},
        {"OfflineSaved", "spw", 5},
    };
    std::vector<WifiApInfo> scanned = {
        {"GuestOpen", -68, false, false, false},
        {"Office", -78, true, false, false},
        {"Office", -54, true, false, false},
        {"Home", -45, true, false, false},
        {"Cafe", -52, true, false, false},
    };
    auto merged = MergeWifiLists(saved_creds, scanned, "Office");
    assert(merged.size() == 5);
    assert(merged[0].ssid == "Office" && merged[0].connected && merged[0].saved && merged[0].rssi == -54);
    assert(merged[1].ssid == "Home" && merged[1].saved && !merged[1].connected && merged[1].rssi == -45);
    assert(merged[2].ssid == "OfflineSaved" && merged[2].saved && merged[2].rssi == -100);
    assert(merged[3].ssid == "Cafe" && !merged[3].saved && merged[3].rssi == -52);
    assert(merged[4].ssid == "GuestOpen" && !merged[4].secure && merged[4].rssi == -68);

    // SelectAutoConnectCandidates excludes specified SSID and sorts visible saved by RSSI desc.
    auto cands = SelectAutoConnectCandidates(saved_creds, scanned, "Office");
    assert(cands.size() == 1);
    assert(cands[0].ssid == "Home");

    assert(std::string(WifiReasonToMessage(15)) == "WRONG PASSWORD");
    assert(std::string(WifiReasonToMessage(202)) == "WRONG PASSWORD");
    assert(std::string(WifiReasonToMessage(201)) == "AP NOT FOUND");
    assert(std::string(WifiReasonToMessage(203)) == "ASSOC FAILED");
}

void TestWifiPortalProtocol() {
    assert(BuildPortalApSsid("30:ed:a0:f4:5b:f0") == "ESP-Mosaico-5BF0");
    assert(BuildPortalApSsid("") == "ESP-Mosaico-SETUP");

    assert(EscapeJsonString("a\"b\\c\n") == "a\\\"b\\\\c\\n");

    // DNS wildcard reply checks.
    std::vector<uint8_t> dns_query = {
        0x12, 0x34, // ID
        0x01, 0x00, // Standard query with RD=1
        0x00, 0x01, // QDCOUNT=1
        0x00, 0x00, // ANCOUNT=0
        0x00, 0x00, // NSCOUNT=0
        0x00, 0x00, // ARCOUNT=0
        0x07, 'c', 'a', 'p', 't', 'i', 'v', 'e',
        0x05, 'a', 'p', 'p', 'l', 'e',
        0x03, 'c', 'o', 'm',
        0x00,       // End of QNAME
        0x00, 0x01, // QTYPE = A
        0x00, 0x01, // QCLASS = IN
    };
    uint8_t reply[128] = {};
    const uint32_t portal_ip_be = htonl(0xC0A80401U); // 192.168.4.1
    const size_t reply_len =
        BuildDnsWildcardReply(dns_query.data(), dns_query.size(), portal_ip_be, reply, sizeof(reply));
    assert(reply_len == dns_query.size() + 16);
    assert(reply[0] == 0x12 && reply[1] == 0x34);
    assert((reply[2] & 0x84U) == 0x84U); // QR=1, AA=1
    assert(reply[6] == 0x00 && reply[7] == 0x01); // ANCOUNT=1
    assert(reply[reply_len - 4] == 192 && reply[reply_len - 3] == 168 &&
           reply[reply_len - 2] == 4 && reply[reply_len - 1] == 1);

    // Invalid / truncated packets must be rejected.
    assert(BuildDnsWildcardReply(dns_query.data(), 8, portal_ip_be, reply, sizeof(reply)) == 0);
    dns_query[2] = 0x80; // QR=1 (not a query)
    assert(BuildDnsWildcardReply(dns_query.data(), dns_query.size(), portal_ip_be, reply, sizeof(reply)) == 0);

    std::string ssid;
    std::string pwd;
    bool use_saved = true;
    assert(ParsePortalConnectRequest(
        "{\"ssid\":\"Home\\\"Net\",\"password\":\"p\\\\w\\\"123\",\"use_saved\":false}",
        &ssid, &pwd, &use_saved));
    assert(ssid == "Home\"Net");
    assert(pwd == "p\\w\"123");
    assert(!use_saved);

    assert(ParsePortalConnectRequest("{\"ssid\":\"Home\",\"use_saved\":true}", &ssid, &pwd, &use_saved));
    assert(ssid == "Home" && pwd.empty() && use_saved);
    assert(!ParsePortalConnectRequest("{\"ssid\":\"\"}", &ssid, &pwd, &use_saved));

    assert(ParsePortalForgetRequest("{\"ssid\":\"Home\"}", &ssid) && ssid == "Home");
    assert(!ParsePortalForgetRequest("{}", &ssid));

    std::string gw_host;
    uint16_t gw_port = 0;
    assert(ParsePortalGatewayRequest("{\"host\":\"192.168.101.7\",\"port\":8088}", &gw_host, &gw_port));
    assert(gw_host == "192.168.101.7" && gw_port == 8088);
    assert(!ParsePortalGatewayRequest("{\"host\":\"192.168.101.7\",\"port\":99999}", &gw_host, &gw_port));
}

void TestMosaicoWifiUiAndKeyboard() {
    MosaicoUi ui;
    int scan_calls = 0;
    int open_calls = 0;
    bool portal_toggle_called = false;
    bool portal_toggle_val = false;
    std::string connect_ssid;
    std::string connect_pwd;
    bool connect_use_saved = false;
    std::string forget_ssid;

    UiCallbacks cbs;
    cbs.on_wifi_scan = [&]() { ++scan_calls; };
    cbs.on_wifi_panel_open = [&]() { ++open_calls; };
    cbs.on_wifi_toggle_portal = [&](bool en) {
        portal_toggle_called = true;
        portal_toggle_val = en;
    };
    cbs.on_wifi_connect = [&](const std::string& s, const std::string& p, bool saved) {
        connect_ssid = s;
        connect_pwd = p;
        connect_use_saved = saved;
    };
    cbs.on_wifi_forget = [&](const std::string& s) { forget_ssid = s; };
    assert(ui.Init(cbs));

    UiSnapshot snap;
    snap.mac = "30:ed:a0:f4:5b:f0";
    snap.ap_ssid = "ESP-Mosaico-5BF0";
    snap.wifi_entries = {
        {"HomeWiFi", -48, true, true, true},
        {"CafeFree", -65, false, false, false},
        {"Guest5G", -72, true, false, false},
    };

    auto tap = [&](int16_t x, int16_t y, int64_t t_ms) {
        ui.OnTouch({true, x, y, 1}, t_ms, &snap);
        ui.OnTouch({false, x, y, 0}, t_ms + 20, &snap);
    };

    // 1. Open via top-left HUD tap.
    tap(42, 18, 100);
    assert(ui.IsWifiMenuOpen());
    assert(open_calls == 1);
    tap(240, 420, 200); // CLOSE
    assert(!ui.IsWifiMenuOpen() && !ui.IsOverlayOpen());

    // 2. Open via drawer WIFI > button.
    std::vector<UiRect> dirty;
    ui.OnTouch({true, 240, 20, 1}, 300, &snap);
    ui.OnTouch({true, 240, 95, 1}, 330, &snap);
    ui.OnTouch({false, 240, 95, 0}, 350, &snap);
    assert(ui.IsDrawerOpen());
    for (int i = 0; i < 15; ++i) {
        ui.StepAndRender(0.05f, snap, &dirty);
    }
    tap(378, 169, 1200);
    assert(!ui.IsDrawerOpen() && ui.IsWifiMenuOpen() && open_calls == 2);

    // 3. Action buttons & row actions.
    tap(110, 146, 1300); // SCAN
    assert(scan_calls == 1);
    tap(248, 146, 1400); // HOTSPOT
    assert(portal_toggle_called && portal_toggle_val);
    tap(200, 200, 1500); // Saved row 0 ("HomeWiFi")
    assert(connect_ssid == "HomeWiFi" && connect_pwd.empty() && connect_use_saved);
    tap(395, 200, 1600); // DEL on row 0
    assert(forget_ssid == "HomeWiFi");
    tap(200, 254, 1700); // Open row 1 ("CafeFree")
    assert(connect_ssid == "CafeFree" && connect_pwd.empty() && !connect_use_saved);

    // 4. Tap secured unsaved row 2 ("Guest5G") -> opens touch keyboard; type "aB75" and JOIN.
    tap(200, 308, 1800);
    assert(ui.IsWifiKeyboardOpen());
    tap(64, 193, 1900);  // 'a'
    tap(64, 245, 2000);  // SHF
    tap(284, 245, 2100); // 'B'
    tap(79, 297, 2200);  // 123
    tap(300, 141, 2300); // '7'
    tap(140, 141, 2400); // '3'
    tap(418, 245, 2500); // DEL -> "aB7"
    tap(220, 141, 2600); // '5' -> "aB75"
    tap(368, 297, 2700); // JOIN
    assert(ui.IsWifiMenuOpen() && !ui.IsWifiKeyboardOpen());
    assert(connect_ssid == "Guest5G" && connect_pwd == "aB75" && !connect_use_saved);

    // 5. Portal bottom-banner tap on avatar screen also opens Wi-Fi menu.
    tap(240, 420, 2800); // CLOSE
    assert(!ui.IsOverlayOpen());
    snap.wifi_mode = WifiUiMode::kPortal;
    tap(240, 435, 2900);
    assert(ui.IsWifiMenuOpen());

    // 6. Preview renders preserve pure black corners on 480x480 AMOLED bezel.
    ui.RenderPreviewWifiMenu(snap);
    assert(ui.Framebuffer()[0] == 0x0000);
    assert(ui.Framebuffer()[479 * 480 + 479] == 0x0000);
    ui.RenderPreviewWifiKeyboard("Guest5G", "aB75", snap);
    assert(ui.Framebuffer()[0] == 0x0000);
    assert(ui.Framebuffer()[479 * 480 + 479] == 0x0000);

    // Verify AppStateMachine provisioning transitions.
    AppStateMachine sm;
    assert(sm.OnBoot() == DeviceState::kIdle);
    assert(sm.OnPortalStarted() == DeviceState::kProvisioning);
    assert(sm.OnPortalStopped(false) == DeviceState::kOffline);
    assert(sm.OnPortalStarted() == DeviceState::kProvisioning);
    assert(sm.OnPortalStopped(true) == DeviceState::kDiscovering);
}

std::vector<uint8_t> DecodeHexBytes(const char* hex) {
    const size_t len = std::strlen(hex);
    assert(len % 2 == 0);
    std::vector<uint8_t> out(len / 2, 0);
    auto nib = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + c - 'a');
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + c - 'A');
        assert(false);
        return 0;
    };
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<uint8_t>((nib(hex[i * 2]) << 4) | nib(hex[i * 2 + 1]));
    }
    return out;
}

std::string EncodeHexBytes(const uint8_t* data, size_t len) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0f]);
        out.push_back(kHex[data[i] & 0x0f]);
    }
    return out;
}

void TestEd25519Rfc8032VectorsAndEzCryptoFixture() {
    struct Vector {
        const char* seed_hex;
        const char* msg_hex;
        const char* expected_pub_hex;
        const char* expected_sig_hex;
    };
    const Vector vectors[] = {
        {
            "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
            "",
            "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
            "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        },
        {
            "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
            "72",
            "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
            "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
        },
        {
            "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
            "af82",
            "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
            "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
            "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a",
        },
    };

    for (const auto& v : vectors) {
        const auto seed = DecodeHexBytes(v.seed_hex);
        const auto msg = DecodeHexBytes(v.msg_hex);
        assert(seed.size() == 32);
        uint8_t pub[32] = {};
        uint8_t sig[64] = {};
        ez_ed25519_public_key(seed.data(), pub);
        assert(EncodeHexBytes(pub, sizeof(pub)) == v.expected_pub_hex);
        ez_ed25519_sign(seed.data(), pub, msg.empty() ? nullptr : msg.data(), msg.size(), sig);
        assert(EncodeHexBytes(sig, sizeof(sig)) == v.expected_sig_hex);
    }

    // Cross-verified against ez-assistant docs/resources/device-protocol-v1/fixtures/crypto-v1.json
    EzPairingIdentity fix_id;
    fix_id.has_seed = true;
    for (uint8_t i = 0; i < 32; ++i) {
        fix_id.ed25519_seed[i] = i;
    }
    ez_ed25519_public_key(fix_id.ed25519_seed, fix_id.ed25519_pub);
    assert(fix_id.PublicKeyBase64Url() == "A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg");
    const std::string fp = fix_id.PublicKeyFingerprint();
    assert(fp.size() == 9 && fp[4] == '-');

    EzCapabilities fix_caps;
    fix_caps.input_text = true;
    fix_caps.input_pcm16_16k_mono = false;
    fix_caps.output_text = true;
    fix_caps.output_pcm16_16k_mono = false;
    fix_caps.playback_cancel = false;
    fix_caps.display_status = true;
    fix_caps.display_transcript = true;

    const std::string auth_sig = EzComputeAuthSignature(
        fix_id.ed25519_seed, fix_id.ed25519_pub, "connection-fixture", "host-auth-nonce",
        "device-fixture", "device-auth-nonce", fix_caps, EzOutputPreference::kText);
    assert(auth_sig ==
           "0Yn3d21SvbS0GEj9BLydIypy4JUotUiburl5jojJTUo9JCXCJ7S9k7VKWM-ZnRTnDBxf7whvbntuUyDraAFsDg");

    const std::string cert_fp(64, 'a');
    const std::string commit_sig_1 = EzComputePairingCommitSignature(
        fix_id.ed25519_seed, fix_id.ed25519_pub, "req-1", "0123456789abcdef0123456789abcdef",
        cert_fp, "dev-n", "host-n", fix_caps, "device-1");
    const std::string commit_sig_2 = EzComputePairingCommitSignature(
        fix_id.ed25519_seed, fix_id.ed25519_pub, "req-1", "0123456789abcdef0123456789abcdef",
        cert_fp, "dev-n", "host-n", fix_caps, "device-2");
    assert(commit_sig_1.size() == 86 && commit_sig_2.size() == 86);
    assert(commit_sig_1 != commit_sig_2);
}

void TestEzProtocolAndUiPairingFlow() {
    EzPairingIdentity orig;
    orig.has_seed = true;
    for (size_t i = 0; i < 32; ++i) {
        orig.ed25519_seed[i] = static_cast<uint8_t>(i + 1);
    }
    ez_ed25519_public_key(orig.ed25519_seed, orig.ed25519_pub);
    orig.paired = true;
    orig.installation_id = "0123456789abcdef0123456789abcdef";
    orig.cert_fingerprint = std::string(64, 'b');
    orig.device_id = "device-abc";
    orig.host_address = "192.168.110.50";
    orig.host_port = 51234;
    orig.host_path = "/device";
    orig.display_name = "Mosaico-5BF0";

    std::vector<uint8_t> blob;
    assert(orig.Serialize(&blob));

    EzPairingIdentity decoded;
    assert(decoded.Deserialize(blob.data(), blob.size()));
    assert(decoded.has_seed && decoded.paired);
    assert(std::memcmp(decoded.ed25519_seed, orig.ed25519_seed, 32) == 0);
    assert(std::memcmp(decoded.ed25519_pub, orig.ed25519_pub, 32) == 0);
    assert(decoded.device_id == "device-abc");
    assert(decoded.host_address == "192.168.110.50");
    assert(decoded.host_port == 51234);

    // Corrupted CRC must fail.
    blob[blob.size() - 1] ^= 0x55;
    assert(!decoded.Deserialize(blob.data(), blob.size()));

    // ClearPairing preserves key seed/pub while wiping pairing fields.
    orig.ClearPairing();
    assert(orig.has_seed && !orig.paired);
    assert(orig.device_id.empty() && orig.host_address.empty() && orig.host_port == 0);

    // PCM16 20ms frame encode / decode
    uint8_t pcm640[640] = {};
    for (size_t i = 0; i < sizeof(pcm640); ++i) {
        pcm640[i] = static_cast<uint8_t>(i & 0xffu);
    }
    uint8_t uplink[656] = {};
    assert(EzEncodeUplinkPcmFrame(7, 42, pcm640, uplink));
    assert(uplink[0] == 1 && uplink[1] == 1);
    assert(uplink[7] == 7 && uplink[11] == 42);
    assert(uplink[12] == 0x02 && uplink[13] == 0x80);

    // Flip kind to downlink=2 and decode.
    uplink[1] = 2;
    uint32_t sid = 0;
    uint32_t seq = 0;
    const uint8_t* out_pcm = nullptr;
    size_t out_len = 0;
    assert(EzDecodeDownlinkPcmFrame(uplink, sizeof(uplink), &sid, &seq, &out_pcm, &out_len));
    assert(sid == 7 && seq == 42 && out_len == 640);
    assert(std::memcmp(out_pcm, pcm640, 640) == 0);
    assert(!EzDecodeDownlinkPcmFrame(uplink, 600, &sid, &seq, &out_pcm, &out_len));

    // Inbound JSON parsing + UTF-8 \uXXXX decoding
    EzInboundMessage msg;
    assert(EzParseInboundMessage(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m1\",\"type\":\"transcript\","
        "\"payload\":{\"client_input_id\":\"in1\",\"text\":\"\\u4f60\\u597d\"}}",
        &msg));
    assert(msg.type == EzInboundType::kTranscript);
    assert(msg.text == "\xE4\xBD\xA0\xE5\xA5\xBD");

    assert(EzParseInboundMessage(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m2\","
        "\"type\":\"pairing_direct_challenge\","
        "\"payload\":{\"pairing_request_id\":\"pr1\",\"host_nonce\":\"hn1\",\"device_id\":\"dev-9\"}}",
        &msg));
    assert(msg.type == EzInboundType::kPairingDirectChallenge);
    assert(msg.pairing_request_id == "pr1" && msg.host_nonce == "hn1" && msg.device_id == "dev-9");

    assert(EzParseInboundMessage(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m3\",\"type\":\"hello_ack\","
        "\"payload\":{\"device_id\":\"dev-9\",\"connection_id\":\"c1\","
        "\"output_preference\":\"text\","
        "\"capabilities\":{\"input_text\":true,\"input_pcm16_16k_mono\":true,"
        "\"output_text\":true,\"output_pcm16_16k_mono\":true,\"playback_cancel\":true,"
        "\"display_status\":true,\"display_transcript\":true}}}",
        &msg));
    assert(msg.type == EzInboundType::kHelloAck);
    assert(msg.capabilities.input_pcm16_16k_mono && msg.capabilities.output_pcm16_16k_mono);
    assert(msg.output_preference == EzOutputPreference::kText);

    assert(EzParseInboundMessage(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m4\",\"type\":\"ping\","
        "\"payload\":{\"nonce\":\"pn-1\",\"sent_at_ms\":1234567890}}",
        &msg));
    assert(msg.type == EzInboundType::kPing && msg.ping_nonce == "pn-1" &&
           msg.ping_sent_at_ms == 1234567890LL);

    // Connect trigger JSON parsing
    EzConnectTrigger trig;
    const std::string trig_json =
        std::string("{\"host\":\"192.168.110.9\",\"port\":50123,\"path\":\"/device\",") +
        "\"installation_id\":\"0123456789ABCDEF0123456789abcdef\"," +
        "\"certificate_fingerprint\":\"" + std::string(64, 'C') + "\"}";
    assert(EzParseConnectTriggerJson(trig_json, &trig));
    assert(trig.host == "192.168.110.9" && trig.port == 50123 && trig.path == "/device");
    assert(trig.installation_id == "0123456789abcdef0123456789abcdef");
    assert(trig.certificate_fingerprint == std::string(64, 'c'));

    // Host-side AudioEngine 20ms PCM16 codec switch & downlink queue test
    auto mosaico_board = CreateMosaicoBoard();
    assert(mosaico_board && mosaico_board->Init());
    AudioEngine eng;
    AudioEngineConfig ecfg;
    ecfg.transport_codec = AudioTransportCodec::kPcm16_20ms;
    assert(eng.Init(mosaico_board->Audio(), ecfg));
    assert(eng.TransportCodec() == AudioTransportCodec::kPcm16_20ms);
    assert(eng.EnqueueDownlinkPcm16(pcm640, sizeof(pcm640)));
    assert(eng.DownlinkQueueSize() == 1);
    eng.ClearDownlinkQueue();
    assert(eng.DownlinkQueueSize() == 0);

    // MosaicoUi direct-mode PAIR READY render & drawer UNPAIR callback test
    MosaicoUi ui;
    bool unpair_called = false;
    UiCallbacks uicbs;
    uicbs.on_ez_unpair = [&]() { unpair_called = true; };
    assert(ui.Init(uicbs));

    UiSnapshot snap;
    snap.mac = "30:ed:a0:f4:5b:f0";
    snap.wifi_connected = true;
    snap.wifi_mode = WifiUiMode::kConnected;
    snap.ez_direct_mode = true;
    snap.ez_paired = false;
    snap.ez_key_fp = "A1B2-C3D4";
    ui.RenderPreviewExpression(AvatarEmotion::kIdle, snap, "");
    assert(ui.Framebuffer()[0] == 0x0000);

    snap.ez_paired = true;
    snap.gateway_connected = true;
    std::vector<UiRect> dirty;
    ui.OnTouch({true, 240, 20, 1}, 100, &snap);
    ui.OnTouch({true, 240, 95, 1}, 130, &snap);
    ui.OnTouch({false, 240, 95, 0}, 150, &snap);
    assert(ui.IsDrawerOpen());
    for (int i = 0; i < 15; ++i) {
        ui.StepAndRender(0.05f, snap, &dirty);
    }
    ui.RenderPreviewDrawer(snap);
    ui.OnTouch({true, 270, 170, 1}, 1000, &snap);
    ui.OnTouch({false, 270, 170, 0}, 1020, &snap);
    assert(unpair_called);
    assert(!snap.ez_paired && !ui.IsDrawerOpen());

    // Phase 2: hardware hello, device_command, speech_status_changed, voiceprint_sync, and aliases
    EzHardwareProfile hw;
    hw.enabled = true;
    hw.mac = "30:ed:a0:f4:5b:f0";
    hw.chip = "esp32s31";
    hw.model = "esp32s31-mosaico";
    hw.zone = "desk";
    hw.group_id = "home-default";
    hw.vp_version = 3;
    hw.capabilities = {"device.display_show", "device.audio_set_volume", "device.sensor_read"};
    const std::string hello_hw = EzBuildAuthHelloJson(
        "m-hw-1", "dev-1", "nonce-1", EzCapabilities{}, EzOutputPreference::kText,
        "0.4.0-direct", "sig-1", &hw);
    assert(hello_hw.find("\"hardware\":{") != std::string::npos);
    assert(hello_hw.find("\"vp_version\":3") != std::string::npos);
    assert(hello_hw.find("\"device.display_show\"") != std::string::npos);

    EzInboundMessage cmd_msg;
    assert(EzParseInboundMessage(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m-cmd-1\",\"type\":\"device_command\","
        "\"payload\":{\"command_id\":\"cmd-42\",\"capability\":\"device.display_show\",\"action\":\"show\","
        "\"params_json\":\"{\\\"emotion\\\":\\\"happy\\\",\\\"text\\\":\\\"Hi\\\"}\"}}",
        &cmd_msg));
    assert(cmd_msg.type == EzInboundType::kDeviceCommand);
    assert(cmd_msg.device_command.command_id == "cmd-42");
    assert(cmd_msg.device_command.capability == "device.display_show");
    assert(cmd_msg.device_command.action == "show");
    std::string emo;
    assert(ExtractJsonStringField(cmd_msg.device_command.params_json, "emotion", &emo) &&
           emo == "happy");

    const std::string cmd_res = EzBuildDeviceCommandResultJson(
        "m-res-1", "cmd-42", true, "{\"ok\":true}", "", "", "");
    assert(cmd_res.find("\"type\":\"device_command_result\"") != std::string::npos);
    assert(cmd_res.find("\"command_id\":\"cmd-42\"") != std::string::npos);

    EzInboundMessage ss_msg;
    assert(EzParseInboundMessage(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m-ss-1\","
        "\"type\":\"speech_status_changed\","
        "\"payload\":{\"speech_services\":{\"asr\":\"ready\",\"tts\":\"unavailable\"}}}",
        &ss_msg));
    assert(ss_msg.type == EzInboundType::kSpeechStatusChanged);
    assert(ss_msg.speech_asr_ready && !ss_msg.speech_tts_ready);

    EzInboundMessage vp_msg;
    assert(EzParseInboundMessage(
        "{\"protocol_major\":1,\"protocol_minor\":0,\"message_id\":\"m-vp-1\","
        "\"type\":\"voiceprint_sync\","
        "\"payload\":{\"group_id\":\"home-default\",\"vp_version\":5,\"full_sync\":true,"
        "\"upserts\":[{\"speaker_id\":\"spk_1\",\"display_name\":\"Luke\",\"role\":1,"
        "\"version\":5,\"embedding\":[0.5,0.5,0.5,0.5]}],\"deleted_speaker_ids\":[]}}",
        &vp_msg));
    assert(vp_msg.type == EzInboundType::kVoiceprintSync);
    VoiceprintSyncPayload vp_sync;
    assert(ParseVoiceprintSyncJson(vp_msg.raw_payload, &vp_sync));
    assert(vp_sync.vp_version == 5 && vp_sync.upserts.size() == 1);
    VoiceprintStore vp_store_test;
    assert(vp_store_test.ApplySync(vp_sync));
    assert(vp_store_test.Version() == 5 && vp_store_test.Count() == 1);
    const std::string vp_ack = EzBuildVoiceprintSyncAckJson("m-ack-1", 5, 1, true);
    assert(vp_ack.find("\"type\":\"voiceprint_sync_ack\"") != std::string::npos);

    LocalExecutor alias_exec(mosaico_board.get(), nullptr, &vp_store_test);
    GatewayCommand vol_cmd;
    vol_cmd.command_id = "c-vol";
    vol_cmd.capability = "audio.volume";
    vol_cmd.action = "set_volume";
    vol_cmd.params_json = "{\"level\":66}";
    const auto vol_res = alias_exec.Execute(vol_cmd);
    assert(vol_res.ok && mosaico_board->Audio()->Volume() == 66);
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
    TestWifiStoreAndSelection();
    TestWifiPortalProtocol();
    TestMosaicoWifiUiAndKeyboard();
    TestEd25519Rfc8032VectorsAndEzCryptoFixture();
    TestEzProtocolAndUiPairingFlow();
    std::printf("ALL FIRMWARE HOST TESTS PASSED\n");
    return 0;
}

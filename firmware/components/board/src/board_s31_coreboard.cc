#include "board.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#endif

namespace esp_pika {
namespace {

#ifdef ESP_PLATFORM
constexpr const char* kTag = "board_core1";

// Reference pin map for ESP32-S31-Function-CoreBoard-1 (adjustable via board profile).
constexpr int kI2sBclkPin = 12;
constexpr int kI2sWsPin = 13;
constexpr int kI2sDoutPin = 14;
constexpr int kI2sDinPin = 15;
constexpr int kStatusLedPin = 8;
#endif

int64_t NowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

class CoreBoardAudio final : public AudioCodecHal {
public:
    ~CoreBoardAudio() override {
#ifdef ESP_PLATFORM
        if (rx_chan_ != nullptr) {
            i2s_channel_disable(rx_chan_);
            i2s_del_channel(rx_chan_);
            rx_chan_ = nullptr;
        }
        if (tx_chan_ != nullptr) {
            i2s_channel_disable(tx_chan_);
            i2s_del_channel(tx_chan_);
            tx_chan_ = nullptr;
        }
#endif
    }

    bool Init(uint32_t sample_rate_in, uint32_t sample_rate_out) override {
#ifdef ESP_PLATFORM
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true;
        if (i2s_new_channel(&chan_cfg, &tx_chan_, &rx_chan_) != ESP_OK) {
            ESP_LOGE(kTag, "i2s_new_channel failed");
            return false;
        }

        i2s_std_config_t tx_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_out),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = static_cast<gpio_num_t>(kI2sBclkPin),
                .ws = static_cast<gpio_num_t>(kI2sWsPin),
                .dout = static_cast<gpio_num_t>(kI2sDoutPin),
                .din = static_cast<gpio_num_t>(kI2sDinPin),
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };
        if (i2s_channel_init_std_mode(tx_chan_, &tx_cfg) != ESP_OK) {
            ESP_LOGE(kTag, "i2s tx init failed");
            return false;
        }

        i2s_std_config_t rx_cfg = tx_cfg;
        rx_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_in);
        if (i2s_channel_init_std_mode(rx_chan_, &rx_cfg) != ESP_OK) {
            ESP_LOGE(kTag, "i2s rx init failed");
            return false;
        }

        if (i2s_channel_enable(tx_chan_) != ESP_OK || i2s_channel_enable(rx_chan_) != ESP_OK) {
            ESP_LOGE(kTag, "i2s channel enable failed");
            return false;
        }
        return true;
#else
        (void)sample_rate_in;
        (void)sample_rate_out;
        return true;
#endif
    }

    size_t ReadPcm(int16_t* dst, size_t max_samples, uint32_t timeout_ms) override {
        if (dst == nullptr || max_samples == 0) {
            return 0;
        }
#ifdef ESP_PLATFORM
        if (rx_chan_ == nullptr) {
            return 0;
        }
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(rx_chan_, dst, max_samples * sizeof(int16_t), &bytes_read,
                                         pdMS_TO_TICKS(timeout_ms));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            return 0;
        }
        return bytes_read / sizeof(int16_t);
#else
        (void)timeout_ms;
        std::memset(dst, 0, max_samples * sizeof(int16_t));
        return max_samples;
#endif
    }

    size_t WritePcm(const int16_t* src, size_t sample_count, uint32_t timeout_ms) override {
        if (src == nullptr || sample_count == 0) {
            return 0;
        }
#ifdef ESP_PLATFORM
        if (tx_chan_ == nullptr) {
            return 0;
        }
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(tx_chan_, src, sample_count * sizeof(int16_t), &bytes_written,
                                          pdMS_TO_TICKS(timeout_ms));
        if (err != ESP_OK) {
            return 0;
        }
        return bytes_written / sizeof(int16_t);
#else
        (void)timeout_ms;
        return sample_count;
#endif
    }

    bool SetVolume(int volume) override {
        volume_ = std::clamp(volume, 0, 100);
        return true;
    }

    int Volume() const override {
        return volume_;
    }

private:
    int volume_ = 70;
#ifdef ESP_PLATFORM
    i2s_chan_handle_t tx_chan_ = nullptr;
    i2s_chan_handle_t rx_chan_ = nullptr;
#endif
};

class CoreBoardDisplay final : public DisplayHal {
public:
    bool Init() override { return true; }
    bool Show(const std::string& emotion, const std::string& text) override {
#ifdef ESP_PLATFORM
        ESP_LOGI(kTag, "display[%s]: %s", emotion.c_str(), text.c_str());
#else
        (void)emotion;
        (void)text;
#endif
        return true;
    }
};

class CoreBoardSensors final : public SensorHal {
public:
    bool Init() override { return true; }
    SensorReading Read(const std::string& sensor_type) override {
        if (sensor_type == "battery") {
            return SensorReading{true, sensor_type, 100.0, "%", NowMillis()};
        }
        return SensorReading{false, sensor_type, 0.0, "", NowMillis()};
    }
};

class CoreBoardGpio final : public GpioHal {
public:
    bool Init() override {
#ifdef ESP_PLATFORM
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = (1ULL << kStatusLedPin);
        cfg.mode = GPIO_MODE_OUTPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&cfg) != ESP_OK) {
            return false;
        }
        gpio_set_level(static_cast<gpio_num_t>(kStatusLedPin), 0);
#endif
        return true;
    }

    bool SetActuator(const std::string& actuator,
                     const std::string& action,
                     const std::string& params_json,
                     std::string* out_state_json) override {
        (void)params_json;
        if (actuator != "led" && actuator != "status_led") {
            return false;
        }
        bool on = (action == "on" || action == "enable" || action == "1");
        if (action == "toggle") {
            on = !led_on_;
        }
        led_on_ = on;
#ifdef ESP_PLATFORM
        gpio_set_level(static_cast<gpio_num_t>(kStatusLedPin), led_on_ ? 1 : 0);
#endif
        if (out_state_json != nullptr) {
            *out_state_json = led_on_ ? "{\"led\":\"on\"}" : "{\"led\":\"off\"}";
        }
        return true;
    }

private:
    bool led_on_ = false;
};

class CoreBoard1 final : public Board {
public:
    bool Init() override {
        return gpio_.Init() && display_.Init() && sensors_.Init() && audio_.Init(16000, 24000);
    }

    const char* BoardName() const override { return "ESP32-S31-Function-CoreBoard-1"; }
    const char* ChipName() const override { return "esp32s31"; }

    std::vector<CapabilityDescriptor> Capabilities() const override {
        return {
            {"audio.output", "tool", "", {}},
            {"actuator.led", "tool", "", {}},
            {"display.status", "tool", "", {}},
        };
    }

    AudioCodecHal* Audio() override { return &audio_; }
    DisplayHal* Display() override { return &display_; }
    CameraHal* Camera() override { return nullptr; }
    SensorHal* Sensors() override { return &sensors_; }
    GpioHal* Gpio() override { return &gpio_; }

private:
    CoreBoardAudio audio_;
    CoreBoardDisplay display_;
    CoreBoardSensors sensors_;
    CoreBoardGpio gpio_;
};

} // namespace

std::unique_ptr<Board> CreateCoreBoard1() {
    return std::make_unique<CoreBoard1>();
}

std::unique_ptr<Board> CreateSelectedBoard() {
#ifdef CONFIG_ESP_PIKA_BOARD_S31_MOSAICO
    return CreateMosaicoBoard();
#elif defined(CONFIG_ESP_PIKA_BOARD_S31_KORVO1)
    return CreateKorvo1Board();
#else
    return CreateCoreBoard1();
#endif
}

} // namespace esp_pika

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace esp_pika {

/// Describes a hardware capability exposed to the gateway in device.hello.
struct CapabilityDescriptor {
    std::string name;
    std::string kind;   // "tool" or "shadow"
    std::string upload; // optional, e.g. "http_post"
    std::vector<std::string> fields;
};

/// Single normalized sensor sample read from a local board sensor.
struct SensorReading {
    bool ok = false;
    std::string sensor_type;
    double value = 0.0;
    std::string unit;
    int64_t timestamp_ms = 0;
};

/// Captured JPEG image payload allocated by the board camera HAL.
struct JpegFrame {
    std::vector<uint8_t> data;
    uint16_t width = 0;
    uint16_t height = 0;
};

/// Hardware abstraction for full-duplex audio codec & volume control.
class AudioCodecHal {
public:
    virtual ~AudioCodecHal() = default;

    /// Initializes I2S RX/TX channels and external codec registers.
    virtual bool Init(uint32_t sample_rate_in, uint32_t sample_rate_out) = 0;

    /// Reads 16-bit mono PCM samples into caller-provided buffer (blocking up to timeout_ms).
    virtual size_t ReadPcm(int16_t* dst, size_t max_samples, uint32_t timeout_ms) = 0;

    /// Writes 16-bit mono PCM samples to speaker output.
    virtual size_t WritePcm(const int16_t* src, size_t sample_count, uint32_t timeout_ms) = 0;

    /// Sets speaker output volume in [0, 100].
    virtual bool SetVolume(int volume) = 0;

    /// Returns current speaker volume in [0, 100].
    virtual int Volume() const = 0;
};

/// Single normalized capacitive touch sample.
struct TouchPoint {
    bool pressed = false;
    int16_t x = 0;
    int16_t y = 0;
    uint8_t points = 0;
};

/// Hardware abstraction for status LCD / OLED / indicator text & emotion display.
class DisplayHal {
public:
    virtual ~DisplayHal() = default;
    virtual bool Init() = 0;
    virtual bool Show(const std::string& emotion, const std::string& text) = 0;

    virtual int Width() const { return 0; }
    virtual int Height() const { return 0; }

    /// Flushes a dirty rectangle from a native-endian RGB565 framebuffer with `stride_pixels` stride.
    virtual bool FlushRegion(int x,
                             int y,
                             int w,
                             int h,
                             const uint16_t* full_framebuffer,
                             int stride_pixels) {
        (void)x;
        (void)y;
        (void)w;
        (void)h;
        (void)full_framebuffer;
        (void)stride_pixels;
        return false;
    }

    virtual bool SetBrightness(uint8_t percent) {
        (void)percent;
        return true;
    }

    virtual void SetShowHook(std::function<void(const std::string&, const std::string&)> hook) {
        (void)hook;
    }
};

/// Hardware abstraction for capacitive touch controller.
class TouchHal {
public:
    virtual ~TouchHal() = default;
    virtual bool Init() = 0;
    virtual bool Read(TouchPoint* out) = 0;
};

/// Hardware abstraction for camera capture with hardware JPEG encoding on ESP32-S31.
class CameraHal {
public:
    virtual ~CameraHal() = default;
    virtual bool Init() = 0;
    virtual bool CaptureJpeg(const std::string& resolution, JpegFrame* out) = 0;
};

/// Hardware abstraction for onboard environmental / presence sensors.
class SensorHal {
public:
    virtual ~SensorHal() = default;
    virtual bool Init() = 0;
    virtual SensorReading Read(const std::string& sensor_type) = 0;
};

/// Hardware abstraction for controllable GPIOs, relays, and LEDs.
class GpioHal {
public:
    virtual ~GpioHal() = default;
    virtual bool Init() = 0;
    virtual bool SetActuator(const std::string& actuator,
                             const std::string& action,
                             const std::string& params_json,
                             std::string* out_state_json) = 0;
};

/// Top-level Board abstraction providing hardware identity and peripheral HALs.
class Board {
public:
    virtual ~Board() = default;

    virtual bool Init() = 0;
    virtual const char* BoardName() const = 0;
    virtual const char* ChipName() const = 0;
    virtual std::vector<CapabilityDescriptor> Capabilities() const = 0;

    virtual AudioCodecHal* Audio() = 0;
    virtual DisplayHal* Display() = 0;
    virtual CameraHal* Camera() = 0;
    virtual SensorHal* Sensors() = 0;
    virtual GpioHal* Gpio() = 0;
    virtual TouchHal* Touch() { return nullptr; }
};

/// Factory creating the board matching sdkconfig target selection.
std::unique_ptr<Board> CreateSelectedBoard();
std::unique_ptr<Board> CreateMosaicoBoard();
std::unique_ptr<Board> CreateCoreBoard1();
std::unique_ptr<Board> CreateKorvo1Board();

} // namespace esp_pika

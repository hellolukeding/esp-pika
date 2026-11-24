#include "board.h"
#include "chime_pcm.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#ifdef ESP_PLATFORM
#include "bmi270.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#ifdef CONFIG_TINYUSB_CDC_ENABLED
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"
#endif
#endif

namespace esp_pika {
namespace {

constexpr int kDisplayWidth = 480;
constexpr int kDisplayHeight = 480;

#ifdef ESP_PLATFORM
constexpr const char* kTag = "board_mosaico";

// Official ESP-Mosaico (ESP32-S31 V1.x) pinout.
constexpr gpio_num_t kMosaicoI2cSda = GPIO_NUM_56;
constexpr gpio_num_t kMosaicoI2cScl = GPIO_NUM_3;
// CoreBoard V1.2 schematic names I2S data nets from the ES8311 codec's perspective:
//   I2S_DOUT (GPIO40) -> ES8311 pin 7 ASDOUT (ADC out -> MCU .din)
//   I2S_DIN  (GPIO52) -> ES8311 pin 9 DSDIN  (DAC in  <- MCU .dout)
constexpr gpio_num_t kMosaicoI2sMclk = GPIO_NUM_54;
constexpr gpio_num_t kMosaicoI2sBclk = GPIO_NUM_37;
constexpr gpio_num_t kMosaicoI2sWs = GPIO_NUM_49;
constexpr gpio_num_t kMosaicoI2sMcuDout = GPIO_NUM_52;
constexpr gpio_num_t kMosaicoI2sMcuDin = GPIO_NUM_40;
constexpr gpio_num_t kMosaicoPaCtrlPin = GPIO_NUM_45;

// CO5300 480x480 QSPI AMOLED display + CST9220 capacitive touch pins (DISP_PWR_EN is active LOW).
constexpr gpio_num_t kMosaicoDispPwrEnPin = GPIO_NUM_60;
constexpr gpio_num_t kMosaicoDispClkPin = GPIO_NUM_42;
constexpr gpio_num_t kMosaicoDispCsPin = GPIO_NUM_50;
constexpr gpio_num_t kMosaicoDispD0Pin = GPIO_NUM_36;
constexpr gpio_num_t kMosaicoDispD1Pin = GPIO_NUM_51;
constexpr gpio_num_t kMosaicoDispD2Pin = GPIO_NUM_35;
constexpr gpio_num_t kMosaicoDispD3Pin = GPIO_NUM_9;
constexpr gpio_num_t kMosaicoDispRstPin = GPIO_NUM_44;
constexpr gpio_num_t kMosaicoTouchIntPin = GPIO_NUM_6;
constexpr int kDisplayStripRows = 24;

// Onboard I2C0 7-bit device addresses.
constexpr uint8_t kMosaicoEs8311Addr7Bit = 0x19;
constexpr uint8_t kMosaicoBq27220Addr7Bit = 0x55;
constexpr uint8_t kMosaicoCst9220Addr7Bit = 0x5A;

// BQ27220 standard I2C command addresses (little-endian 16-bit values).
constexpr uint8_t kBqRegTemperature = 0x06; // 0.1 K
constexpr uint8_t kBqRegVoltage = 0x08;     // mV
constexpr uint8_t kBqRegCurrent = 0x0C;     // signed mA
constexpr uint8_t kBqRegSoc = 0x2C;         // %

// Custom 480x480 CO5300 QSPI initialization table (overrides default 466x466 window).
constexpr uint8_t kCo5300CmdFeData[] = {0x00};
constexpr uint8_t kCo5300CmdC4Data[] = {0x80};
constexpr uint8_t kCo5300Cmd35Data[] = {0x00};
constexpr uint8_t kCo5300Cmd53Data[] = {0x20};
constexpr uint8_t kCo5300Cmd51Data[] = {0xFF};
constexpr uint8_t kCo5300Cmd63Data[] = {0xFF};
constexpr uint8_t kCo5300Cmd2aData[] = {0x00, 0x00, 0x01, 0xDF};
constexpr uint8_t kCo5300Cmd2bData[] = {0x00, 0x00, 0x01, 0xDF};

constexpr co5300_lcd_init_cmd_t kMosaicoCo5300InitCmds[] = {
    {0xFE, kCo5300CmdFeData, sizeof(kCo5300CmdFeData), 0},
    {0xC4, kCo5300CmdC4Data, sizeof(kCo5300CmdC4Data), 0},
    {0x35, kCo5300Cmd35Data, sizeof(kCo5300Cmd35Data), 10},
    {0x53, kCo5300Cmd53Data, sizeof(kCo5300Cmd53Data), 10},
    {0x51, kCo5300Cmd51Data, sizeof(kCo5300Cmd51Data), 10},
    {0x63, kCo5300Cmd63Data, sizeof(kCo5300Cmd63Data), 10},
    {0x2A, kCo5300Cmd2aData, sizeof(kCo5300Cmd2aData), 0},
    {0x2B, kCo5300Cmd2bData, sizeof(kCo5300Cmd2bData), 0},
    {0x11, nullptr, 0, 120},
    {0x29, nullptr, 0, 20},
};
#endif

int64_t NowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

#ifdef ESP_PLATFORM
class ScopedI2cBus {
public:
    ~ScopedI2cBus() { Reset(); }

    bool Init() {
        if (handle_ != nullptr) {
            return true;
        }
        i2c_master_bus_config_t i2c_cfg = {};
        i2c_cfg.i2c_port = I2C_NUM_0;
        i2c_cfg.sda_io_num = kMosaicoI2cSda;
        i2c_cfg.scl_io_num = kMosaicoI2cScl;
        i2c_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_cfg.glitch_ignore_cnt = 7;
        i2c_cfg.flags.enable_internal_pullup = true;
        if (i2c_new_master_bus(&i2c_cfg, &handle_) != ESP_OK) {
            ESP_LOGE(kTag, "i2c_new_master_bus(I2C0 SDA=GPIO%d SCL=GPIO%d) failed",
                     kMosaicoI2cSda, kMosaicoI2cScl);
            handle_ = nullptr;
            return false;
        }
        return true;
    }

    void Reset() {
        if (handle_ != nullptr) {
            i2c_del_master_bus(handle_);
            handle_ = nullptr;
        }
    }

    i2c_master_bus_handle_t Handle() const { return handle_; }

private:
    i2c_master_bus_handle_t handle_ = nullptr;
};
#endif

class MosaicoAudio final : public AudioCodecHal {
public:
    ~MosaicoAudio() override { Deinit(); }

#ifdef ESP_PLATFORM
    void SetI2cBus(i2c_master_bus_handle_t bus) { i2c_bus_ = bus; }
#endif

    void Deinit() {
#ifdef ESP_PLATFORM
        if (codec_dev_ != nullptr) {
            esp_codec_dev_close(codec_dev_);
            esp_codec_dev_delete(codec_dev_);
            codec_dev_ = nullptr;
        }
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
        if (i2c_bus_ == nullptr) {
            ESP_LOGE(kTag, "MosaicoAudio missing shared I2C0 handle");
            return false;
        }

        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        chan_cfg.dma_desc_num = 8;
        chan_cfg.dma_frame_num = 320; // 8 * 320 = 2560 samples (160ms hardware DMA cushion @ 16kHz)
        chan_cfg.auto_clear = true;
        if (i2s_new_channel(&chan_cfg, &tx_chan_, &rx_chan_) != ESP_OK) {
            ESP_LOGE(kTag, "i2s_new_channel failed");
            return false;
        }

        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_in),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = kMosaicoI2sMclk,
                .bclk = kMosaicoI2sBclk,
                .ws = kMosaicoI2sWs,
                .dout = kMosaicoI2sMcuDout,
                .din = kMosaicoI2sMcuDin,
                .invert_flags = {false, false, false},
            },
        };
        if (i2s_channel_init_std_mode(tx_chan_, &std_cfg) != ESP_OK ||
            i2s_channel_init_std_mode(rx_chan_, &std_cfg) != ESP_OK) {
            ESP_LOGE(kTag, "i2s_channel_init_std_mode failed");
            return false;
        }
        if (i2s_channel_enable(tx_chan_) != ESP_OK || i2s_channel_enable(rx_chan_) != ESP_OK) {
            ESP_LOGE(kTag, "i2s_channel_enable failed");
            return false;
        }

        audio_codec_i2s_cfg_t i2s_if_cfg = {};
        i2s_if_cfg.port = I2S_NUM_0;
        i2s_if_cfg.rx_handle = rx_chan_;
        i2s_if_cfg.tx_handle = tx_chan_;
        const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_if_cfg);

        audio_codec_i2c_cfg_t i2c_if_cfg = {};
        i2c_if_cfg.port = I2C_NUM_0;
        i2c_if_cfg.addr = static_cast<uint8_t>(kMosaicoEs8311Addr7Bit << 1);
        i2c_if_cfg.bus_handle = i2c_bus_;
        const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_if_cfg);
        const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();

        es8311_codec_cfg_t es_cfg = {};
        es_cfg.ctrl_if = ctrl_if;
        es_cfg.gpio_if = gpio_if;
        es_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
        es_cfg.pa_pin = kMosaicoPaCtrlPin;
        es_cfg.pa_reverted = false;
        es_cfg.use_mclk = true;
        es_cfg.digital_mic = false;
        es_cfg.no_dac_ref = true;
        es_cfg.mclk_div = 256;
        const audio_codec_if_t* codec_if = es8311_codec_new(&es_cfg);

        esp_codec_dev_cfg_t dev_cfg = {};
        dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT;
        dev_cfg.codec_if = codec_if;
        dev_cfg.data_if = data_if;
        codec_dev_ = esp_codec_dev_new(&dev_cfg);
        if (codec_dev_ == nullptr) {
            ESP_LOGE(kTag, "esp_codec_dev_new(ES8311) failed");
            return false;
        }

        esp_codec_dev_sample_info_t fs = {};
        fs.bits_per_sample = 16;
        fs.channel = 1;
        fs.sample_rate = sample_rate_in;
        if (esp_codec_dev_open(codec_dev_, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(kTag, "esp_codec_dev_open failed");
            return false;
        }
        // On ESP32-S31 (SOC_I2S_HW_VERSION_2), TX duplicates mono samples to both L/R I2S slots
        // only when slot_mode == MONO and slot_mask == I2S_STD_SLOT_BOTH (leaving RX unchanged).
        i2s_channel_disable(tx_chan_);
        i2s_std_slot_config_t tx_slot =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
        tx_slot.slot_mask = I2S_STD_SLOT_BOTH;
        if (i2s_channel_reconfig_std_slot(tx_chan_, &tx_slot) != ESP_OK) {
            ESP_LOGW(kTag, "i2s_channel_reconfig_std_slot(TX BOTH) failed");
        }
        i2s_channel_enable(tx_chan_);

        esp_codec_dev_vol_map_t vol_map[2] = {
            {.vol = 0, .db_value = -50.0f},
            {.vol = 100, .db_value = 12.0f},
        };
        esp_codec_dev_vol_curve_t vol_curve = {
            .vol_map = vol_map,
            .count = 2,
        };
        esp_codec_dev_set_vol_curve(codec_dev_, &vol_curve);
        esp_codec_dev_set_out_mute(codec_dev_, false);
        esp_codec_dev_set_out_vol(codec_dev_, volume_);
        esp_codec_dev_set_in_gain(codec_dev_, 30.0f);

        gpio_config_t pa_io = {};
        pa_io.pin_bit_mask = (1ULL << static_cast<uint32_t>(kMosaicoPaCtrlPin));
        pa_io.mode = GPIO_MODE_INPUT_OUTPUT;
        pa_io.pull_up_en = GPIO_PULLUP_DISABLE;
        pa_io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        pa_io.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&pa_io);
        gpio_set_level(kMosaicoPaCtrlPin, 1);

        // Allow ES8311 VMID (1uF) and NS4150B Bypass (1uF) pop-suppression to unmute before chime.
        vTaskDelay(pdMS_TO_TICKS(180));

        int r00 = -1, r09 = -1, r0d = -1, r12 = -1, r13 = -1, r14 = -1, r16 = -1, r17 = -1;
        int r31 = -1, r32 = -1, r44 = -1;
        esp_codec_dev_read_reg(codec_dev_, 0x00, &r00);
        esp_codec_dev_read_reg(codec_dev_, 0x09, &r09);
        esp_codec_dev_read_reg(codec_dev_, 0x0D, &r0d);
        esp_codec_dev_read_reg(codec_dev_, 0x12, &r12);
        esp_codec_dev_read_reg(codec_dev_, 0x13, &r13);
        esp_codec_dev_read_reg(codec_dev_, 0x14, &r14);
        esp_codec_dev_read_reg(codec_dev_, 0x16, &r16);
        esp_codec_dev_read_reg(codec_dev_, 0x17, &r17);
        esp_codec_dev_read_reg(codec_dev_, 0x31, &r31);
        esp_codec_dev_read_reg(codec_dev_, 0x32, &r32);
        esp_codec_dev_read_reg(codec_dev_, 0x44, &r44);
        ESP_LOGI(kTag,
                 "ES8311 regs: 00=%02x 09=%02x 0D=%02x 12=%02x 13=%02x 14=%02x 16=%02x 17=%02x 31=%02x 32=%02x 44=%02x PA=%d",
                 r00, r09, r0d, r12, r13, r14, r16, r17, r31, r32, r44, gpio_get_level(kMosaicoPaCtrlPin));

        {
            size_t wrote = 0;
            size_t chime_samples = 0;
            const int16_t* chime_pcm = GetSystemChimePcm16k(&chime_samples);
            constexpr size_t kChunkSamples = 320; // 20ms @ 16kHz
            int16_t* dma_chunk = static_cast<int16_t*>(
                heap_caps_malloc(kChunkSamples * sizeof(int16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            if (dma_chunk != nullptr && chime_pcm != nullptr) {
                for (size_t offset = 0; offset < chime_samples; offset += kChunkSamples) {
                    const size_t n = std::min(kChunkSamples, chime_samples - offset);
                    std::memcpy(dma_chunk, chime_pcm + offset, n * sizeof(int16_t));
                    wrote += WritePcm(dma_chunk, n, 100);
                }
            }
            if (dma_chunk != nullptr) {
                heap_caps_free(dma_chunk);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            ESP_LOGI(kTag, "ESP-Mosaico ES8311 initialized (in=%luHz out=%luHz PA=GPIO%d vol=%d chime=%u)",
                     static_cast<unsigned long>(sample_rate_in),
                     static_cast<unsigned long>(sample_rate_out),
                     kMosaicoPaCtrlPin,
                     volume_,
                     static_cast<unsigned>(wrote));
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
        (void)timeout_ms;
        if (codec_dev_ == nullptr) {
            return 0;
        }
        if (esp_codec_dev_read(codec_dev_, dst, static_cast<int>(max_samples * sizeof(int16_t))) !=
            ESP_CODEC_DEV_OK) {
            return 0;
        }
        return max_samples;
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
        (void)timeout_ms;
        if (codec_dev_ == nullptr) {
            return 0;
        }
        if (esp_codec_dev_write(codec_dev_, const_cast<int16_t*>(src),
                                static_cast<int>(sample_count * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
            return 0;
        }
        return sample_count;
#else
        (void)timeout_ms;
        return sample_count;
#endif
    }

    bool SetVolume(int volume) override {
        volume_ = std::clamp(volume, 0, 100);
#ifdef ESP_PLATFORM
        if (codec_dev_ != nullptr) {
            return esp_codec_dev_set_out_vol(codec_dev_, volume_) == ESP_CODEC_DEV_OK;
        }
#endif
        return true;
    }

    int Volume() const override { return volume_; }

private:
    int volume_ = 85;
#ifdef ESP_PLATFORM
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2s_chan_handle_t tx_chan_ = nullptr;
    i2s_chan_handle_t rx_chan_ = nullptr;
    esp_codec_dev_handle_t codec_dev_ = nullptr;
#endif
};

class MosaicoDisplay final : public DisplayHal {
public:
    ~MosaicoDisplay() override { Deinit(); }

    void Deinit() {
#ifdef ESP_PLATFORM
        if (panel_handle_ != nullptr) {
            esp_lcd_panel_del(panel_handle_);
            panel_handle_ = nullptr;
        }
        if (io_handle_ != nullptr) {
            esp_lcd_panel_io_del(io_handle_);
            io_handle_ = nullptr;
        }
        if (spi_bus_ready_) {
            spi_bus_free(SPI2_HOST);
            spi_bus_ready_ = false;
        }
        if (tx_done_sem_ != nullptr) {
            vSemaphoreDelete(tx_done_sem_);
            tx_done_sem_ = nullptr;
        }
        for (int i = 0; i < 2; ++i) {
            if (dma_strip_[i] != nullptr) {
                heap_caps_free(dma_strip_[i]);
                dma_strip_[i] = nullptr;
            }
        }
#endif
        ready_ = false;
    }

    int Width() const override { return kDisplayWidth; }
    int Height() const override { return kDisplayHeight; }

    bool Init() override {
#ifdef ESP_PLATFORM
        std::lock_guard<std::mutex> lock(mu_);
        if (ready_) {
            return true;
        }

        // 1. Power up AMOLED rail before reset (GPIO60 active LOW).
        gpio_config_t pwr_cfg = {};
        pwr_cfg.pin_bit_mask = (1ULL << kMosaicoDispPwrEnPin);
        pwr_cfg.mode = GPIO_MODE_OUTPUT;
        pwr_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        pwr_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        pwr_cfg.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&pwr_cfg) != ESP_OK) {
            ESP_LOGE(kTag, "CO5300 DISP_PWR_EN gpio_config failed");
            return false;
        }
        gpio_set_level(kMosaicoDispPwrEnPin, 0);
        vTaskDelay(pdMS_TO_TICKS(25));

        // 2. Allocate ping-pong DMA strip bounce buffers in internal DRAM.
        const size_t strip_bytes =
            static_cast<size_t>(kDisplayWidth) * kDisplayStripRows * sizeof(uint16_t);
        for (int i = 0; i < 2; ++i) {
            dma_strip_[i] = static_cast<uint16_t*>(
                heap_caps_malloc(strip_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            if (dma_strip_[i] == nullptr) {
                ESP_LOGE(kTag, "Failed to allocate CO5300 DMA strip[%d] (%u bytes)", i,
                         static_cast<unsigned>(strip_bytes));
                Deinit();
                return false;
            }
        }

        tx_done_sem_ = xSemaphoreCreateBinary();
        if (tx_done_sem_ == nullptr) {
            ESP_LOGE(kTag, "Failed to create CO5300 DMA semaphore");
            Deinit();
            return false;
        }

        // 3. Configure QSPI bus on SPI2_HOST.
        spi_bus_config_t bus_cfg = {};
        bus_cfg.sclk_io_num = kMosaicoDispClkPin;
        bus_cfg.data0_io_num = kMosaicoDispD0Pin;
        bus_cfg.data1_io_num = kMosaicoDispD1Pin;
        bus_cfg.data2_io_num = kMosaicoDispD2Pin;
        bus_cfg.data3_io_num = kMosaicoDispD3Pin;
        bus_cfg.data4_io_num = -1;
        bus_cfg.data5_io_num = -1;
        bus_cfg.data6_io_num = -1;
        bus_cfg.data7_io_num = -1;
        bus_cfg.max_transfer_sz = static_cast<int>(strip_bytes + 16);
        if (spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
            ESP_LOGE(kTag, "spi_bus_initialize(SPI2_HOST QSPI) failed");
            Deinit();
            return false;
        }
        spi_bus_ready_ = true;

        // 4. Create QSPI Panel IO & CO5300 480x480 panel driver.
        esp_lcd_panel_io_spi_config_t io_cfg = {};
        io_cfg.cs_gpio_num = kMosaicoDispCsPin;
        io_cfg.dc_gpio_num = GPIO_NUM_NC;
        io_cfg.spi_mode = 0;
        io_cfg.pclk_hz = 40 * 1000 * 1000;
        io_cfg.trans_queue_depth = 10;
        io_cfg.on_color_trans_done = &MosaicoDisplay::OnColorTransDone;
        io_cfg.user_ctx = this;
        io_cfg.lcd_cmd_bits = 32;
        io_cfg.lcd_param_bits = 8;
        io_cfg.flags.quad_mode = true;
        if (esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io_handle_) != ESP_OK) {
            ESP_LOGE(kTag, "esp_lcd_new_panel_io_spi(CO5300) failed");
            Deinit();
            return false;
        }

        co5300_vendor_config_t vendor_cfg = {};
        vendor_cfg.init_cmds = kMosaicoCo5300InitCmds;
        vendor_cfg.init_cmds_size =
            sizeof(kMosaicoCo5300InitCmds) / sizeof(kMosaicoCo5300InitCmds[0]);
        vendor_cfg.flags.use_qspi_interface = 1;

        esp_lcd_panel_dev_config_t panel_cfg = {};
        panel_cfg.reset_gpio_num = kMosaicoDispRstPin;
        panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_cfg.bits_per_pixel = 16;
        panel_cfg.vendor_config = &vendor_cfg;
        if (esp_lcd_new_panel_co5300(io_handle_, &panel_cfg, &panel_handle_) != ESP_OK) {
            ESP_LOGE(kTag, "esp_lcd_new_panel_co5300 failed");
            Deinit();
            return false;
        }

        if (esp_lcd_panel_reset(panel_handle_) != ESP_OK ||
            esp_lcd_panel_init(panel_handle_) != ESP_OK ||
            esp_lcd_panel_disp_on_off(panel_handle_, true) != ESP_OK) {
            ESP_LOGE(kTag, "CO5300 panel reset/init/disp_on failed");
            Deinit();
            return false;
        }
        esp_lcd_panel_co5300_set_brightness(panel_handle_, 90);
        ready_ = true;

        ClearBlackLocked();
        ESP_LOGI(kTag, "CO5300 %dx%d QSPI AMOLED initialized (PWR=GPIO%d RST=GPIO%d)",
                 kDisplayWidth, kDisplayHeight, kMosaicoDispPwrEnPin, kMosaicoDispRstPin);
        return true;
#else
        ready_ = true;
        return true;
#endif
    }

    bool FlushRegion(int x,
                     int y,
                     int w,
                     int h,
                     const uint16_t* full_framebuffer,
                     int stride_pixels) override {
#ifdef ESP_PLATFORM
        std::lock_guard<std::mutex> lock(mu_);
        if (!ready_ || dma_strip_[0] == nullptr || dma_strip_[1] == nullptr ||
            panel_handle_ == nullptr || full_framebuffer == nullptr || stride_pixels <= 0 ||
            w <= 0 || h <= 0) {
            return false;
        }
        // CO5300 requires even-aligned CASET/RASET boundaries.
        const int x0 = std::clamp(x, 0, kDisplayWidth) & ~1;
        const int y0 = std::clamp(y, 0, kDisplayHeight) & ~1;
        const int x1 = std::min(kDisplayWidth, (std::clamp(x + w, 0, kDisplayWidth) + 1) & ~1);
        const int y1 = std::min(kDisplayHeight, (std::clamp(y + h, 0, kDisplayHeight) + 1) & ~1);
        if (x1 <= x0 || y1 <= y0) {
            return true;
        }
        const int rw = x1 - x0;
        const int max_strip_pixels = kDisplayWidth * kDisplayStripRows;
        const int max_rows = std::max(1, max_strip_pixels / rw);

        // Ping-pong overlap: CPU byte-swaps next strip into dma_strip_[buf_idx] while
        // previous strip transfers asynchronously over QSPI DMA.
        int buf_idx = 0;
        bool dma_in_flight = false;
        xSemaphoreTake(tx_done_sem_, 0);

        for (int row = y0; row < y1; row += max_rows) {
            const int rows = std::min(max_rows, y1 - row);
            uint16_t* dst = dma_strip_[buf_idx];
            for (int r = 0; r < rows; ++r) {
                const uint16_t* src =
                    full_framebuffer + static_cast<size_t>(row + r) * stride_pixels + x0;
                for (int c = 0; c < rw; ++c) {
                    *dst++ = __builtin_bswap16(src[c]);
                }
            }
            if (dma_in_flight) {
                if (xSemaphoreTake(tx_done_sem_, pdMS_TO_TICKS(1000)) != pdTRUE) {
                    ESP_LOGW(kTag, "CO5300 DMA timeout at y=%d", row);
                    return false;
                }
            }
            if (esp_lcd_panel_draw_bitmap(panel_handle_, x0, row, x1, row + rows,
                                          dma_strip_[buf_idx]) != ESP_OK) {
                ESP_LOGE(kTag, "CO5300 draw_bitmap failed at y=%d", row);
                return false;
            }
            dma_in_flight = true;
            buf_idx ^= 1;
        }
        if (dma_in_flight) {
            if (xSemaphoreTake(tx_done_sem_, pdMS_TO_TICKS(1000)) != pdTRUE) {
                ESP_LOGW(kTag, "CO5300 final DMA timeout");
                return false;
            }
        }
        return true;
#else
        (void)x;
        (void)y;
        (void)w;
        (void)h;
        (void)full_framebuffer;
        (void)stride_pixels;
        return ready_;
#endif
    }

    bool SetBrightness(uint8_t percent) override {
        const uint8_t clamped = static_cast<uint8_t>(std::clamp<int>(percent, 5, 100));
#ifdef ESP_PLATFORM
        std::lock_guard<std::mutex> lock(mu_);
        if (!ready_ || panel_handle_ == nullptr) {
            return false;
        }
        return esp_lcd_panel_co5300_set_brightness(panel_handle_, clamped) == ESP_OK;
#else
        (void)clamped;
        return true;
#endif
    }

    void SetShowHook(std::function<void(const std::string&, const std::string&)> hook) override {
        std::lock_guard<std::mutex> lock(mu_);
        show_hook_ = std::move(hook);
    }

    bool Show(const std::string& emotion, const std::string& text) override {
        std::function<void(const std::string&, const std::string&)> hook;
        {
            std::lock_guard<std::mutex> lock(mu_);
            last_emotion_ = emotion;
            last_text_ = text;
            hook = show_hook_;
        }
#ifdef ESP_PLATFORM
        ESP_LOGI(kTag, "AMOLED[%s]: %s", emotion.c_str(), text.c_str());
#endif
        if (hook) {
            hook(emotion, text);
        }
        return ready_;
    }

private:
#ifdef ESP_PLATFORM
    static bool IRAM_ATTR OnColorTransDone(esp_lcd_panel_io_handle_t /*io*/,
                                           esp_lcd_panel_io_event_data_t* /*edata*/,
                                           void* user_ctx) {
        auto* self = static_cast<MosaicoDisplay*>(user_ctx);
        BaseType_t high_task_wakeup = pdFALSE;
        if (self != nullptr && self->tx_done_sem_ != nullptr) {
            xSemaphoreGiveFromISR(self->tx_done_sem_, &high_task_wakeup);
        }
        return high_task_wakeup == pdTRUE;
    }

    void ClearBlackLocked() {
        if (dma_strip_[0] == nullptr || panel_handle_ == nullptr) {
            return;
        }
        std::memset(dma_strip_[0], 0,
                    static_cast<size_t>(kDisplayWidth) * kDisplayStripRows * sizeof(uint16_t));
        for (int y0 = 0; y0 < kDisplayHeight; y0 += kDisplayStripRows) {
            const int rows = std::min(kDisplayStripRows, kDisplayHeight - y0);
            xSemaphoreTake(tx_done_sem_, 0);
            if (esp_lcd_panel_draw_bitmap(panel_handle_, 0, y0, kDisplayWidth, y0 + rows,
                                          dma_strip_[0]) == ESP_OK) {
                xSemaphoreTake(tx_done_sem_, pdMS_TO_TICKS(1000));
            }
        }
    }

    esp_lcd_panel_io_handle_t io_handle_ = nullptr;
    esp_lcd_panel_handle_t panel_handle_ = nullptr;
    uint16_t* dma_strip_[2] = {nullptr, nullptr};
    SemaphoreHandle_t tx_done_sem_ = nullptr;
    bool spi_bus_ready_ = false;
#endif

    mutable std::mutex mu_;
    bool ready_ = false;
    std::string last_emotion_;
    std::string last_text_;
    std::function<void(const std::string&, const std::string&)> show_hook_;
};

class MosaicoSensors final : public SensorHal {
public:
    ~MosaicoSensors() override { Deinit(); }

#ifdef ESP_PLATFORM
    void SetI2cBus(i2c_master_bus_handle_t bus) { i2c_bus_ = bus; }
#endif

    void Deinit() {
#ifdef ESP_PLATFORM
        if (bmi_handle_ != nullptr) {
            bmi270_stop(bmi_handle_);
            bmi270_delete(bmi_handle_);
            bmi_handle_ = nullptr;
        }
        bmi_present_ = false;
        if (bq_dev_ != nullptr) {
            i2c_master_bus_rm_device(bq_dev_);
            bq_dev_ = nullptr;
        }
        bq_present_ = false;
#endif
    }

    bool Init() override {
#ifdef ESP_PLATFORM
        std::lock_guard<std::mutex> lock(mu_);
        if (i2c_bus_ == nullptr) {
            ESP_LOGE(kTag, "MosaicoSensors missing shared I2C0 handle");
            return false;
        }

        // 1. Probe and bind BQ27220 fuel gauge (0x55).
        if (i2c_master_probe(i2c_bus_, kMosaicoBq27220Addr7Bit, 50) == ESP_OK) {
            i2c_device_config_t dev_cfg = {};
            dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
            dev_cfg.device_address = kMosaicoBq27220Addr7Bit;
            dev_cfg.scl_speed_hz = 400000;
            if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &bq_dev_) == ESP_OK) {
                bq_present_ = true;
                uint16_t mv = 0;
                uint16_t soc = 0;
                ReadBqReg16Locked(kBqRegVoltage, &mv);
                ReadBqReg16Locked(kBqRegSoc, &soc);
                ESP_LOGI(kTag, "BQ27220 initialized at 0x%02X (vbat=%umV soc=%u%%)",
                         kMosaicoBq27220Addr7Bit, mv, soc);
            }
        } else {
            ESP_LOGW(kTag, "BQ27220 not detected on I2C0 (0x%02X)", kMosaicoBq27220Addr7Bit);
        }

        // 2. Probe and initialize BMI270 6-axis IMU (0x69 or 0x68).
        uint8_t bmi_addr = 0;
        if (i2c_master_probe(i2c_bus_, BMI270_I2C_ADDRESS_H, 50) == ESP_OK) {
            bmi_addr = BMI270_I2C_ADDRESS_H;
        } else if (i2c_master_probe(i2c_bus_, BMI270_I2C_ADDRESS_L, 50) == ESP_OK) {
            bmi_addr = BMI270_I2C_ADDRESS_L;
        }

        if (bmi_addr != 0) {
            bmi270_driver_config_t drv_cfg = {};
            drv_cfg.addr = bmi_addr;
            drv_cfg.interface = BMI270_USE_I2C;
            drv_cfg.i2c_bus = i2c_bus_;
            if (bmi270_create(&drv_cfg, &bmi_handle_) == ESP_OK && bmi_handle_ != nullptr) {
                uint8_t chip_id = 0;
                bmi270_get_chip_id(bmi_handle_, &chip_id);
                const bmi270_config_t acq_cfg = {
                    .acce_odr = BMI270_ACC_ODR_100_HZ,
                    .acce_range = BMI270_ACC_RANGE_4_G,
                    .gyro_odr = BMI270_GYR_ODR_100_HZ,
                    .gyro_range = BMI270_GYR_RANGE_1000_DPS,
                };
                if (bmi270_start(bmi_handle_, &acq_cfg) == ESP_OK) {
                    bmi_present_ = true;
                    float ax = 0.0f;
                    float ay = 0.0f;
                    float az = 0.0f;
                    bmi270_get_acce_data(bmi_handle_, &ax, &ay, &az);
                    ESP_LOGI(kTag, "BMI270 initialized at 0x%02X (chip_id=0x%02X accel=[%.2f, %.2f, %.2f]g)",
                             bmi_addr, chip_id, ax, ay, az);
                } else {
                    ESP_LOGW(kTag, "BMI270 start failed at 0x%02X", bmi_addr);
                    bmi270_delete(bmi_handle_);
                    bmi_handle_ = nullptr;
                }
            } else {
                ESP_LOGW(kTag, "bmi270_create failed at 0x%02X", bmi_addr);
            }
        } else {
            ESP_LOGW(kTag, "BMI270 not detected on I2C0 (0x68/0x69)");
        }
        return true;
#else
        return true;
#endif
    }

    SensorReading Read(const std::string& sensor_type) override {
#ifdef ESP_PLATFORM
        std::lock_guard<std::mutex> lock(mu_);
        const int64_t ts = NowMillis();

        if (sensor_type == "battery" || sensor_type == "soc") {
            uint16_t raw = 0;
            if (bq_present_ && ReadBqReg16Locked(kBqRegSoc, &raw)) {
                return SensorReading{true, sensor_type, static_cast<double>(raw), "%", ts};
            }
            return SensorReading{false, sensor_type, 0.0, "", ts};
        }
        if (sensor_type == "battery_voltage" || sensor_type == "voltage") {
            uint16_t raw = 0;
            if (bq_present_ && ReadBqReg16Locked(kBqRegVoltage, &raw)) {
                return SensorReading{true, sensor_type, static_cast<double>(raw) / 1000.0, "V", ts};
            }
            return SensorReading{false, sensor_type, 0.0, "", ts};
        }
        if (sensor_type == "battery_current" || sensor_type == "current") {
            uint16_t raw = 0;
            if (bq_present_ && ReadBqReg16Locked(kBqRegCurrent, &raw)) {
                return SensorReading{true, sensor_type,
                                     static_cast<double>(static_cast<int16_t>(raw)), "mA", ts};
            }
            return SensorReading{false, sensor_type, 0.0, "", ts};
        }
        if (sensor_type == "temperature" || sensor_type == "battery_temp") {
            uint16_t raw = 0;
            if (bq_present_ && ReadBqReg16Locked(kBqRegTemperature, &raw)) {
                return SensorReading{true, sensor_type, static_cast<double>(raw) * 0.1 - 273.15, "C",
                                     ts};
            }
            return SensorReading{false, sensor_type, 0.0, "", ts};
        }
        if (sensor_type == "imu" || sensor_type == "accel_x" || sensor_type == "accel_y" ||
            sensor_type == "accel_z") {
            float ax = cached_ax_;
            float ay = cached_ay_;
            float az = cached_az_;
            bool ok = bmi_present_ && (cached_imu_ms_ >= 0 && (ts - cached_imu_ms_) <= 8);
            if (!ok && bmi_present_ && bmi270_get_acce_data(bmi_handle_, &ax, &ay, &az) == ESP_OK) {
                cached_ax_ = ax;
                cached_ay_ = ay;
                cached_az_ = az;
                cached_imu_ms_ = ts;
                ok = true;
            }
            if (ok) {
                if (sensor_type == "accel_x") {
                    return SensorReading{true, sensor_type, ax, "g", ts};
                }
                if (sensor_type == "accel_y") {
                    return SensorReading{true, sensor_type, ay, "g", ts};
                }
                if (sensor_type == "accel_z") {
                    return SensorReading{true, sensor_type, az, "g", ts};
                }
                const double mag = std::sqrt(static_cast<double>(ax) * ax +
                                             static_cast<double>(ay) * ay +
                                             static_cast<double>(az) * az);
                return SensorReading{true, sensor_type, mag, "g", ts};
            }
            return SensorReading{false, sensor_type, 0.0, "", ts};
        }
        if (sensor_type == "gyro_x" || sensor_type == "gyro_y" || sensor_type == "gyro_z") {
            float gx = 0.0f;
            float gy = 0.0f;
            float gz = 0.0f;
            if (bmi_present_ && bmi270_get_gyro_data(bmi_handle_, &gx, &gy, &gz) == ESP_OK) {
                if (sensor_type == "gyro_x") {
                    return SensorReading{true, sensor_type, gx, "dps", ts};
                }
                if (sensor_type == "gyro_y") {
                    return SensorReading{true, sensor_type, gy, "dps", ts};
                }
                return SensorReading{true, sensor_type, gz, "dps", ts};
            }
            return SensorReading{false, sensor_type, 0.0, "", ts};
        }
        return SensorReading{false, sensor_type, 0.0, "", ts};
#else
        if (sensor_type == "battery") {
            return SensorReading{true, sensor_type, 100.0, "%", NowMillis()};
        }
        if (sensor_type == "imu" || sensor_type == "accel_z") {
            return SensorReading{true, sensor_type, 1.0, "g", NowMillis()};
        }
        return SensorReading{false, sensor_type, 0.0, "", NowMillis()};
#endif
    }

private:
#ifdef ESP_PLATFORM
    bool ReadBqReg16Locked(uint8_t reg, uint16_t* out_val) const {
        if (bq_dev_ == nullptr || out_val == nullptr) {
            return false;
        }
        uint8_t buf[2] = {};
        if (i2c_master_transmit_receive(bq_dev_, &reg, 1, buf, sizeof(buf), 100) != ESP_OK) {
            return false;
        }
        *out_val = static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
        return true;
    }

    mutable std::mutex mu_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_master_dev_handle_t bq_dev_ = nullptr;
    bmi270_handle_t* bmi_handle_ = nullptr;
    bool bq_present_ = false;
    bool bmi_present_ = false;
    float cached_ax_ = 0.0f;
    float cached_ay_ = 0.0f;
    float cached_az_ = 1.0f;
    int64_t cached_imu_ms_ = -1;
#endif
};

class MosaicoGpio final : public GpioHal {
public:
    bool Init() override { return true; }
    bool SetActuator(const std::string& actuator,
                     const std::string& action,
                     const std::string& params_json,
                     std::string* out_state_json) override {
        (void)params_json;
        if (actuator != "pa" && actuator != "amp") {
            return false;
        }
        const bool on = (action == "on" || action == "enable" || action == "1");
#ifdef ESP_PLATFORM
        gpio_set_level(kMosaicoPaCtrlPin, on ? 1 : 0);
#endif
        if (out_state_json != nullptr) {
            *out_state_json = on ? "{\"pa\":\"on\"}" : "{\"pa\":\"off\"}";
        }
        return true;
    }
};

class MosaicoTouch final : public TouchHal {
public:
    ~MosaicoTouch() override { Deinit(); }

#ifdef ESP_PLATFORM
    void SetI2cBus(i2c_master_bus_handle_t bus) { i2c_bus_ = bus; }
#endif

    void Deinit() {
#ifdef ESP_PLATFORM
        if (isr_attached_) {
            gpio_isr_handler_remove(kMosaicoTouchIntPin);
            isr_attached_ = false;
        }
        if (cst_dev_ != nullptr) {
            i2c_master_bus_rm_device(cst_dev_);
            cst_dev_ = nullptr;
        }
        present_ = false;
        was_pressed_ = false;
#endif
    }

    bool Init() override {
#ifdef ESP_PLATFORM
        std::lock_guard<std::mutex> lock(mu_);
        if (i2c_bus_ == nullptr) {
            return false;
        }
        gpio_config_t int_cfg = {};
        int_cfg.pin_bit_mask = (1ULL << kMosaicoTouchIntPin);
        int_cfg.mode = GPIO_MODE_INPUT;
        int_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        int_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        int_cfg.intr_type = GPIO_INTR_NEGEDGE;
        gpio_config(&int_cfg);

        // Do NOT toggle GPIO44 here because CO5300 already reset the shared RST line.
        if (i2c_master_probe(i2c_bus_, kMosaicoCst9220Addr7Bit, 50) != ESP_OK) {
            ESP_LOGW(kTag, "CST9220 touch not detected on I2C0 (0x%02X)", kMosaicoCst9220Addr7Bit);
            return false;
        }
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = kMosaicoCst9220Addr7Bit;
        dev_cfg.scl_speed_hz = 400000;
        if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &cst_dev_) != ESP_OK) {
            ESP_LOGW(kTag, "CST9220 i2c_master_bus_add_device failed");
            return false;
        }
        const esp_err_t isr_err = gpio_install_isr_service(0);
        if (isr_err == ESP_OK || isr_err == ESP_ERR_INVALID_STATE) {
            if (gpio_isr_handler_add(kMosaicoTouchIntPin, &MosaicoTouch::OnTouchIsr, this) == ESP_OK) {
                isr_attached_ = true;
            }
        }
        present_ = true;
        ESP_LOGI(kTag, "CST9220 touch initialized at 0x%02X (INT=GPIO%d irq=%s)",
                 kMosaicoCst9220Addr7Bit, kMosaicoTouchIntPin, isr_attached_ ? "yes" : "poll");
        return true;
#else
        return true;
#endif
    }

    bool Read(TouchPoint* out) override {
        if (out == nullptr) {
            return false;
        }
        *out = {};
#ifdef ESP_PLATFORM
        std::lock_guard<std::mutex> lock(mu_);
        if (!present_ || cst_dev_ == nullptr) {
            return false;
        }
        // Zero-overhead gate: skip I2C transaction completely when untouched and no edge IRQ latched.
        if (!irq_pending_.exchange(false, std::memory_order_acquire) &&
            gpio_get_level(kMosaicoTouchIntPin) != 0 && !was_pressed_) {
            return false;
        }
        const uint8_t reg[2] = {0xD0, 0x00};
        uint8_t data[7] = {};
        if (i2c_master_transmit_receive(cst_dev_, reg, sizeof(reg), data, sizeof(data), 10) !=
            ESP_OK) {
            was_pressed_ = false;
            return false;
        }
        if (data[6] != 0xAB) {
            was_pressed_ = false;
            return false;
        }
        // Required CST9220 handshake ACK so the internal report FIFO keeps advancing.
        const uint8_t ack[3] = {0xD0, 0x00, 0xAB};
        i2c_master_transmit(cst_dev_, ack, sizeof(ack), 10);

        const uint8_t fingers = data[5] & 0x7F;
        const bool contact = (fingers > 0) && ((data[0] & 0x0F) == 0x06);
        was_pressed_ = contact;
        out->points = fingers;
        out->pressed = contact;
        if (contact) {
            const uint16_t raw_x =
                (static_cast<uint16_t>(data[1]) << 4) | ((data[3] >> 4) & 0x0FU);
            const uint16_t raw_y = (static_cast<uint16_t>(data[2]) << 4) | (data[3] & 0x0FU);
            out->x = static_cast<int16_t>(std::clamp<int>(raw_x, 0, kDisplayWidth - 1));
            out->y = static_cast<int16_t>(std::clamp<int>(raw_y, 0, kDisplayHeight - 1));
        }
        return true;
#else
        return false;
#endif
    }

private:
#ifdef ESP_PLATFORM
    static void IRAM_ATTR OnTouchIsr(void* arg) {
        auto* self = static_cast<MosaicoTouch*>(arg);
        if (self != nullptr) {
            self->irq_pending_.store(true, std::memory_order_release);
        }
    }

    mutable std::mutex mu_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_master_dev_handle_t cst_dev_ = nullptr;
    std::atomic<bool> irq_pending_{false};
    bool isr_attached_ = false;
    bool was_pressed_ = false;
    bool present_ = false;
#endif
};

void InitMosaicoUsbConsole() {
#if defined(ESP_PLATFORM) && defined(CONFIG_TINYUSB_CDC_ENABLED)
    tinyusb_config_t tusb_cfg = {};
    tusb_cfg.port = TINYUSB_PORT_DEFAULT_HS;
    tusb_cfg.phy.skip_setup = false;
    tusb_cfg.phy.self_powered = false;
    tusb_cfg.phy.vbus_monitor_io = -1;
    tusb_cfg.task.size = TINYUSB_DEFAULT_TASK_SIZE;
    tusb_cfg.task.priority = TINYUSB_DEFAULT_TASK_PRIO;
    tusb_cfg.task.xCoreID = TINYUSB_DEFAULT_TASK_AFFINITY;
    if (tinyusb_driver_install(&tusb_cfg) != ESP_OK) {
        ESP_LOGW(kTag, "tinyusb_driver_install failed");
        return;
    }

    tinyusb_config_cdcacm_t acm_cfg = {};
    acm_cfg.cdc_port = TINYUSB_CDC_ACM_0;
    if (tinyusb_cdcacm_init(&acm_cfg) != ESP_OK) {
        ESP_LOGW(kTag, "tinyusb_cdcacm_init failed");
        return;
    }

    if (tinyusb_console_init(TINYUSB_CDC_ACM_0) == ESP_OK) {
        // Allow USB host enumeration to settle before emitting early boot logs.
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(kTag, "TinyUSB CDC console initialized on ESP-Mosaico USB-OTG");
    }
#endif
}

class MosaicoBoard final : public Board {
public:
    bool Init() override {
        InitMosaicoUsbConsole();
        if (!gpio_.Init()) {
            return false;
        }

        if (!display_.Init()) {
#ifdef ESP_PLATFORM
            ESP_LOGW(kTag, "CO5300 AMOLED display init failed; continuing headless");
#endif
        }

#ifdef ESP_PLATFORM
        // Probe I2C0 after display_.Init() enables VCC_3V3 / CODEC_3V3 (GPIO60 active LOW).
        if (!i2c_bus_.Init()) {
            return false;
        }
        ProbeKnownI2cDevices();
        sensors_.SetI2cBus(i2c_bus_.Handle());
        touch_.SetI2cBus(i2c_bus_.Handle());
        audio_.SetI2cBus(i2c_bus_.Handle());
#endif
        touch_.Init();
        sensors_.Init();
        return audio_.Init(16000, 16000);
    }

    const char* BoardName() const override { return "ESP-Mosaico (ESP32-S31)"; }
    const char* ChipName() const override { return "esp32s31"; }

    std::vector<CapabilityDescriptor> Capabilities() const override {
        return {
            {"audio.output", "tool", "", {}},
            {"display.amoled", "tool", "", {"co5300", "480x480", "3d_avatar"}},
            {"sensor.imu_battery",
             "shadow",
             "",
             {"battery", "battery_voltage", "battery_current", "temperature", "imu", "accel_x",
              "accel_y", "accel_z", "gyro_x", "gyro_y", "gyro_z"}},
        };
    }

    AudioCodecHal* Audio() override { return &audio_; }
    DisplayHal* Display() override { return &display_; }
    CameraHal* Camera() override { return nullptr; }
    SensorHal* Sensors() override { return &sensors_; }
    GpioHal* Gpio() override { return &gpio_; }
    TouchHal* Touch() override { return &touch_; }

private:
#ifdef ESP_PLATFORM
    void ProbeKnownI2cDevices() const {
        struct KnownDev {
            uint8_t addr;
            const char* name;
        };
        constexpr KnownDev kDevs[] = {
            {kMosaicoEs8311Addr7Bit, "ES8311"},
            {kMosaicoBq27220Addr7Bit, "BQ27220"},
            {kMosaicoCst9220Addr7Bit, "CST9220"},
            {BMI270_I2C_ADDRESS_L, "BMI270_L"},
            {BMI270_I2C_ADDRESS_H, "BMI270_H"},
        };
        for (const auto& dev : kDevs) {
            const bool ack = (i2c_master_probe(i2c_bus_.Handle(), dev.addr, 40) == ESP_OK);
            ESP_LOGI(kTag, "I2C0 probe 0x%02X (%s): %s", dev.addr, dev.name, ack ? "ACK" : "-");
        }
    }

    // Must outlive audio_, sensors_, and touch_ (destructed last in reverse member order).
    ScopedI2cBus i2c_bus_;
#endif
    MosaicoAudio audio_;
    MosaicoDisplay display_;
    MosaicoSensors sensors_;
    MosaicoGpio gpio_;
    MosaicoTouch touch_;
};

} // namespace

std::unique_ptr<Board> CreateMosaicoBoard() {
    return std::make_unique<MosaicoBoard>();
}

} // namespace esp_pika

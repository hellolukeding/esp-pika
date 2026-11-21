# firmware — ESP32-S31 终端固件（ESP-IDF v6.x）

基于 ESP-IDF v6.x + CMake + C++20/C 构建。

## 目录规范

* `main/`：应用入口 `app_main` 与顶层状态机组装（禁止堆砌驱动与协议实现）
* `components/board/`：开发板硬件抽象（`ESP32-S31-Function-CoreBoard-1`、`ESP32-S31-Korvo-1`）
* `components/audio_engine/`：I2S + AFE（AEC/NS/VADNet/WakeNet/MultiNet）+ Pre-roll + Opus 流水线（绑定 Core 0）
* `components/voiceprint/`：端侧声纹识别（`esp-dsp` FBank/MFCC + `esp-dl` INT8 + 加密 NVS，运行在 Core 1）
* `components/net_client/`：`espressif/mdns` 局域网发现 + 出厂 eFuse MAC 认证 + WebSocket / HTTP 媒体上传
* `components/local_executor/`：本地外设执行器（硬件 JPEG 拍照、传感器读取、GPIO、LCD 显示）
* `partitions/`：自定义 Flash 分区表 CSV（含 `ota_0/1`、`nvs_keys`、`model` 只读分区）
* `configs/`：`sdkconfig.defaults` 及目标芯片默认配置

# Hwaipy ESP32 Audio Recorder

ESP32-S3 Super Mini 与 ESP32-S3 N16R8 USB 麦克风实时录音客户端。固件以 USB Host 模式驱动
UAC 1.0 麦克风，采集 48 kHz、16-bit、单声道 PCM，并以 100 ms 帧接入
`audio-stream` 的 `RSPAUDIO1` 兼容协议：

- TCP `grayfog.chat:8054` 实时上传、字节偏移确认和自动重连；
- Web `grayfog.chat:8055` 实时监听、历史录音和设备状态；
- 在同一个流连接上热更新麦克风增益 0–16 与硬件 AGC；
- 约 4 秒 PSRAM 队列吸收短暂网络抖动；断线重连时使用新的 stream ID，避免
  无本地持久 spool 的 ESP32 卡在旧确认偏移；
- 采集时间戳按样本数连续推进，并在系统时钟跳变超过 2 秒时重新锚定。

当前板子的服务端设备 ID 使用可读库存代号 `S3-0001`。PCM 传输约 770 kbps；
此版本不在 4 MB flash 上实现 Pi 客户端的持久 Opus spool，断电或长时间断网期间的
音频无法补传。复制到其他板子时必须为其分配不同的可读设备代号。

当前正式版本为 `audio_recorder_1.0.3`（构建号 `20260819.3`），内嵌 base
`0.4.2`。正式固件不向
临时调试接收端口发送遥测；后续开发版本继续使用 OTA 服务的 `data/debug/`
隔离通道，不覆盖正式发布。

```bash
~/.platformio/penv/bin/pio run -e esp32-s3-supermini
~/.platformio/penv/bin/pio run -e esp32-s3-n16r8
```

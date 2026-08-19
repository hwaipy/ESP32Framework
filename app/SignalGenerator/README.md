# SignalGenerator

`SignalGenerator` 是从仓库 `base` 固件派生的信号发生器应用，当前版本为
`signal_generator_0.1.1`，构建号为 `20260819.1`，内嵌 base `0.4.1`。

当前版本复用 base 提供的三板型支持、串口配置、Wi-Fi NVS、HTTPS 心跳、
SHA-256 校验和 OTA 升降级能力。

所有安全通用 GPIO 同步输出 10 ms 周期脉冲：高电平 1 ms，低电平 9 ms，
占空比 10%。独立的 `esp_timer` 每 1 ms 更新一次输出，因此 Wi-Fi 连接和
心跳请求不会阻塞波形。

| 板型 | 输出 GPIO | 排除原则 |
| --- | --- | --- |
| ESP32-S3 Super Mini | 1, 2, 4–8, 15–18, 21 | 排除绑带、Flash/PSRAM、USB、JTAG及不可输出引脚 |
| ESP32-C3 Super Mini | 0, 1, 3, 10 | 排除绑带、Flash、USB、JTAG和UART引脚 |
| ESP32-C6 Super Mini | 0–3, 14, 20–23 | 排除绑带、Flash、USB、JTAG和UART引脚 |

这里的“非特殊 GPIO”采用保守定义。USB 数据脚被保留，因此原生 USB
串口、首次刷写和故障恢复仍可使用。

一次编译三个板型：

```bash
cd app/SignalGenerator
~/.platformio/penv/bin/pio run
```

单独编译某个板型时使用 `-e esp32-s3-supermini`、
`-e esp32-c3-supermini` 或 `-e esp32-c6-supermini`。

编译产物位于：

```text
/Users/hwaipy/Documents/PlatformIO/ESP32Framework/app/SignalGenerator/build/<环境名>/firmware.bin
```

首次 USB 刷写前应确认实际板型。固件发布时，版本号必须保留
`signal_generator_` 应用前缀，并且任何二进制变化都需要使用新版本号。

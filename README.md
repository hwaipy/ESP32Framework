# Hwaipy ESP32 Framework

这是 ESP32-S3、ESP32-C3、ESP32-C6 共用的 OTA 工程。一套 `base/src` 源码通过三个 PlatformIO 环境分别生成适配各芯片的固件；三个 BIN 架构不同，不能交叉刷写。

## 目录

- `base/`：三种板型共用的 PlatformIO/Arduino OTA 基础程序。
- `app/`：从 `base` 派生的具体设备应用。
- `app/SignalGenerator/`：信号发生器固件，复用 base 的 OTA 能力。
- `server/`：FastAPI OTA 服务端源码、部署配置和测试。
- `base/OTA_RULES.md`：所有板型共同遵守的 OTA 协议。
- `AGENTS.md`：源码目录与外部构建目录的强制约定。

Google Drive 项目目录只保存源码。PlatformIO 编译产物、下载依赖和缓存位于：

```text
/Users/hwaipy/Documents/PlatformIO/ESP32Framework/
```

`server/` 允许保留本地 `.venv`；仓库根目录和 `server/.gitignore` 会忽略虚拟环境、Python/pytest 缓存及运行数据。构建 Docker 镜像时仍根据 `requirements.txt` 安装依赖。

## Base 固件

当前公共源码版本：`0.2.0`，构建号：`20260808.1`。

一次编译三个板型：

```bash
cd base
~/.platformio/penv/bin/pio run
```

单独编译：

```bash
~/.platformio/penv/bin/pio run -e esp32-s3-supermini
~/.platformio/penv/bin/pio run -e esp32-c3-supermini
~/.platformio/penv/bin/pio run -e esp32-c6-supermini
```

固件输出：

```text
/Users/hwaipy/Documents/PlatformIO/ESP32Framework/base/build/esp32-s3-supermini/firmware.bin
/Users/hwaipy/Documents/PlatformIO/ESP32Framework/base/build/esp32-c3-supermini/firmware.bin
/Users/hwaipy/Documents/PlatformIO/ESP32Framework/base/build/esp32-c6-supermini/firmware.bin
```

首次 USB 刷写必须指定实际板型，例如 S3：

```bash
~/.platformio/penv/bin/pio run -e esp32-s3-supermini --target upload
~/.platformio/penv/bin/pio device monitor
```

串口命令：

```text
info
heartbeat
wifi {"ssid":"YOUR_SSID","password":"YOUR_PASSWORD"}
wifi clear
```

Wi-Fi 凭据保存在 ESP32 NVS，不写入源码，也不会因普通 OTA 更新而丢失。设备初始心跳间隔为 60 秒，服务器正常下发 20 秒；请求失败后 15 秒重试。服务器可在心跳回复中下发 15–3600 秒的间隔。管理网页每 3 秒刷新一次。

S3 Super Mini 会检测预期的 2 MB PSRAM；C3、C6 没有 PSRAM 属于正常情况，不会导致自检失败。

## 已验证设备

- 型号：`esp32-s3-supermini`
- 唯一 ID：`2884856b37c8`
- 硬件：ESP32-S3、4 MB Flash、2 MB PSRAM
- 已验证：HTTPS 心跳、精确版本分配、SHA-256 校验、OTA 升级、主动降级、Wi-Fi NVS 持久化
- 线上历史版本：`0.1.0`、`0.1.1`、`0.1.2`；其中 `0.1.0` 仅作历史保留，不应再次分配

本地 `0.2.0` 目前只完成三板型编译验证，尚未发布或分配给设备。

## SignalGenerator 固件

当前应用版本：`signal_generator_0.1.0`，构建号：`20260808.1`。

```bash
cd app/SignalGenerator
~/.platformio/penv/bin/pio run
```

该应用以轻量入口复用 `base/src/main.cpp`，具备 base 的设备自检、Wi-Fi 配置、
心跳和 OTA 能力。三个板型上无启动、存储、USB、调试或串口职责的安全 GPIO
同步输出 10 ms 周期脉冲，其中高电平持续 1 ms、低电平持续 9 ms。

## 服务端

线上目录：`/home/ubuntu/codes/ESP32OTA`  
唯一应用端口：`8956`

```text
Internet HTTPS
  -> ota.hwaipy.cn 上既有 Nginx
  -> http://hwaipy.cn:8956
  -> hwaipy-ota 容器内 FastAPI
```

部署源码时不要覆盖服务器已有的 `.env` 和 `data/`：

```bash
rsync -az --exclude='.env' --exclude='data/' server/ Code:/home/ubuntu/codes/ESP32OTA/
ssh Code 'cd /home/ubuntu/codes/ESP32OTA && docker-compose up -d --build'
```

健康检查和管理入口：

- `https://ota.hwaipy.cn/health`
- `https://ota.hwaipy.cn/manage`

## 发布规则

1. 修改源码中的 `FIRMWARE_VERSION` 和 `FIRMWARE_BUILD`。
2. 为目标板型编译对应的 `firmware.bin`。
3. 从管理页上传固件，填写板型、版本、构建号和备注。
4. 在设备列表中为具体设备选择目标版本。
5. 设备下一次心跳获得带明确版本号的 OTA URL；升级和回退使用同一套流程。

同一 `{板型, 版本号}` 的发布不可覆盖。任何二进制变化都必须使用新版本号。应用固件可采用 `{应用前缀}_{语义版本}`，例如 `super_sonic_cleaner_presser_0.1.0`。

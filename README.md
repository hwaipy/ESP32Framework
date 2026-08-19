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

当前公共源码版本：`0.4.0`，构建号：`20260819.1`。

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

### 从 GitHub 一行烧写 factory 固件

GitHub Release 中的 `factory.bin` 是包含 bootloader、分区表、OTA 数据和应用程序的
完整镜像，适合新板首次烧写或整片重刷。以下命令适用于 macOS/Linux，会依次安装
烧写工具、下载指定版本、从地址 `0x0` 写入唯一连接的 ESP32 USB 设备，并通过串口
保存 Wi-Fi 凭据。执行前把 Wi-Fi 占位符替换为实际值：

```bash
VERSION=0.4.0; BOARD=esp32-s3-supermini; CHIP=esp32s3; export WIFI_SSID='YOUR_WIFI_SSID' WIFI_PASSWORD='YOUR_WIFI_PASSWORD'; curl -LsSf https://astral.sh/uv/install.sh | sh && curl -fL "https://github.com/hwaipy/ESP32Framework/releases/download/base-v${VERSION}/ESP32Framework-base-${VERSION}-${BOARD}-factory.bin" -o /tmp/esp32-factory.bin && ~/.local/bin/uvx --from esptool==5.3.0 esptool --chip "$CHIP" write-flash 0x0 /tmp/esp32-factory.bin && sleep 4 && ~/.local/bin/uv run --with pyserial python -c 'import json,os,time,serial; from serial.tools import list_ports; ports=[p.device for p in list_ports.comports() if p.vid is not None or any(x in p.device.lower() for x in ("usbmodem","usbserial","ttyacm","ttyusb"))]; assert ports,"未找到 ESP32 串口"; s=serial.Serial(ports[0],115200,timeout=0.2,write_timeout=2); time.sleep(3); s.reset_input_buffer(); command="wifi "+json.dumps({"ssid":os.environ["WIFI_SSID"],"password":os.environ["WIFI_PASSWORD"]},ensure_ascii=False)+"\n"; s.write(command.encode("utf-8")); s.flush(); time.sleep(6); out=s.read_all(); print(out.decode("utf-8","replace")); assert b"credentials saved" in out,"Wi-Fi 配置未确认保存，请重新执行串口配置"'
```

上例用于 ESP32-S3 Super Mini。其他板型只需替换命令开头的参数：

```text
ESP32-C3 Super Mini: BOARD=esp32-c3-supermini; CHIP=esp32c3
ESP32-C6 Super Mini: BOARD=esp32-c6-supermini; CHIP=esp32c6
```

三种芯片的固件不能交叉烧写。计算机同时连接多个 USB 串口设备时，应先断开无关
设备，避免自动选择错误串口。Wi-Fi 密码会以明文出现在执行命令的终端历史记录中。
如果开发板无法自动进入烧录模式，请按住 `BOOT`、短按 `RESET`，然后重新执行命令。

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
wifi add {"ssid":"ANOTHER_SSID","password":"ANOTHER_PASSWORD"}
wifi list
wifi remove 0
wifi clear
```

最多可以按录入顺序保存 8 组 Wi-Fi。`wifi` 与 `wifi add` 都会追加新 SSID；再次
录入同名 SSID 会在原位置更新密码。连接时从索引 0 开始逐个尝试，每组最多等待
20 秒。`wifi list` 只显示 SSID，不显示密码；`wifi remove` 使用列表中的索引删除。
0.2.x 及更早版本保存的单组凭据会自动兼容，并在下一次配置变更时迁移。

Wi-Fi 凭据保存在 ESP32 NVS，不写入源码，也不会因普通 OTA 更新而丢失。设备初始心跳间隔为 60 秒，服务器正常下发 20 秒；请求失败后 15 秒重试。服务器可在心跳回复中下发 15–3600 秒的间隔。管理网页每 3 秒刷新一次。
心跳会报告当前连接的 Wi-Fi SSID、本地 IP，以及编译进固件的 base 版本；服务端将其保存到
设备状态和心跳历史，并在管理页中显示。App 只需声明自己的固件版本，base 版本由
`base/src/main.cpp` 自动提供，不需要在每个 app 中重复填写。

S3 Super Mini 会检测预期的 2 MB PSRAM；C3、C6 没有 PSRAM 属于正常情况，不会导致自检失败。

## 已验证设备

- 型号：`esp32-s3-supermini`
- 唯一 ID：`2884856b37c8`
- 硬件：ESP32-S3、4 MB Flash、2 MB PSRAM
- 已验证：HTTPS 心跳、精确版本分配、SHA-256 校验、OTA 升级、主动降级、Wi-Fi NVS 持久化
- 线上历史版本：`0.1.0`、`0.1.1`、`0.1.2`；其中 `0.1.0` 仅作历史保留，不应再次分配

`0.2.0` 已完成三板型编译验证，并发布为 GitHub Release `base-v0.2.0`。
`0.3.0` 增加有序多 Wi-Fi 配置，目前尚未发布。
`0.4.0` 增加心跳本地 IP 上报和 OTA 管理页网络信息展示，已完成三板型编译验证并发布为 GitHub Release `base-v0.4.0`。

## SignalGenerator 固件

当前应用版本：`signal_generator_0.1.1`，构建号：`20260819.1`，内嵌 base
`0.4.0`。

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

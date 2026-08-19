# ESP32 OTA 规则

最后更新：2026-08-06

本文档是本项目所有 ESP32 板卡共同遵守的 OTA 协议规则。

## 1. 设备身份

- 每台设备由“板子型号”和“板子唯一 ID”共同标识。
- 板子型号使用稳定的小写 slug，例如：
  - `esp32-s3-supermini`
  - `esp32-c3-supermini`
  - `esp32-c6-supermini`
- 板子唯一 ID 使用芯片的 eFuse Factory MAC：去掉冒号并转换为小写十六进制。
- 当前 ESP32-S3 Super Mini 的唯一 ID 为 `2884856b37c8`。

## 2. URL 规则

### 2.0 服务入口

- 对外只使用 `https://ota.hwaipy.cn`。
- `Code` 服务器上的既有 Nginx 负责 HTTPS，并代理到 `http://hwaipy.cn:8956`。
- OTA API、心跳、固件下载和管理页面全部集中在端口 `8956`，不占用其他新端口。

### 2.1 OTA 固件 URL

```text
https://ota.hwaipy.cn/{板子型号}/{板子唯一ID}/bin/{版本号}
```

当前板卡示例：

```text
https://ota.hwaipy.cn/esp32-s3-supermini/2884856b37c8/bin/0.1.1
```

OTA URL 中必须包含明确的目标版本号。设备不自行拼接或猜测 OTA URL，完整 URL 由心跳接口的回复提供。

### 2.2 心跳 URL

```text
https://ota.hwaipy.cn/{板子型号}/{板子唯一ID}/hb
```

当前板卡示例：

```text
https://ota.hwaipy.cn/esp32-s3-supermini/2884856b37c8/hb
```

设备使用带查询参数的 HTTP GET 请求发送心跳。当前冻结字段如下：

| 参数 | 必填 | 含义 |
| --- | --- | --- |
| `v` | 是 | 当前语义版本号，例如 `0.1.2` |
| `base` | 否 | 编译进当前固件的 base 源码版本，例如 `0.4.0` |
| `build` | 否 | 构建编号 |
| `uptime` | 否 | 启动至今的秒数 |
| `status` | 否 | 运行状态，默认 `ok` |
| `rssi` | 否 | Wi-Fi RSSI，单位 dBm |
| `wifi_ssid` | 否 | 当前连接的 Wi-Fi SSID，UTF-8 URL 编码，最长 32 个字符 |
| `local_ip` | 否 | 板卡在当前 Wi-Fi 网络中的本地 IP 地址 |
| `heap` | 否 | 当前空闲堆字节数 |
| `reset` | 否 | 最近一次复位原因 |
| `ota` | 否 | OTA 状态，默认 `idle` |

### 2.3 管理 URL

```text
https://ota.hwaipy.cn/manage
```

管理页面需要同时适配电脑端和手机端。

## 3. 版本与固件历史

- 不设置“每个板型的默认最新版本”。
- 每台设备显式绑定一个目标版本；目标版本可以为空。
- 服务器保存每个板型的所有历史固件版本。
- 已发布的历史版本不删除。
- 已发布的 `{板子型号, 版本号}` 必须保持不可变，不得用不同的 BIN 覆盖。
- 如果固件内容发生变化，必须发布新的版本号。
- 将设备目标版本指定为较旧版本，即表示要求设备主动降级。
- 服务器通过心跳回复告诉设备目标版本和对应的完整 OTA URL。

## 4. 心跳回复

当设备当前版本与目标版本不一致时，心跳回复应包含：

- 当前版本 `current_version`
- 目标版本 `target_version`
- 动作 `action`：`update` 或 `downgrade`
- 完整 OTA URL `ota_url`
- 固件大小 `firmware_size`
- 固件 SHA-256 `firmware_sha256`
- 下次心跳间隔 `heartbeat_interval`

示例：

```json
{
  "ok": true,
  "current_version": "0.1.0",
  "target_version": "0.1.1",
  "action": "update",
  "ota_url": "https://ota.hwaipy.cn/esp32-s3-supermini/2884856b37c8/bin/0.1.1",
    "firmware_size": 895344,
    "firmware_sha256": "bebdf24de3a6272e3961f4cee80ef60f2fa506b09d47bc0a128879f4a3af2865",
  "heartbeat_interval": 20
}
```

当设备无需切换版本时：

- `action` 为 `none`
- `ota_url` 为 `null`
- `target_version` 可以等于当前版本，也可以为 `null`

设备判断是否需要切换固件的核心条件是：

```text
target_version != null 且 current_version != target_version
```

设备不能只接受更高版本；必须允许服务器明确要求安装历史版本。

## 5. 管理页面

`/manage` 至少需要展示：

- 板子型号和唯一 ID
- 设备名称
- 当前版本
- 目标版本
- 当前版本与目标版本是否一致
- 在线状态和最后心跳时间
- 运行状态
- 最近一次 OTA 结果
- 该板型的全部历史版本

管理端应允许将单台设备的目标版本设置为任意兼容的历史版本，以支持升级和主动降级。

## 6. OTA、降级与故障恢复

- OTA 使用两个应用分区和 OTA Data 分区。
- 下载的固件必须校验大小和 SHA-256。
- 新固件启动后执行基础硬件自检，并记录安装结果供下一次心跳上报。
- 当前 Arduino 预编译 Bootloader 没有启用“未确认镜像自动回滚”；不能把它误认为已有保障。
- 故障自动回滚与管理端指定历史版本是两个不同机制：
  - 故障自动回滚用于新固件无法正常启动，仍是后续需要单独实现和破坏性测试的增强项。
  - 指定历史版本用于有计划的主动降级。
- 因为系统需要允许主动降级，不启用阻止旧固件安装的 eFuse anti-rollback 策略。

## 7. 已冻结的实现约定

- 基础固件版本使用语义版本格式 `major.minor.patch`，允许预发布后缀。
- 具体应用允许使用 `{小写蛇形应用前缀}_{major.minor.patch}`，例如 `super_sonic_cleaner_presser_0.1.0`。
- 发布通过 `/api/manage/releases` 完成；相同 `{板型, 版本}` 拒绝再次发布。
- 在线阈值为 150 秒，延迟为 150 秒至 1 小时，超过 1 小时为离线。
- `/manage` 及管理 API 使用 HTTP Basic Auth，并且只通过 HTTPS 对外访问。
- 设备首次心跳时自动登记，不自动分配目标版本。
- 固件下载仅允许已登记设备下载当前明确分配给它的目标版本。

## 8. 后续安全增强

- 当前设备心跳依赖 HTTPS 传输安全，但设备本身尚无独立密钥或 token。
- 在设备规模扩大或部署到不可信网络前，应增加逐设备认证、密钥轮换和心跳防重放机制。

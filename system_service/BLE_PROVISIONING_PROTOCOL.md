# 蓝牙配网协议说明

本文档根据 `app/system_service` 当前实现整理，覆盖 BLE 传输层、`0x55AA` 应用帧、网络命令集子命令、异步推送和错误码。

相关源码：

- `ble_gatt.c`：BlueZ GATT/NUS 服务，负责 BLE 收发。
- `protocol.h` / `protocol.c`：`0x55AA` 帧编解码。
- `proto_dispatch.c`：协议分发和每条命令的处理逻辑。
- `wifi_switch.c` / `wifi_switch.h`：WiFi STA/AP 切换、扫描、AP 信息读取。

## 1. BLE 传输层

设备通过 BlueZ 暴露一个 Nordic UART Service 风格的 GATT 服务，手机端把它当作 BLE 串口使用。

| 项目 | 值 | 说明 |
| --- | --- | --- |
| 广播名 / Adapter Alias | `WFY` | `ble_gatt.c` 中 `DEVICE_NAME` 当前定义为 `WFY`。 |
| Service UUID | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | Nordic UART Service。 |
| RX Characteristic | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` | 手机写入，属性为 `write`、`write-without-response`。 |
| TX Characteristic | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` | 设备通知，属性为 `notify`。 |
| 配对方式 | Just Works | Agent 能力为 `NoInputNoOutput`。 |

手机端使用顺序：

1. 扫描并连接广播名为 `WFY`、包含 NUS UUID 的设备。
2. 订阅 TX Characteristic 的 notify。
3. 将协议帧写入 RX Characteristic。
4. 从 TX notify 接收同步响应 `RESP` 和异步推送 `PUSH`。

注意：

- 如果手机没有订阅 TX notify，设备端会丢弃要发回的响应或推送。
- `RX.WriteValue()` 收到的一次写入会作为一个缓冲区交给协议分发器。缓冲区内可以拼接多个完整 `0x55AA` 帧。
- 当前实现没有跨 BLE write 的分片重组。如果一个应用帧被拆成两次 BLE 写入，第一次会因为 `msg_len` 不足而解码失败。
- 协议层最大 payload 为 256 字节。扫描结果推送每帧实际控制在约 220 字节以内。

## 2. 应用帧格式

所有命令都封装在固定 10 字节头部后面，头部字段均按字节排列。

| 偏移 | 长度 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0 | 1 | magic[0] | 固定 `0x55`。 |
| 1 | 1 | magic[1] | 固定 `0xAA`。 |
| 2 | 2 | crc | 大端。当前保留，不校验；设备发包时填 `0x0000`。 |
| 4 | 1 | type | `0x00` = `REQ`，`0x01` = `RESP`。 |
| 5 | 1 | seq | 序号。响应和异步结果沿用请求的 seq。 |
| 6 | 1 | command_set | 命令集。当前只定义 `0x01 NETWORK`。 |
| 7 | 1 | command_id | 命令集内的子命令 ID。 |
| 8 | 2 | msg_len | payload 长度，大端。 |
| 10 | N | payload | `msg_len` 字节，可为空。 |

字段约定：

- 手机发起的命令使用 `type=0x00 REQ`。
- 设备同步回复使用 `type=0x01 RESP`，`seq`、`command_set`、`command_id` 与请求一致。
- 设备异步推送也使用 `type=0x00 REQ`，例如 `PUSH_RESULT`、`PUSH_SCAN_RESULT`。
- 设备收到 `type != REQ` 的帧只记录日志，不回复。
- 未知 `command_set` 或未知 `command_id` 会回复 `BAD_CMD`。
- payload 超过 256 字节、帧长度不足、magic 错误时，当前实现只记录解码错误，不会生成协议错误响应。

## 3. 类型、命令集和错误码

### 3.1 type

| 值 | 名称 | 方向 | 说明 |
| --- | --- | --- | --- |
| `0x00` | `REQ` | 手机到设备，或设备到手机 | 普通请求；设备异步推送也用这个类型。 |
| `0x01` | `RESP` | 设备到手机 | 对手机请求的同步响应。 |

### 3.2 command_set

| 值 | 名称 | 说明 |
| --- | --- | --- |
| `0x01` | `NETWORK` | 蓝牙配网相关命令。 |

### 3.3 通用错误码

多数 `RESP` 的 payload 是 1 字节错误码。

| 值 | 名称 | 说明 |
| --- | --- | --- |
| `0x00` | `NONE` | 成功 / 无错误。 |
| `0x01` | `BAD_CMD` | 不支持的命令集或子命令。 |
| `0x02` | `NO_SSID` | 没有目标 SSID，或扫描中未发现目标 SSID。 |
| `0x03` | `BAD_PASSWORD` | 密码错误，认证或四次握手失败。 |
| `0x04` | `CONNECT_FAIL` | 关联、DHCP 或其他连接步骤失败。 |
| `0x05` | `IN_PROGRESS` | 请求已接受，后台处理中，后续会有异步推送。 |
| `0xFF` | `UNKNOWN` | 内部错误、忙、参数不合法等未细分错误。 |

例外：`GET_WIFI_STATE` 的响应 payload 是 WiFi 状态字节，不是错误码。

## 4. NETWORK 子命令总表

| command_id | 名称 | 方向 | 同步响应 | 异步推送 | 作用 |
| --- | --- | --- | --- | --- | --- |
| `0x00` | `GET_WIFI_STATE` | 手机 -> 设备 | WiFi 状态字节 | 无 | 查询当前 STA 是否已联网。 |
| `0x01` | `STATUS` | 手机 -> 设备 | 错误码 | 无 | 设备存活/状态查询，当前只返回成功。 |
| `0x02` | `SSID` | 手机 -> 设备 | 错误码 | 无 | 暂存目标 WiFi SSID。 |
| `0x03` | `PASSWD` | 手机 -> 设备 | 错误码 | 无 | 暂存目标 WiFi 密码。 |
| `0x04` | `CONNECT` | 手机 -> 设备 | 错误码，通常 `IN_PROGRESS` | `PUSH_RESULT` | 使用暂存 SSID/密码连接 WiFi。 |
| `0x05` | `PUSH_RESULT` | 设备 -> 手机 | 无 | 本身就是推送 | 推送 `CONNECT` 的最终结果。 |
| `0x06` | `GET_AP_SSID` | 手机 -> 设备 | 错误码 + AP SSID | 无 | 读取设备当前 AP 热点 SSID。 |
| `0x07` | `GET_AP_PASSWD` | 手机 -> 设备 | 错误码 + AP 密码 | 无 | 读取设备当前 AP 热点密码。 |
| `0x08` | `SCAN_WIFI` | 手机 -> 设备 | 错误码，通常 `IN_PROGRESS` | `PUSH_SCAN_RESULT` | 触发 WiFi 扫描。 |
| `0x09` | `PUSH_SCAN_RESULT` | 设备 -> 手机 | 无 | 本身就是推送 | 分片推送扫描结果列表。 |
| `0x0A` | `SWITCH_TO_AP` | 手机 -> 设备 | 错误码 | 无 | 强制切换到 AP fallback 模式。 |

## 5. 子命令详解

### 5.1 `0x00 GET_WIFI_STATE`

作用：查询设备 `wlan0` 当前是否已经作为 STA 联网。

请求：

| 字段 | 值 |
| --- | --- |
| type | `REQ(0x00)` |
| command_set | `NETWORK(0x01)` |
| command_id | `0x00` |
| payload | 空 |

响应：

| payload | 说明 |
| --- | --- |
| `0x00` | 已连接。 |
| `0x01` | 未连接。 |

说明：

- 这个响应不是通用错误码。
- 判断依据是 `wifi_switch_is_connected()`：`wlan0` 有真实 IPv4 地址，不是 link-local 地址。

示例请求，`seq=0x01`：

```text
55 AA 00 00 00 01 01 00 00 00
```

### 5.2 `0x01 STATUS`

作用：设备状态/存活查询。当前实现没有额外状态字段，只返回成功。

请求：

| 字段 | 值 |
| --- | --- |
| type | `REQ(0x00)` |
| command_set | `NETWORK(0x01)` |
| command_id | `0x01` |
| payload | 空 |

响应 payload：

| 字节 | 含义 |
| --- | --- |
| `[0]` | 通用错误码，当前正常为 `0x00 NONE`。 |

### 5.3 `0x02 SSID`

作用：把手机选择的目标 WiFi SSID 暂存在进程内，供后续 `CONNECT` 使用。

请求 payload：

| 字段 | 长度 | 说明 |
| --- | --- | --- |
| ssid | 1..32 字节 | SSID 原始字节。通常是 UTF-8。 |

响应 payload：

| 字节 | 含义 |
| --- | --- |
| `[0]` | `0x00 NONE` 表示暂存成功；长度为 0 或大于 32 时返回 `0xFF UNKNOWN`。 |

实现细节：

- SSID 存在全局暂存区 `s_pending_ssid`。
- 不做 UTF-8 校验。
- 后续 `CONNECT` 不带 SSID，而是读取这个暂存值。

示例请求，`seq=0x02`，SSID 为 `ZTE-th99Ry`：

```text
55 AA 00 00 00 02 01 02 00 0A 5A 54 45 2D 74 68 39 39 52 79
```

成功响应：

```text
55 AA 00 00 01 02 01 02 00 01 00
```

### 5.4 `0x03 PASSWD`

作用：把目标 WiFi 密码暂存在进程内，供后续 `CONNECT` 使用。

请求 payload：

| 字段 | 长度 | 说明 |
| --- | --- | --- |
| password | 1..63 字节 | WPA/WPA2 PSK。 |

响应 payload：

| 字节 | 含义 |
| --- | --- |
| `[0]` | `0x00 NONE` 表示暂存成功；长度为 0 或大于 63 时返回 `0xFF UNKNOWN`。 |

实现细节：

- 密码存在全局暂存区 `s_pending_passwd`。
- 日志只记录长度，不打印密码。
- 协议处理器拒绝 0 长度 `PASSWD`。如果要连接开放网络，当前代码路径只能依赖“未设置密码”的暂存状态；但暂存密码没有清空子命令，手机端不应在同一服务进程中混用开放网络和带密码网络而不重新设置状态。

### 5.5 `0x04 CONNECT`

作用：触发设备使用暂存的 SSID/密码连接目标 WiFi。

请求：

| 字段 | 值 |
| --- | --- |
| type | `REQ(0x00)` |
| command_set | `NETWORK(0x01)` |
| command_id | `0x04` |
| payload | 当前实现不使用，建议为空 |

同步响应 payload：

| 值 | 说明 |
| --- | --- |
| `0x05 IN_PROGRESS` | 已创建后台连接线程，等待后续 `PUSH_RESULT`。 |
| `0x02 NO_SSID` | 还没有通过 `SSID` 命令暂存目标 SSID。 |
| `0xFF UNKNOWN` | 创建线程失败或其他内部错误。 |

异步推送：连接线程结束后，设备发送 `0x05 PUSH_RESULT`。

实现细节：

- `CONNECT` 不直接阻塞 BLE D-Bus 线程，而是创建后台线程。
- 后台线程调用 `wifi_switch_run_now(ssid, passwd)`，可能持续约 60 秒。
- 连接成功后会调用 `wifi_switch_persist_home_wifi()`，把凭据写入 `/etc/taishan_home_wifi.conf`，供下次开机使用。
- 如果连接失败，`wifi_switch_run_now()` 会尝试恢复 AP fallback，方便继续配网。

示例请求，`seq=0x04`：

```text
55 AA 00 00 00 04 01 04 00 00
```

同步响应，后台处理中：

```text
55 AA 00 00 01 04 01 04 00 01 05
```

### 5.6 `0x05 PUSH_RESULT`

作用：设备异步推送 `CONNECT` 的最终结果。

方向：设备 -> 手机。

帧字段：

| 字段 | 值 |
| --- | --- |
| type | `REQ(0x00)` |
| seq | 原始 `CONNECT` 请求的 seq |
| command_set | `NETWORK(0x01)` |
| command_id | `0x05` |

payload：

| 字节 | 含义 |
| --- | --- |
| `[0]` | 通用错误码。 |

常见结果：

| 值 | 说明 |
| --- | --- |
| `0x00 NONE` | 连接成功，且会尝试持久化凭据。 |
| `0x02 NO_SSID` | 扫描中未发现目标 SSID。 |
| `0x03 BAD_PASSWORD` | 密码错误。 |
| `0x04 CONNECT_FAIL` | 关联、DHCP 或其他连接步骤失败。 |
| `0xFF UNKNOWN` | 忙或内部错误。 |

说明：

- 这是设备发给手机的推送，不是手机应主动发送的命令。
- 设备端协议分发器没有处理手机发来的 `PUSH_RESULT`；手机若发送该子命令会收到 `BAD_CMD`。

示例推送，`seq=0x04`，连接成功：

```text
55 AA 00 00 00 04 01 05 00 01 00
```

### 5.7 `0x06 GET_AP_SSID`

作用：读取设备 AP fallback 模式下 hostapd 正在广播的 SSID。

请求：

| 字段 | 值 |
| --- | --- |
| type | `REQ(0x00)` |
| command_set | `NETWORK(0x01)` |
| command_id | `0x06` |
| payload | 空 |

响应 payload：

| 字段 | 长度 | 说明 |
| --- | --- | --- |
| errcode | 1 字节 | `0x00 NONE` 表示成功；失败为 `0xFF UNKNOWN`。 |
| ap_ssid | 0..255 字节 | 当 errcode 为 `NONE` 时跟随，内容来自 `/etc/hostapd.conf` 的 `ssid=`。 |

说明：

- 当前实现通过 `wifi_switch_get_ap_credentials()` 解析 `/etc/hostapd.conf`。
- 如果读取不到 SSID 或 SSID 为空，返回 `UNKNOWN`。

### 5.8 `0x07 GET_AP_PASSWD`

作用：读取设备 AP fallback 模式下 hostapd 的热点密码。

请求：

| 字段 | 值 |
| --- | --- |
| type | `REQ(0x00)` |
| command_set | `NETWORK(0x01)` |
| command_id | `0x07` |
| payload | 空 |

响应 payload：

| 字段 | 长度 | 说明 |
| --- | --- | --- |
| errcode | 1 字节 | `0x00 NONE` 表示成功；失败为 `0xFF UNKNOWN`。 |
| ap_passwd | 0..255 字节 | 当 errcode 为 `NONE` 时跟随，内容来自 `/etc/hostapd.conf` 的 `wpa_passphrase=`。开放 AP 时可能为空。 |

说明：

- 日志不会打印 AP 密码，只记录长度。

### 5.9 `0x08 SCAN_WIFI`

作用：触发设备扫描附近 WiFi，并异步返回扫描列表。

请求：

| 字段 | 值 |
| --- | --- |
| type | `REQ(0x00)` |
| command_set | `NETWORK(0x01)` |
| command_id | `0x08` |
| payload | 当前实现不使用，建议为空 |

同步响应 payload：

| 值 | 说明 |
| --- | --- |
| `0x05 IN_PROGRESS` | 已创建后台扫描线程，等待后续 `PUSH_SCAN_RESULT`。 |
| `0xFF UNKNOWN` | 创建线程失败或内部错误。 |

异步推送：扫描结束后，设备发送一个或多个 `0x09 PUSH_SCAN_RESULT`。

实现细节：

- 扫描通过 `iw dev wlan0 scan` 获取结果，最多重试 3 次。
- 扫描结果最多保留 64 个 AP。
- 隐藏 SSID 或 0 长度 SSID 会被过滤。
- 当前实现不会为了扫描关闭 hostapd，AP 广播会保持。
- 如果扫描失败，仍会推送一个空结果结束帧：`frame_index=0`、`flags=0x01`、`entry_count=0`。

### 5.10 `0x09 PUSH_SCAN_RESULT`

作用：设备异步推送 WiFi 扫描结果。扫描结果可能分多帧发送。

方向：设备 -> 手机。

帧字段：

| 字段 | 值 |
| --- | --- |
| type | `REQ(0x00)` |
| seq | 原始 `SCAN_WIFI` 请求的 seq |
| command_set | `NETWORK(0x01)` |
| command_id | `0x09` |

payload 头部：

| 偏移 | 长度 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0 | 1 | frame_index | 分片序号，从 0 开始递增。 |
| 1 | 1 | flags | bit0 = `end_of_list`。为 1 表示最后一帧。 |
| 2 | 1 | entry_count | 本帧包含的 WiFi 条目数量。 |

payload 后续是 `entry_count` 个变长条目。

单个 WiFi 条目格式：

| 字段 | 长度 | 说明 |
| --- | --- | --- |
| ssid_len | 1 字节 | SSID 长度，范围 1..32。 |
| ssid | ssid_len 字节 | SSID 原始字节，通常按 UTF-8 显示。 |
| rssi | 1 字节 | 有符号 int8，单位 dBm。例如 `0xC4` 表示 -60。 |
| auth_type | 1 字节 | 认证类型，见下表。 |
| band | 1 字节 | 频段，见下表。 |

认证类型 `auth_type`：

| 值 | 名称 | 说明 |
| --- | --- | --- |
| `0` | OPEN | 开放网络。 |
| `1` | WEP | WEP。 |
| `2` | WPA_PSK | WPA-PSK。 |
| `3` | WPA2_PSK | WPA2-PSK；WPA2/WPA3 transition 模式也会优先报这个值。 |
| `4` | WPA3_SAE | WPA3-SAE only。 |
| `5` | ENTERPRISE | 企业认证 / 802.1X / EAP。 |
| `255` | UNKNOWN | 未知类型，当前扫描解析正常路径很少产生。 |

频段 `band`：

| 值 | 说明 |
| --- | --- |
| `0` | 2.4 GHz。 |
| `1` | 5 GHz。 |

分片规则：

- 每帧最多编码 5 个 WiFi 条目。
- 单帧扫描 payload 目标上限为 220 字节，协议绝对上限仍是 256 字节。
- 手机端应按 `frame_index` 递增接收，直到 `flags & 0x01 != 0`。
- 空扫描结果也是合法结果：`entry_count=0` 且 `end_of_list=1`。

说明：

- 这是设备发给手机的推送，不是手机应主动发送的命令。
- 设备端协议分发器没有处理手机发来的 `PUSH_SCAN_RESULT`；手机若发送该子命令会收到 `BAD_CMD`。

### 5.11 `0x0A SWITCH_TO_AP`

作用：强制设备进入 AP fallback 模式，便于手机重新连接设备热点或重新配网。

请求：

| 字段 | 值 |
| --- | --- |
| type | `REQ(0x00)` |
| command_set | `NETWORK(0x01)` |
| command_id | `0x0A` |
| payload | 当前实现不使用，建议为空 |

响应 payload：

| 字节 | 含义 |
| --- | --- |
| `[0]` | 通用错误码。成功为 `0x00 NONE`。 |

实现细节：

- 调用 `wifi_switch_ensure_ap_mode()`。
- 如果 AP 已经在运行，直接返回成功。
- 如果当前 STA 活跃，会停止 STA 并启动 hostapd + dnsmasq，AP IP 为 `192.168.50.1`。
- 如果 WiFi 切换/扫描正在进行中，后端返回 busy，但协议层会映射成 `0xFF UNKNOWN`。

## 6. 典型配网流程

### 6.1 查询设备是否已经联网

1. 手机订阅 TX notify。
2. 手机发送 `GET_WIFI_STATE`。
3. 设备返回：
   - `0x00`：已经联网，可结束配网。
   - `0x01`：未联网，继续扫描或输入 SSID。

### 6.2 扫描 WiFi 并选择目标网络

1. 手机发送 `SCAN_WIFI`。
2. 设备同步返回 `IN_PROGRESS`。
3. 手机等待 `PUSH_SCAN_RESULT` 分片。
4. 手机收集到 `flags bit0 = 1` 的最后一帧后，显示 WiFi 列表。

### 6.3 下发凭据并连接

1. 手机发送 `SSID`，payload 为目标 SSID。
2. 设备返回 `NONE`。
3. 手机发送 `PASSWD`，payload 为目标 WiFi 密码。
4. 设备返回 `NONE`。
5. 手机发送 `CONNECT`。
6. 设备同步返回 `IN_PROGRESS`。
7. 手机等待 `PUSH_RESULT`：
   - `NONE`：配网成功。
   - `NO_SSID`：设备扫描不到该 WiFi。
   - `BAD_PASSWORD`：密码错误。
   - `CONNECT_FAIL`：连接或 DHCP 失败。
   - `UNKNOWN`：忙或内部错误，可提示重试。

### 6.4 重新进入 AP 模式

1. 手机发送 `SWITCH_TO_AP`。
2. 设备返回 `NONE` 表示 AP 模式已可用或已经处于 AP 模式。
3. 手机可通过 `GET_AP_SSID` / `GET_AP_PASSWD` 获取设备热点信息。

## 7. 手机端实现注意点

- 先订阅 TX notify，再发送任何需要响应的命令。
- 每次请求使用新的 `seq`，异步推送靠相同 `seq` 关联原请求。
- `GET_WIFI_STATE` 响应不是错误码，不能用通用错误码表解析。
- `CONNECT` 和 `SCAN_WIFI` 是两阶段命令：先收同步 `RESP IN_PROGRESS`，再收异步 push。
- 设备 push 使用 `type=REQ`，手机端不要把它误认为需要回写响应的普通请求。
- 写入 RX 的应用帧不要跨 BLE write 分片；如需发送大 payload，应在手机端保证单帧完整写入。
- 当前没有清空暂存 SSID/密码的子命令。手机端每次配网都应按 `SSID` -> `PASSWD` -> `CONNECT` 顺序完整下发，避免复用旧状态。

## 8. 快速示例

以下示例均使用 `command_set=0x01 NETWORK`，CRC 填 `0x0000`。

查询 WiFi 状态，`seq=0x01`：

```text
REQ : 55 AA 00 00 00 01 01 00 00 00
RESP: 55 AA 00 00 01 01 01 00 00 01 00   # 已连接
RESP: 55 AA 00 00 01 01 01 00 00 01 01   # 未连接
```

设置 SSID 为 `ZTE-th99Ry`，`seq=0x02`：

```text
REQ : 55 AA 00 00 00 02 01 02 00 0A 5A 54 45 2D 74 68 39 39 52 79
RESP: 55 AA 00 00 01 02 01 02 00 01 00
```

触发连接，`seq=0x04`：

```text
REQ : 55 AA 00 00 00 04 01 04 00 00
RESP: 55 AA 00 00 01 04 01 04 00 01 05   # IN_PROGRESS
PUSH: 55 AA 00 00 00 04 01 05 00 01 00   # 最终成功
```

扫描 WiFi，`seq=0x08`，空结果结束帧示例：

```text
REQ : 55 AA 00 00 00 08 01 08 00 00
RESP: 55 AA 00 00 01 08 01 08 00 01 05   # IN_PROGRESS
PUSH: 55 AA 00 00 00 08 01 09 00 03 00 01 00
```

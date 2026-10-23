# ESP32-U4WDH 蓝牙协处理器固件

本目录是在 ESP-IDF 官方 `examples/bluetooth/bluedroid/classic_bt/a2dp_source`
基础上修改的 U4 固件，用于把 ESP32-P4 的音频经 Classic BT A2DP 输出到蓝牙音箱。
除上游 A2DP Source 功能外，还加入了与 P4 的 SDIO Slave 板间链路：

- 16 KiB 内部 PCM 环形缓冲；P4 通过 SDIO 送入 16-bit 立体声 PCM；
- 未检测到 P4 时，连接音箱后播放 440 Hz 测试音，便于单独验证蓝牙；
- 收到 P4 的第一个合法包后关闭测试音，进入主机控制模式；
- 支持扫描、连接、断开、音量、媒体启停命令和扫描/状态/连接事件；
- 板间协议定义见 `main/link_protocol.h`。

## 构建

```bash
cd u4_a2dp
idf.py set-target esp32
idf.py build
```

默认配置为 4 MB Flash、`Single factory app (large)`。

## 一次性硬件准备：烧写 eFuse 固定 VDD_SDIO = 3.3 V

### 为什么必须烧

ESP32-U4WDH 使用 **3.3 V 片内 Flash**。ESP32 每次复位时用 strapping 引脚
MTDI（GPIO12）选择 VDD_SDIO 电压：低电平 = 3.3 V，高电平 = 1.8 V。
而 4-bit SDIO 的 DAT2 恰好也是 GPIO12，按 SDIO 规范需要 10–90 kΩ 上拉，
两者直接冲突：

- DAT2 加上拉 → 复位时 GPIO12 为高 → ROM 按 1.8 V 给 Flash 供电，3.3 V Flash 无法启动；
- DAT2 不加拉 → 启动正常，但 4-bit SDIO 缺少一条数据线。

烧写 `XPD_SDIO_FORCE` / `XPD_SDIO_REG` / `XPD_SDIO_TIEH` 三个 eFuse 后，
VDD_SDIO 被永久固定为 3.3 V，GPIO12 不再参与电压选择，DAT2 上拉可以保留，
4-bit SDIO 正常工作。本工程固件（`main/sdio_link.c`）默认按 4-bit 打开，
即依赖该 eFuse。

> ⚠️ **eFuse 是一次性、不可逆操作**。只能对确认是 3.3 V Flash 的芯片
> （ESP32-U4WDH 符合）执行；烧写期间断电可能永久损坏芯片。若无法接受，
> 请改用文末的 1-bit SDIO / SPI 方案。

### 1. 准备

- 确认芯片型号为 ESP32-U4WDH（4 MB 片内 3.3 V Flash）；
- 提供稳定 3.3 V 供电和可靠的 UART（TX / RX / GND）；
- 在 ESP-IDF 命令行环境中操作（`espefuse.py` 随 IDF 一起提供，esptool v4.12）。
  若提示找不到命令，先运行 `export.bat` / `export.ps1`，或直接使用
  `%IDF_PATH%\components\esptool_py\esptool\espefuse.py`（其本质是
  `python -m espefuse`）；
- 在 Windows 设备管理器中确认 U4 的串口号，下文用 `COMx` 表示；
- 先按下一节进入下载模式，确认能正常连上芯片：

```bat
esptool.py -p COMx --chip esp32 read_mac
```

### 2. 进入下载模式（关键）

```text
复位瞬间：GPIO0 = 0，GPIO2 = 0，CHIP_PU(EN) 走一次低 → 高
```

- ROM 的 UART 下载模式要求 **GPIO0 和 GPIO2 同时为低**；
- 本项目 PCB 上 IO2/DAT0 有 SDIO 上拉电阻，会把 GPIO2 拉高，从而阻止进入下载
  模式。首次烧录和烧 eFuse 时必须临时断开该上拉，或在复位期间用跳线把 IO2
  拉到 GND；此时 P4 必须复位/断电，否则 P4 的 IO14 会驱动同一网络。若 PCB
  没有给 DAT0/IO2 加上拉，IO2 悬空时 ROM 内部默认下拉即可满足 GPIO2=0，
  不需要额外处理；
- **IO12/DAT2 的上拉不需要拆除**：烧写走 UART 下载模式，不访问片内 Flash，
  上拉不影响；烧写完成后它正好保留给 4-bit SDIO；
- PCB 上 `CHIP_PU` 接在 P4 的 IO54：烧写时需让 P4 不再驱动该脚（P4 复位/断电，
  或用临时固件把 IO54 设为输入），避免与编程器抢 EN；
- `CHIP_PU` 只能复位，不能烧录；必须有 UART0 TX/RX 通路。建议在 PCB 上预留
  U4 的 EN / GPIO0 / GPIO2 / TX / RX 测试点。

### 3. 备份当前 eFuse

```bat
espefuse.py -p COMx --chip esp32 summary
```

- 输出包含 MAC 和全部 eFuse 字段，建议保存一份文本备份；
- 烧写前，最后一行通常为：

```text
Flash voltage (VDD_SDIO) determined by GPIO12 on reset (High for 1.8V, Low/NC for 3.3V)
```

### 4. 烧写

```bat
espefuse.py -p COMx --chip esp32 set_flash_voltage 3.3V
```

工具会打印将要烧写的位：

```text
Enable internal flash voltage regulator (VDD_SDIO) to 3.3V.
The following eFuses are burned: XPD_SDIO_FORCE, XPD_SDIO_REG, XPD_SDIO_TIEH.
```

随后要求输入 `BURN` 确认。**确认电压是 3.3V、芯片是 ESP32-U4WDH 后再输入**；
烧写期间不要断电、不要拔线。成功后会看到 `BURN BLOCK0 - OK` 等输出。

- `--do-not-confirm` 可以跳过输入确认，但不清楚后果时不要使用；
- 该操作不会擦除 Flash，不影响已烧录的固件；
- 新版 esptool（5.x）命令改为连字符：`espefuse set-flash-voltage 3.3V`。

### 5. 验证

```bat
espefuse.py -p COMx --chip esp32 summary
```

确认以下三项为 1（字段顺序因版本略有差异）：

```text
XPD_SDIO_FORCE   1
XPD_SDIO_REG     1
XPD_SDIO_TIEH    1
```

并且 summary 末尾变为：

```text
Flash voltage (VDD_SDIO) set to 3.3V by efuse.
```

此时 VDD_SDIO 永久为 3.3 V，GPIO12 高低不再影响 Flash 电压，DAT2 上拉可保留，
4-bit SDIO 可用。

### 6. 之后每次给 U4 刷固件

- eFuse 只固定电压，不改变下载模式要求：仍然需要复位瞬间 GPIO0=0、GPIO2=0；
- 也就是说 **IO2/DAT0 上拉的问题每次烧录都会遇到**，建议在 PCB 上为
  IO0 / IO2 / EN / TX / RX 预留测试点或跳线；
- 正常运行时（GPIO0=1）GPIO2 是 don't-care，上拉照常保留，不影响 SDIO 工作；
- 刷固件使用 `idf.py -p COMx flash monitor` 即可。

### 7. 常见错误

| 现象 | 原因 |
| --- | --- |
| `Wrong boot mode detected (0x13)` / `Failed to connect ... Wrong boot mode` | 复位时 GPIO0、GPIO2 没有同时为低，检查 IO2 上拉和跳线 |
| `Timed out waiting for packet header` | 没有进入下载模式，或 TX/RX 接反、供电不稳 |
| `espefuse.py: command not found` | 不在 IDF 命令行环境，运行 `export.bat`/`export.ps1`，或用完整路径 |
| `Can't set regulator to 1.8V if XPD_SDIO_TIEH efuse is already burned` | 该芯片已固定 3.3 V，无需（也无法）再改回 1.8 V |

### 8. 不烧 eFuse 的替代方案

如果不想做不可逆操作，可以改用：

1. **1-bit SDIO**：把 `main/sdio_link.c` 中 `sdio_slave_config_t.flags` 从 `0`
   改为 `SDIO_SLAVE_FLAG_DAT2_DISABLED`，P4 侧按 1-bit 连接；
2. **SPI 模式**：使用 CLK / MOSI / MISO / CS + DAT1 中断，P4 作为 SPI 主机，
   协议需自行实现。PCM 码率约 1.5 Mbps，SPI 足以胜任。

这两种方案都要求 U4 复位时 DAT2 不被外部上拉保持为高；若 PCB 已焊上拉，
就需要改板，或者仍然烧 eFuse。

## 烧录前必须设置音箱名称

独立验证时，固件会按 `CONFIG_EXAMPLE_PEER_DEVICE_NAME` 自动连接音箱：

```text
idf.py menuconfig
  A2DP Example Configuration -> Target Device Name
```

或在 `sdkconfig` 中修改：

```text
CONFIG_EXAMPLE_PEER_DEVICE_NAME="你的音箱名称"
```

名称必须与音箱实际广播名称完全一致；否则会一直扫描。接入 P4 后可以由
`LINK_CMD_CONNECT` 按 Bluetooth 地址连接，不再依赖该名称。

U4 下载模式：GPIO0=0、GPIO2=0；`CHIP_PU` 复位。注意 IO2/DAT0 的 SDIO 上拉会阻止
进入下载模式，处理方式见「一次性硬件准备」一节。烧写 `XPD_SDIO_*` eFuse 后
VDD_SDIO 固定为 3.3 V，IO12 的 SDIO DAT2 上拉可以保留（本项目所用 U4 已完成烧写，
复刻时必须自行完成）。

## 与 P4 的接口

- SDIO Slave 固定引脚：CLK=14、CMD=15、DAT0=2、DAT1=4、DAT2=12、DAT3=13；
- P4 侧对应 SDMMC slot1：clk=18、cmd=19、d0=14、d1=15、d2=16、d3=17；
- 包格式、寄存器、命令/事件和 4 字节对齐规则见 `main/link_protocol.h`。

---

以下为上游 ESP-IDF A2DP Source 例程说明：

| Supported Targets | ESP32 |
| ----------------- | ----- |

A2DP-SOURCE EXAMPLE
========================

Example of A2DP audio source role

This is the example of using Advanced Audio Distribution Profile (A2DP) APIs to transmit audio stream. Application can take advantage of this example to implement portable audio players or microphones to transmit audio stream to A2DP sink devices.

## How to use this example

### Hardware Required

This example is able to run on any commonly available ESP32 development board, and is supposed to connect to [A2DP sink example](../a2dp_sink_stream) in ESP-IDF.

### Configure the project

```
idf.py menuconfig
```

* Enable Classic Bluetooth and A2DP under Component config --> Bluetooth --> Bluedroid Enable

### Build and Flash

Build the project and flash it to the board, then run monitor tool to view serial output.

```
idf.py -p PORT flash monitor
```

(Replace PORT with the name of the serial port to use.)

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.

## Example Output

For the first step, this example performs device discovery to search for a target device (A2DP sink) whose device name is "ESP_SPEAKER" and whose "Rendering" bit of its Service Class field is set in its Class of Device (COD). If a candidate target is found, the local device will initiate connection with it.

After connection with A2DP sink is established, the example performs the following running loop 1-2-3-4-1:
1. audio transmission starts and lasts for a while
2. audio transmission stops
3. disconnect with target device
4. reconnect to target device

The example implements an event loop triggered by a periodic "heart beat" timer and events from Bluetooth protocol stack callback functions.

After the local device discovers the target device and initiates connection, there will be logging message like this:

```
I (4090) BT_AV: Found a target device, address xx:xx:xx:xx:xx:xx, name ESP_SPEAKER
I (4090) BT_AV: Cancel device discovery ...
I (4100) BT_AV: Device discovery stopped.
I (4100) BT_AV: a2dp connecting to peer: ESP_SPEAKER
```

If connection is set up successfully, there will be such message:

```
I (5100) BT_AV: a2dp connected
```

Starting of audio transmission has the following notification message:

```
I (10880) BT_AV: a2dp media ready checking ...
...
I (10880) BT_AV: a2dp media ready, starting ...
...
I (11400) BT_AV: a2dp media start successfully.
```

Stop of audio transmission, and disconnection with remote device generate the following notification message:

```
I (110880) BT_AV: a2dp media stopping...
...
I (110920) BT_AV: a2dp media stopped successfully, disconnecting...
...
I (111040) BT_AV: a2dp disconnected
```

## Troubleshooting
* For current stage, the supported audio codec in ESP32 A2DP is SBC. SBC audio stream is encoded from PCM data normally formatted as 44.1kHz sampling rate, two-channel 16-bit sample data.
* The raw PCM media stream in the example is generated by a sequence of random number, so the sound played on the sink side will be piercing noise.
* As a usage limitation, ESP32 A2DP source can support at most one connection with remote A2DP sink devices. Also, A2DP source cannot be used together with A2DP sink at the same time, but can be used with other profiles such as SPP and HFP.
* Usually, the `ACL_CONN_NUM` is set to `2` but not `1` as one is used for a2dp connection and the other is used for avrcp connection.

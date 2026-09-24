# HEI_MAO_POD2 引脚分配表（Pin Map）

> **本文件只记录「哪个外设接哪个引脚」。** 系统架构、软件设计、生产工艺等其它内容属于别的文档，不要往这里加。

**Agent 使用规则**

1. 改任何引脚（代码 / Kconfig / 原理图）**必须同步本文件**，并在 §4 变更记录追加一行。
2. 引脚号统一写 `GPIOxx`。P4 与 ESP32 两侧引脚号**同名不同芯**，读表时注意所属芯片列。
3. 分配新引脚前，先看 §3「P4 不可用 / 保留引脚」。
4. 标记约定：`[已定]` = 硬件已定，不得擅改；`[建议]` = 可改，但改了要回写。

| 版本 | 更新日期 | 主控 | 协处理器 |
| --- | --- | --- | --- |
| v0.11 | 2026-09-24 | **ESP32-P4NRW32（芯片版本 v3.2）** | ESP32-U4WDH |

---

## 1. ESP32-P4 引脚分配

### 1.1 协处理器 SDIO 链路（SDMMC Slot 1）—— `[已定]`

| 信号 | P4 引脚 | 对端 ESP32 引脚 | 备注 |
| --- | --- | --- | --- |
| SDIO_CLK | **GPIO18** | GPIO14 | 预留串阻 22–33 Ω |
| SDIO_CMD | **GPIO19** | GPIO15 | 51 kΩ 上拉 |
| SDIO_D0 | **GPIO14** | GPIO2 | 51 kΩ 上拉 |
| SDIO_D1 | **GPIO15** | GPIO4 | 51 kΩ 上拉 |
| SDIO_D2 | **GPIO16** | GPIO12 | 51 kΩ 上拉；**ESP32 侧有 eFuse 约束，见 §2.1** |
| SDIO_D3 | **GPIO17** | GPIO13 | 51 kΩ 上拉 |
| COPROC_EN（复位） | **GPIO54** | EN (CHIP_PU) | 主机上电后拉低一次，同步主从状态 |

> Slot1 经 GPIO Matrix，P4 侧引脚可改；此处沿用 ESP32-P4-Function-EV-Board 映射，ESP-Hosted 默认配置开箱可用。全部 3.3 V，走线等长（优先 CLK 与数据线等长）。

### 1.2 显示与触摸 —— 触摸 `[已定]` / DSI `[建议]`

| 功能 | P4 引脚 | 备注 |
| --- | --- | --- |
| MIPI-DSI（CO5300 460×460，1-lane） | `DSI_*` **专用 PHY 焊盘** | **不占 GPIO**；`DSI_REXT` 需 4.02 kΩ 到地 |
| VDD_MIPI_DPHY（2.5 V） | 内部 LDO **VO3 (VDDO_3)** | 代码 `EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN = 3` |
| LCD 复位 | 未使用 | 代码 `EXAMPLE_PIN_NUM_LCD_RST = -1` |
| 触摸 I2C SCL / SDA | **GPIO8 / GPIO7** | `I2C_NUM_0`，400 kHz |
| 触摸 RST | **GPIO5** | 硬件已接；代码里现为 `GPIO_NUM_NC`，需改。GPIO5 = `MTDO`(JTAG) |
| 触摸 INT | **GPIO6** | 硬件已接；代码里现为 `NC`，需改。LP 域，可作待机唤醒源 |

### 1.3 I2C 总线（两路）—— `[已定]`

| 总线 | SCL | SDA | 挂载设备 | 控制器 |
| --- | --- | --- | --- | --- |
| I2C-0（触摸专用） | **GPIO8** | **GPIO7** | CST820 触摸 | `I2C_NUM_0`（HP），400 kHz |
| I2C-1（外设/控制） | **GPIO3** | **GPIO4** | CS43131（DAC）、AXP2101（PMIC）、BMI270（IMU）、PCF85063（RTC） | **待定**（HP `I2C_NUM_1` 或 LP_I2C） |

> - **必须外部上拉 2.2–4.7 kΩ**（照总线电容算）。代码里的 `enable_internal_pullup` 仅供飞线验证，官方明确内部上拉在高频下不够用。
> - 上拉电源须与总线 GPIO 同电源域（两路都在 `VDD_LP` 常电域）。
> - GPIO3 = `MTDI`、GPIO4 = `MTMS`，本体是 JTAG 脚；但 P4 默认 JTAG 来自 USB-Serial/JTAG，故可占用 → **本设计不再具备引脚 JTAG 能力**。
> - 每路各自维护从机地址表，避免同路撞地址。

**I2C-1 从机的中断脚**

| 从机 | INT 引脚 | 备注 |
| --- | --- | --- |
| AXP2101（PMIC） | **GPIO10** | LP 域，可作深睡唤醒源（电源键、充电器插拔等事件经它上报） |
| PCF85063TP（RTC） | **GPIO11** | LP 域。⚠ **TP 版无 Alarm / Timer**（手册 Table 35：*basic functions only, no alarm*），INT 只能给 MI(每分钟)/HMI(30 秒)/CIE；若要闹钟唤醒须换 **PCF85063A**（封装不同：SO8 / DFN2626-10 / TSSOP8），详见外设寄存器手册 |
| BMI270（IMU） | **GPIO12** | LP 域，可作双敲唤醒；若确定不需要 IMU 唤醒，可改挂普通空闲脚 |

> 极性/上拉按各自 datasheet 确认（PCF85063 的 INT 为开漏低有效，需上拉）。
> ⚠ **LP 域（GPIO0–GPIO15）空闲引脚至此只剩 GPIO13 与 GPIO0–GPIO2**，后续需要深睡唤醒的按键/信号请从这里出。

### 1.4 microSD 卡座（SDMMC Slot 0）—— `[已定]`

| 卡座引脚 | P4 引脚 | 说明 |
| --- | --- | --- |
| CLK | **GPIO43** | |
| CMD | **GPIO44** | |
| DAT0 | **GPIO39** | |
| DAT1 | **GPIO40** | 1-bit 模式不用，上拉仍保留 |
| DAT2 | **GPIO41** | 同上 |
| CD/DAT3 | **GPIO42** | **上拉强制**（复位时被拉低会让卡进 SPI 模式） |
| CARD_DET | **GPIO9** `[建议]` | 卡座检测开关另一端接 GND，插卡=低。LP 域，可作插卡唤醒；备选 GPIO49–GPIO53 |
| D4–D7 | GPIO45–GPIO48 | 仅 8-bit eMMC 用，SD 卡不接 |

> - Slot0 信号经 IO MUX **硬固定**在 GPIO39–44，不可自选；Slot1 已被协处理器占用，故卡必须走 Slot0。
> - CLK/CMD/DAT0–DAT3 各 10 kΩ 上拉 → 3.3 V；**每条线预留串阻位**（默认 0 Ω，调试可换 22–33 Ω）；CLK 另预留对地 ≤10 pF 电容位。
> - **VDD_IO_5（芯片第 85 脚，IDF 称 `VDDPST_5`/`SD_VREF`）必须供电**：不做 UHS-I 时直连 3.3 V，上拉电源与它同网络。建议再预留一条经 **VDDO_4**（内部 LDO 通道 4）的 0 Ω 通路，以便日后切 1.8 V 上 UHS-I。
> - 卡电源接系统 3.3 V。速率：4-bit @ 40 MHz 已远超播放 FLAC 所需，**本设计不需要 UHS-I**。

### 1.5 I2S → CS43131（音频 DAC）—— `[已定]`

| 信号 | P4 引脚 | 方向 | 说明 |
| --- | --- | --- | --- |
| SCLK（BCLK） | **GPIO28** | out | P4 作 I2S master |
| LRCLK（WS） | **GPIO29** | out | |
| SDIN → DAC 的 DIN | **GPIO30** | out | 对应 IDF 的 `i2s_std_gpio_config_t::dout`，**别配成 `din`** |
| RST | **GPIO31** | out | DAC 复位 |
| INT | **GPIO21** | in | 中断输入（确认是否需外部上拉）。原定 GPIO32，因 v3.x 疑似 strapping 而改，见 §3；layout 不便时可改用 GPIO49–GPIO53 |

> - **DAC 自带独立晶振，P4 不提供 MCLK**（因此无需 512×fs MCLK 输出）。
> - DAC 的控制接口走 I2C-1（SCL=GPIO3 / SDA=GPIO4，见 §1.3）。
> - 5 个信号全部经电平转换芯片到 DAC。⚠ 若转换芯片是 **TXS0108/TXB0108 这类自动方向器件**，会给 SCLK 引入额外抖动，建议换**带方向脚的单向转换**（SN74LVC1T45/2T45 类）；RST/INT 是慢信号，不受影响。若转换芯片有 OE/EN 脚，确认是常使能还是要 GPIO 控制。
> - 软件：I2S 时钟源用 **APLL**（否则 44.1k 族采样率有偏差/抖动）；`slot_bit_width` 用 24-bit 时 `mclk_multiple` 必须取 3 的倍数（如 384），否则 WS 不准。

### 1.6 按键 —— `[已定]`

| 按键 | P4 引脚 | 备注 |
| --- | --- | --- |
| 音量键 A（兼 BOOT / 下载键） | **GPIO35**（硬件已接） | **strapping 脚**：复位时须为高，按住复位会进 Joint Download Boot；**非 LP 域，不能作深睡唤醒源**；按键直接对地，禁止在该脚加大电容 |
| 音量键 B | **GPIO20** | 普通 GPIO，内部上拉 + 按键对地 |
| 电源键 | **不占 P4 引脚** | 接 AXP2101 的 `PWRON`。P4 侧读键状态/事件走 I2C-1 读 PMIC 状态寄存器，并靠 AXP2101 的 IRQ（**GPIO10**，LP 域，见 §1.3）产生中断与深睡唤醒 |

> - 音量键不需要 LP 域（深睡时无音频可调），与 GPIO35 本身非 LP 的属性一致。
> - 若要靠 GPIO35 进下载模式：官方提醒复位时 **GPIO36 也需为高**才可靠。

### 1.7 环境光传感器 ALS-PT19-315C —— ADC

| 信号 | P4 引脚 | 备注 |
| --- | --- | --- |
| 分压点（传感器 3.3 V → 10 kΩ → GND 的中点） | **GPIO22**（ADC1_CH6）`[建议]` | 同为 ADC1 可用脚：GPIO21（CH5）、GPIO23（CH7），layout 需要时三选一 |

> - 走 **ADC1** 通道；12-bit + 12 dB 衰减覆盖 0–3.3 V。传感器供电就是 3.3 V，与 ADC 同域，**无需电平转换**。
> - 分压点源阻抗约 10 kΩ 偏高，建议 ADC 脚对地加 0.1 µF 并在驱动里调长采样时间。
> - ⚠ 按 ALS-PT19-315C 的 Iph–照度曲线核算一下 10 kΩ 下的满量程照度：若几百 lux 就顶到 3.3 V，室内会长期饱和（自动亮度失去意义），此时把负载电阻换小（如 2.2 kΩ / 1 kΩ），建议预留并联焊盘。
> - 深睡时不读环境光（屏已关，无意义），故不需要 LP 域 ADC 引脚。

### 1.8 待分配

- 状态 LED / 背光使能
- 可选：`COPROC_BOOT`（P4 → ESP32 GPIO0，1 个 GPIO），用于产线经 SDIO 首烧/救砖
- 可选：BT HCI 专用 UART 备份通道（4 线含 RTS/CTS）

---

## 2. ESP32-U4WDH 引脚分配（协处理器）

### 2.1 SDIO Slave 固定引脚 —— `[已定，不可更改]`

| 信号 | ESP32 引脚 | 对端 P4 |
| --- | --- | --- |
| CLK | **GPIO14** | GPIO18 |
| CMD | **GPIO15** | GPIO19 |
| DAT0 | **GPIO2** | GPIO14 |
| DAT1 | **GPIO4** | GPIO15 |
| DAT2 | **GPIO12** | GPIO16 |
| DAT3 | **GPIO13** | GPIO17 |
| EN（复位） | **EN / CHIP_PU** | GPIO54 |

> 这 6 个脚在 ESP32 上是**硬固定的**，无法用 GPIO Matrix 重映射，且不可再作他用。

⚠ **必读：DAT2 = GPIO12 = MTDI strapping**
SDIO 规范强制 DAT2 上拉，而 GPIO12 在复位时决定内部 LDO `VDD_SDIO` 的电压（1 → 1.8 V）；U4WDH 的**内置 Flash 是 3.3 V 器件**，被拉高会导致芯片起不来。处理办法：

1. 产线烧一次 eFuse 永久锁 3.3 V：`espefuse set-flash-voltage 3.3V`（**不可逆**）。
2. 因该上拉会妨碍首次启动，DAT2 的 51 kΩ 上拉建议做成**跳线 / 0 Ω 可选**，首烧时断开；或预留 `GPIO0` 测试点，从下载模式完成 eFuse 烧录。
3. 另注意 GPIO2 = DAT0：复位时若被拉低会连同 GPIO0 一起进下载模式，故 P4 侧不要在 CP 复位时把 D0 驱低（除非就是要进下载模式烧录）。
4. 调试/首烧用测试点：`EN`、`GPIO0`、`TXD0`、`RXD0`。

### 2.2 被内置 Flash 占用 / 剩余可用引脚

| 项目 | 引脚 |
| --- | --- |
| **被内置 4 MB Flash 占用，原理图不要外接** | GPIO6 / GPIO7 / GPIO8 / GPIO11 / GPIO16 / GPIO17（GPIO9、GPIO10 同属 SPI0 总线，一并避开） |
| 剩余可用 | GPIO0、GPIO1(TXD0)、GPIO3(RXD0)、GPIO5、GPIO18、GPIO19、GPIO21、GPIO22、GPIO23、GPIO25、GPIO26、GPIO27、GPIO32、GPIO33 + 仅输入的 GPIO34–GPIO39 |

> 另：GPIO0 是 BOOT strapping（复位必须为高），预留 10 kΩ 上拉、**禁止加大电容**；若挂 BOOT 按键需强下拉（10 kΩ 到 GND）。GPIO5 与 MTDO(GPIO15) 共同选 SDIO Slave 时序，默认值即推荐值。

---

## 3. ESP32-P4 不可用 / 保留引脚（分配新功能前必读）

| 类别 | 引脚 | 说明 |
| --- | --- | --- |
| Flash / PSRAM | `FLASH_CS/Q/WP/HOLD/CK/D`、`VDDO_FLASH`、`VDDO_PSRAM` | 独立焊盘，**不是 GPIO** |
| MIPI | `DSI_*`、`CSI_*`、`DSI_REXT`、`CSI_REXT` | 专用 PHY 焊盘，不占 GPIO |
| USB | `USB_DM`、`USB_DP`、`VDD_USBPHY` | USB 2.0 OTG 专用 PHY |
| 时钟 / 复位 | `XTAL_P/N`、`CHIP_PU` | `CHIP_PU` 不可悬空 |
| **USB Serial/JTAG（必须保留）** | **GPIO24、GPIO25**（备用通道 GPIO26、GPIO27） | P4 的调试 / 下载 / 串口控制台默认走这里；改作 GPIO 会**失去 USB-JTAG 调试能力** |
| **Strapping（禁止占用）** | **GPIO34、GPIO36、GPIO37、GPIO38** | GPIO35–38 决定启动模式，GPIO34 决定 JTAG 来源；GPIO37/38 同时是 UART0。**例外：GPIO35 已接 BOOT/音量键**（见 §1.6），复位时必须为高 |
| **⚠ v3.x 疑似 strapping** | **GPIO32、GPIO33** | IDF 的 `hw_ver3` 寄存器定义中这两个脚被标为 `Diag Group Sel1 / Sel0`（`hw_ver1` 无此标注），含义未明。本项目**按 strapping 处理、不使用**；公开数据手册只列 GPIO34–38，需以 **v3.x 版数据手册**最终确认 |
| PSRAM | — | 封装内 OPI/HPI PSRAM **引脚未引出**，故 **GPIO28–GPIO38（SPI2 复用组）不被它占用**，可作普通 IO |
| JTAG 引脚 | `MTCK=GPIO2`、`MTDI=GPIO3`、`MTMS=GPIO4`、`MTDO=GPIO5` | 默认不启用（JTAG 走 USB-Serial/JTAG）；其中 3/4（控制 I2C）、5（触摸 RST）已占用 |
| SDIO Slot0 | GPIO39–GPIO48 | 固定给存储，不再分配他用 |
| 电源 / 模拟 | `VDD_*`、`VDDO_*`、`FB_DCDC`、`EN_DCDC`、`VDD_LDO` | 按硬件设计指南处理 |

**已被占用**：GPIO3–GPIO12（I2C×2、触摸 RST/INT、CARD_DET、三个从机中断）、GPIO14–GPIO19（协处理器链路）、GPIO20（音量键）、GPIO21（DAC INT）、GPIO22（环境光 ADC）、GPIO28–GPIO31（I2S）、GPIO35（BOOT/音量键）、GPIO39–GPIO48（存储）、GPIO54（协处理器复位）

**剩余可自由分配**：**GPIO13、GPIO23、GPIO49–GPIO53**（7 个，GPIO32/33 按 strapping 处理、不用）；另有 GPIO0–GPIO2 可作普通 IO，但属调试/下载相关，分配前需评估。⚠ 其中**只有 GPIO13 属 LP 域**（可作深睡唤醒）。

---

## 4. 变更记录

| 日期 | 版本 | 变更 |
| --- | --- | --- |
| 2026-09-24 | v0.1–v0.5 | 初始建立：协处理器 SDIO 链路（Slot1）、触摸与 DSI、I2C 双总线、microSD（Slot0） |
| 2026-09-24 | v0.6 | 新增 §1.5 I2S → CS43131 引脚（GPIO28–GPIO32，DAC 自带晶振故不设 MCLK）；**全文重构为「只记录引脚分配」**，删除系统架构、软件 Kconfig、电气/生产工艺等章节 |
| 2026-09-24 | v0.7 | §1.3 补充 I2C-1 从机清单与三个中断脚：PMIC_INT=**GPIO10**、RTC_INT=**GPIO11**、IMU_INT=**GPIO12**（均为 LP 域）；§3 占用/可用清单与 §1.6 同步更新 |
| 2026-09-24 | v0.8 | 新增 §1.6 按键：音量键 A=**GPIO35**（兼 BOOT，硬件已接）、音量键 B=**GPIO20**、电源键状态读取=**GPIO21**（从 PMIC PWRON 经串阻引入）；§3 strapping 行标注 GPIO35 例外、占用/可用清单更新（可用剩 9 个）；原 §1.6 待分配顺延为 §1.7 |
| 2026-09-24 | v0.9 | 电源键不再独占引脚：改为经 I2C-1 读 AXP2101 状态寄存器 + IRQ(GPIO10) 获取键状态，§1.6 中 GPIO21 行改为「不占 P4 引脚」；§3 可用清单恢复为 11 个（GPIO21 回收） |
| 2026-09-24 | v0.10 | 新增 §1.7 环境光传感器（ALS-PT19-315C）：ADC 输入取 **GPIO22**（ADC1_CH6，备选 GPIO21/23）；§3 占用/可用清单更新（可用剩 9 个）；原 §1.7 待分配顺延为 §1.8 |
| 2026-09-24 | v0.11 | 确定主控型号为 **ESP32-P4NRW32（芯片版本 v3.2）**；因 IDF `hw_ver3` 把 GPIO32/GPIO33 标为 strapping（`Diag Group Sel`），**DAC INT 由 GPIO32 改为 GPIO21**，GPIO32/33 列入不用；§3 增加「封装内 PSRAM 未引出、GPIO28–38 可用」说明；可用清单剩 7 个 |

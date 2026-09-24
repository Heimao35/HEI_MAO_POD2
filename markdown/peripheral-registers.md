# HEI_MAO_POD2 外设寄存器手册（I2C 器件）

> 本文件记录本项目 I2C 外设芯片的**器件地址与寄存器信息**，供编写驱动时查阅。引脚分配见 [hardware-pinmap.md](hardware-pinmap.md)。

| 版本 | 更新日期 | 覆盖器件 |
| --- | --- | --- |
| v1.0 | 2026-09-24 | CS43131（DAC）、AXP2101（PMIC）、BMI270（IMU）、PCF85063TP（RTC） |

## 器件地址总表（**统一按 7 位地址**）

| 器件 | 7 位地址 | 8 位写/读 | 地址如何决定 | 硬件现状 / 建议 |
| --- | --- | --- | --- | --- |
| CS43131（DAC） | **0x30** | 0x60 / 0x61 | `ADR` 脚外接电阻，POR / 复位时测量阻值并锁存，之后不可改（手册 Table 4-12） | 建议 `ADR` **直连 GND**（=0x30）；备选：4990 Ω↓=0x31、4990 Ω↑=0x32、直连 VL=0x33 |
| AXP2101（PMIC） | **0x34**（待确认） | 0x68 / 0x69 | 固定，无地址脚、无地址配置寄存器 | 手册 6.3 原文写 `default address 0x68/0x69`（8 位形式，反推 7 位 0x34）；**上电后建议先做 I2C 扫描确认** |
| PCF85063TP（RTC） | **0x51** | 0xA2 / 0xA3 | 固定且唯一，无地址选择手段 | 同一总线只能挂 1 片 |
| BMI270（IMU） | **0x68** | 0xD0 / 0xD1 | `SDO` 脚：接 GND→0x68，接 VDDIO→0x69 | 建议 `SDO` **直连 GND**（=0x68）；若扫描发现 AXP2101 实际占用了 0x68，则把 `SDO` 改接 VDDIO 用 0x69 |

> ⚠️ **7 位 / 8 位混淆陷阱**：AXP2101 的 *8 位* 地址（0x68）与 BMI270 的 *7 位* 地址（0x68）数值相同。驱动**全流程只使用 7 位地址**（ESP-IDF 的 `i2c_device_config_t.device_address` 填 7 位），否则会出现"地址看着对、器件就是不应答"这类难查的问题。

**中断脚（P4 侧，详见 [hardware-pinmap.md](hardware-pinmap.md) §1.3）**

CS43131 = **GPIO21** · AXP2101 = **GPIO10** · BMI270 = **GPIO12** · PCF85063TP = **GPIO11**

**总线**：上表四颗全部挂在 **I2C-1**（SCL=GPIO3 / SDA=GPIO4）。I2C-0（SCL=GPIO8 / SDA=GPIO7）为触摸专用，挂 CST820，驱动已在 [components/esp_lcd_touch_cst820/](../components/esp_lcd_touch_cst820/)。

## 阅读约定

- 地址一律 16 进制；寄存器名保留手册原文英文，说明为中文。
- `（待确认）` 表示该点在手册中未能确证，实现前请核对原手册或实测。
- 只列出地址与用途、未逐位展开的寄存器属正常情况，见各节末尾的「手册依据」行。
- 内容均来自**厂商数据手册原文**，不是从其他型号推断的；修改本文件请同步更新上方版本表。

**目录**：1. CS43131（音频 DAC） · 2. AXP2101（PMIC） · 3. BMI270（IMU） · 4. PCF85063TP（RTC）

---
## 1. CS43131（音频 DAC）

| 项 | 值 |
| --- | --- |
| 数据手册 | cs43131.pdf DS1155F2（2022 年 12 月，Cirrus Logic） |
| 7 位地址 | 0x30（默认：ADR 直接接 GND，AD1 AD0 = 00） |
| 8 位地址（写/读） | 0x60 / 0x61（默认） |
| 地址配置 | ADR 引脚在 POR 或 RESET 释放时检测外接电阻并锁存地址末两位（AD1 AD0），此后不再改变，直到下一次硬件复位；配置完成前（tPUD 内）不响应任何 I2C 命令，配置完成后 ADR 引脚内部三态 |
| 总线速率上限 | 1 MHz（Fast Mode Plus；SM 100 kbit/s、FM 400 kbit/s 亦支持）。注意 HP_IN_LP = 1 时最高仅 400 kHz（只支持 SM/FM） |

**7 位地址与 ADR 配置对应关系**（Table 4-12，手册称「I2C Address Configurations」）：

| ADR 连接方式 | 电阻值 | 地址末两位 AD1 AD0 | 7 位地址 | 8 位写 / 读 |
| --- | --- | --- | --- | --- |
| 下拉到 GND（直连） | 0 Ω | 00（默认） | 0x30 | 0x60 / 0x61 |
| 下拉到 GND | 4990 Ω（±5%） | 01 | 0x31 | 0x62 / 0x63 |
| 上拉到 VL | 4990 Ω（±5%） | 10 | 0x32 | 0x64 / 0x65 |
| 上拉到 VL（直连） | 0 Ω | 11 | 0x33 | 0x66 / 0x67 |

- 芯片地址固定前 5 位为 `01100`，后接 AD1 AD0，再跟 R/W 位（手册时序图写作 `Addr = 01100 AD1 AD0 R/W`）。
- 同一总线可挂载数量：地址末两位有 4 种组合，理论上最多 4 片；**手册未明示上限（待确认）**。
- 通信格式：3 字节 MAP（寄存器地址 23:16 / 15:8 / 7:0，即 24 位地址空间）+ 1 字节控制字节。控制字节 bit[2:1] SIZE 固定为 `00`（8 位数据访问），bit[0] INCR = 1 时地址自增，bit[7:3] 保留为 0。
- 通信必须在上电就绪且 RESET 释放后 tPUD 之后才能开始（Table 3-22）。

### 1.1 寄存器地址总表

地址空间按模块划分（Table 6-1）：Global 0x01 0000、PLL 0x03 0000、ASP/XSP 0x04 0000、DSD 0x06 0000、Headphone and PCM 0x08 0000、Interrupt 0x0F 0000。下表中「复位值」列取手册 Table 6-1 ~ 6-6 与 Section 7 的默认值。

| 地址 | 名称 | 功能 | 复位值 |
| --- | --- | --- | --- |
| 0x01 0000 | Device ID A and B (R/O) | 器件 ID：DEVIDA=4 / DEVIDB=3 | 0x43 |
| 0x01 0001 | Device ID C and D (R/O) | 器件 ID：DEVIDC=1 / DEVIDD=3 | 0x13 |
| 0x01 0002 | Device ID E (R/O) | 器件 ID：DEVIDE=1 | 0x10 |
| 0x01 0004 | Revision ID (R/O) | 芯片版本：AREVID[7:4] / MTLREVID[3:0] | x |
| 0x01 0005 | Subrevision ID (R/O) | 子版本号 SUBREVID[7:0] | 0x0x |
| 0x01 0006 | System Clocking Control | 内部 MCLK 频率与来源选择 | 0x02 |
| 0x01 0007–0x01 000A | Reserved | 保留 | 0x00 |
| 0x01 000B | Serial Port Sample Rate | ASP 采样率（32 k~384 kHz） | 0x01 |
| 0x01 000C | Serial Port Sample Bit Size | ASP/XSP 采样字长 | 0x05 |
| 0x01 000D | Pad Interface Configuration | Master 模式串口时钟输出使能/三态 | 0x03 |
| 0x01 000E–0x01 FFFF | Reserved | 保留（其中 0x01 0010 见 1.3 待确认项） | 0x00 |
| 0x02 0000 | Power Down Control | 各模块电源域上下电控制 | 0xFE |
| 0x02 0001–0x02 0051 | Reserved | 保留 | 0x00 |
| 0x02 0052 | Crystal Setting | 晶振偏置电流强度 | 0x04 |
| 0x02 0053–0x03 0000 | Reserved | 保留 | 0x00 |
| 0x03 0001 | PLL Setting 1 | PLL_START 启动位 | 0x00 |
| 0x03 0002 | PLL Setting 2 | PLL 反馈分频小数部分 LSB | 0x00 |
| 0x03 0003 | PLL Setting 3 | PLL 反馈分频小数部分中间字节 | 0x00 |
| 0x03 0004 | PLL Setting 4 | PLL 反馈分频小数部分 MSB | 0x00 |
| 0x03 0005 | PLL Setting 5 | PLL 反馈分频整数部分 | 0x40 |
| 0x03 0006–0x03 0007 | Reserved | 保留 | 0x00 |
| 0x03 0008 | PLL Setting 6 | PLL 最终输出分频值 | 0x10 |
| 0x03 0009 | Reserved | 保留 | 0x00 |
| 0x03 000A | PLL Setting 7 | PLL VCO 校准比例 | 0x80 |
| 0x03 000B–0x03 001A | Reserved | 保留 | 0x00 |
| 0x03 001B | PLL Setting 8 | PLL_MODE（500/512 因子旁路） | 0x03 |
| 0x03 001C–0x04 0001 | Reserved | 保留 | 0x00 |
| 0x04 0002 | PLL Setting 9 | PLL 参考时钟预分频 | 0x02 |
| 0x04 0003 | Reserved | 保留 | 0x00 |
| 0x04 0004 | CLKOUT Control | CLKOUT 输出源与分频 | 0x00 |
| 0x04 0005–0x04 000F | Reserved | 保留 | 0x00 |
| 0x04 0010 | ASP Numerator 1 | ASP SCLK 小数分频分子 LSB | 0x01 |
| 0x04 0011 | ASP Numerator 2 | ASP SCLK 小数分频分子 MSB | 0x00 |
| 0x04 0012 | ASP Denominator 1 | ASP SCLK 小数分频分母 LSB | 0x08 |
| 0x04 0013 | ASP Denominator 2 | ASP SCLK 小数分频分母 MSB | 0x00 |
| 0x04 0014 | ASP LRCK High Time 1 | ASP LRCK 高电平时长 LSB | 0x1F |
| 0x04 0015 | ASP LRCK High Time 2 | ASP LRCK 高电平时长 MSB | 0x00 |
| 0x04 0016 | ASP LRCK Period 1 | ASP LRCK 周期 LSB | 0x3F |
| 0x04 0017 | ASP LRCK Period 2 | ASP LRCK 周期 MSB | 0x00 |
| 0x04 0018 | ASP Clock Configuration | ASP 主从 + SCLK/LRCK 极性 | 0x0C |
| 0x04 0019 | ASP Frame Configuration | 帧起始相位、50/50 占空比、帧起始延迟 | 0x0A |
| 0x04 001A–0x04 001F | Reserved | 保留 | 0x00 |
| 0x04 0020 | XSP Numerator 1 | XSP SCLK 小数分频分子 LSB | 0x01 |
| 0x04 0021 | XSP Numerator 2 | XSP SCLK 小数分频分子 MSB | 0x00 |
| 0x04 0022 | XSP Denominator 1 | XSP SCLK 小数分频分母 LSB | 0x02 |
| 0x04 0023 | XSP Denominator 2 | XSP SCLK 小数分频分母 MSB | 0x00 |
| 0x04 0024 | XSP LRCK High Time 1 | XSP LRCK 高电平时长 LSB | 0x1F |
| 0x04 0025 | XSP LRCK High Time 2 | XSP LRCK 高电平时长 MSB | 0x00 |
| 0x04 0026 | XSP LRCK Period 1 | XSP LRCK 周期 LSB | 0x3F |
| 0x04 0027 | XSP LRCK Period 2 | XSP LRCK 周期 MSB | 0x00 |
| 0x04 0028 | XSP Clock Configuration | XSP 主从 + SCLK/LRCK 极性 | 0x0C |
| 0x04 0029 | XSP Frame Configuration | XSP 帧起始相位、50/50 占空比、帧起始延迟 | 0x0A |
| 0x04 002A–0x04 FFFF | Reserved | 保留 | 0x00 |
| 0x05 0000 | ASP Channel 1 Location | TDM 帧内 ASP 通道 1 起始 SCLK 位置 | 0x00 |
| 0x05 0001 | ASP Channel 2 Location | TDM 帧内 ASP 通道 2 起始 SCLK 位置 | 0x00 |
| 0x05 0002–0x05 0009 | Reserved | 保留 | 0x00 |
| 0x05 000A | ASP Channel 1 Size and Enable | ASP 通道 1 有效相位/使能/位宽 | 0x06 |
| 0x05 000B | ASP Channel 2 Size and Enable | ASP 通道 2 有效相位/使能/位宽 | 0x0E |
| 0x05 000C–0x05 FFFF | Reserved | 保留 | 0x00 |
| 0x06 0000 | XSP Channel 1 Location | TDM 帧内 XSP 通道 1 起始 SCLK 位置 | 0x00 |
| 0x06 0001 | XSP Channel 2 Location | TDM 帧内 XSP 通道 2 起始 SCLK 位置 | 0x00 |
| 0x06 0002–0x06 0009 | Reserved | 保留 | 0x00 |
| 0x06 000A | XSP Channel 1 Size and Enable | XSP 通道 1 有效相位/使能/位宽 | 0x06 |
| 0x06 000B | XSP Channel 2 Size and Enable | XSP 通道 2 有效相位/使能/位宽 | 0x0E |
| 0x06 000C–0x06 FFFF | Reserved | 保留 | 0x00 |
| 0x07 0000 | DSD Volume B | DSD 通道 B 数字音量（0.5 dB 步进） | 0x78（−60 dB） |
| 0x07 0001 | DSD Volume A | DSD 通道 A 数字音量 | 0x78（−60 dB） |
| 0x07 0002 | DSD Processor Path Signal Control 1 | DSD 软斜坡/自动静音/通道静音 | 0xA8 |
| 0x07 0003 | DSD Interface Configuration | DSD 主从、相位调制模式 | 0x00 |
| 0x07 0004 | DSD Processor Path Signal Control 2 | DSD 源选择、使能、速率、检测 | 0x02 |
| 0x07 0005 | DSD and PCM Mixing Control | PCM 与 DSD 混音使能 | 0x00 |
| 0x07 0006 | DSD Processor Path Signal Control 3 | DSD 电平匹配、HPF、反相/交换/复制 | 0x40 |
| 0x07 0007–0x07 FFFF | Reserved | 保留 | 0x00 |
| 0x08 0000 | HP Output Control 1 | 输出满量程、HPIN 开关/低功耗、钳位 | 0x3C |
| 0x09 0000 | PCM Filter Option | PCM 数字滤波器选择、HPF、去加重、WBF | 0x02 |
| 0x09 0001 | PCM Volume B | PCM 通道 B 数字音量（0.5 dB 步进） | 0x78（−60 dB） |
| 0x09 0002 | PCM Volume A | PCM 通道 A 数字音量 | 0x78（−60 dB） |
| 0x09 0003 | PCM Path Signal Control 1 | 软斜坡/过零/自动静音/通道静音 | 0xA8 |
| 0x09 0004 | PCM Path Signal Control 2 | 通道反相/交换/复制 | 0x00 |
| 0x09 000A | Programmable Filter Control 1 | 可编程滤波器 SOS1/SOS2/FOS/SOS3 使能 | 0x00 |
| 0x09 000B | Programmable Filter Control 2 | 控制端口写入的滤波器系数生效使能 | 0x00 |
| 0x09 000C | Programmable Filter Coefficients | 可编程滤波器系数（Q1.17） | 见 Table 6-5 |
| 0x09 000D | Programmable Filter Coefficients | 同上（SOS1_COEFF_B0_LSBYTE） | 0x00 |
| 0x09 000E | Programmable Filter Coefficients | 同上（SOS1_COEFF_B0_MSBYTE） | 0x00 |
| 0x09 000F | Programmable Filter Coefficients | 同上（SOS1_COEFF_B0_SIGN） | 0x00 |
| 0x09 0010 | Programmable Filter Coefficients | 同上（SOS1_COEFF_B1_LSBYTE） | 0x00 |
| 0x09 0011 | Programmable Filter Coefficients | 同上（SOS1_COEFF_B1_MSBYTE） | 0x00 |
| 0x09 0012 | Programmable Filter Coefficients | 同上（SOS1_COEFF_B1_SIGN） | 0x00 |
| 0x09 0013 | Programmable Filter Coefficients | 同上（SOS1_COEFF_B2_LSBYTE） | 0x00 |
| 0x09 0014 | Programmable Filter Coefficients | 同上（SOS1_COEFF_B2_MSBYTE） | 0x00 |
| 0x09 0015 | Programmable Filter Coefficients | 同上（SOS1_COEFF_B2_SIGN） | 0x00 |
| 0x09 0016 | Programmable Filter Coefficients | 同上（SOS1_COEFF_A1_LSBYTE） | 0x00 |
| 0x09 0017 | Programmable Filter Coefficients | 同上（SOS1_COEFF_A1_MSBYTE） | 0x00 |
| 0x09 0018 | Programmable Filter Coefficients | 同上（SOS1_COEFF_A1_SIGN） | 0x00 |
| 0x09 0019 | Programmable Filter Coefficients | 同上（SOS1_COEFF_A2_LSBYTE） | 0x00 |
| 0x09 001A | Programmable Filter Coefficients | 同上（SOS1_COEFF_A2_MSBYTE） | 0x00 |
| 0x09 001B | Programmable Filter Coefficients | 同上（SOS1_COEFF_A2_SIGN） | 0x00 |
| 0x09 001C | Programmable Filter Coefficients | 同上（SOS2_COEFF_B0_LSBYTE） | 0x00 |
| 0x09 001D | Programmable Filter Coefficients | 同上（SOS2_COEFF_B0_MSBYTE） | 0x01 |
| 0x09 001E | Programmable Filter Coefficients | 同上（SOS2_COEFF_B0_SIGN） | 0x00 |
| 0x09 001F | Programmable Filter Coefficients | 同上（SOS2_COEFF_B1_LSBYTE） | 0x00 |
| 0x09 0020 | Programmable Filter Coefficients | 同上（SOS2_COEFF_B1_MSBYTE） | 0x00 |
| 0x09 0021 | Programmable Filter Coefficients | 同上（SOS2_COEFF_B1_SIGN） | 0x00 |
| 0x09 0022 | Programmable Filter Coefficients | 同上（SOS2_COEFF_B2_LSBYTE） | 0x00 |
| 0x09 0023 | Programmable Filter Coefficients | 同上（SOS2_COEFF_B2_MSBYTE） | 0x00 |
| 0x09 0024 | Programmable Filter Coefficients | 同上（SOS2_COEFF_B2_SIGN） | 0x00 |
| 0x09 0025 | Programmable Filter Coefficients | 同上（SOS2_COEFF_A1_LSBYTE） | 0x00 |
| 0x09 0026 | Programmable Filter Coefficients | 同上（SOS2_COEFF_A1_MSBYTE） | 0x00 |
| 0x09 0027 | Programmable Filter Coefficients | 同上（SOS2_COEFF_A1_SIGN） | 0x00 |
| 0x09 0028 | Programmable Filter Coefficients | 同上（SOS2_COEFF_A2_LSBYTE） | 0x00 |
| 0x09 0029 | Programmable Filter Coefficients | 同上（SOS2_COEFF_A2_MSBYTE） | 0x00 |
| 0x09 002A | Programmable Filter Coefficients | 同上（SOS2_COEFF_A2_SIGN） | 0x00 |
| 0x09 002B | Programmable Filter Coefficients | 同上（SOS3_COEFF_B0_LSBYTE） | 0x00 |
| 0x09 002C | Programmable Filter Coefficients | 同上（SOS3_COEFF_B0_MSBYTE） | 0x01 |
| 0x09 002D | Programmable Filter Coefficients | 同上（SOS3_COEFF_B0_SIGN） | 0x00 |
| 0x09 002E | Programmable Filter Coefficients | 同上（SOS3_COEFF_B1_LSBYTE） | 0x00 |
| 0x09 002F | Programmable Filter Coefficients | 同上（SOS3_COEFF_B1_MSBYTE） | 0x00 |
| 0x09 0030 | Programmable Filter Coefficients | 同上（SOS3_COEFF_B1_SIGN） | 0x00 |
| 0x09 0031 | Programmable Filter Coefficients | 同上（SOS3_COEFF_B2_LSBYTE） | 0x00 |
| 0x09 0032 | Programmable Filter Coefficients | 同上（SOS3_COEFF_B2_MSBYTE） | 0x00 |
| 0x09 0033 | Programmable Filter Coefficients | 同上（SOS3_COEFF_B2_SIGN） | 0x00 |
| 0x09 0034 | Programmable Filter Coefficients | 同上（SOS3_COEFF_A1_LSBYTE） | 0x00 |
| 0x09 0035 | Programmable Filter Coefficients | 同上（SOS3_COEFF_A1_MSBYTE） | 0x00 |
| 0x09 0036 | Programmable Filter Coefficients | 同上（SOS3_COEFF_A1_SIGN） | 0x00 |
| 0x09 0037 | Programmable Filter Coefficients | 同上（SOS3_COEFF_A2_LSBYTE） | 0x00 |
| 0x09 0038 | Programmable Filter Coefficients | 同上（SOS3_COEFF_A2_MSBYTE） | 0x00 |
| 0x09 0039 | Programmable Filter Coefficients | 同上（SOS3_COEFF_A2_SIGN） | 0x00 |
| 0x09 003A | Programmable Filter Coefficients | 同上（FOS_COEFF_B0_LSBYTE） | 0x00 |
| 0x09 003B | Programmable Filter Coefficients | 同上（FOS_COEFF_B0_MSBYTE） | 0x01 |
| 0x09 003C | Programmable Filter Coefficients | 同上（FOS_COEFF_B0_SIGN） | 0x00 |
| 0x09 003D | Programmable Filter Coefficients | 同上（FOS_COEFF_B1_LSBYTE） | 0x00 |
| 0x09 003E | Programmable Filter Coefficients | 同上（FOS_COEFF_B1_MSBYTE） | 0x00 |
| 0x09 003F | Programmable Filter Coefficients | 同上（FOS_COEFF_B1_SIGN） | 0x00 |
| 0x09 0040 | Programmable Filter Coefficients | 同上（FOS_COEFF_A1_LSBYTE） | 0x00 |
| 0x09 0041 | Programmable Filter Coefficients | 同上（FOS_COEFF_A1_MSBYTE） | 0x00 |
| 0x0B 0000 | Class H Control | 自适应电源、高压模式、外部 VCPFILT | 0x1E |
| 0x0D 0000 | HP Detect | 耳机插拔检测控制与去抖 | 0x04 |
| 0x0D 0001 | HP Status (R/O) | 耳机插拔去抖状态 | 0x00 |
| 0x0D 0002–0x0D FFFF | Reserved | 保留 | 0x00 |
| 0x0E 0000 | HP Load 1 | 负载测量使能、通道选择、AC/DC 触发 | 0x00 |
| 0x0E 0001–0x0E 0002 | Reserved | 保留 | 0x00 |
| 0x0E 0003 | HP Load Measurement 1 | AC 检测测量频率 LSB | 0x00 |
| 0x0E 0004 | HP Load Measurement 2 | AC 检测测量频率 MSB | 0x00 |
| 0x0E 0005–0x0E 000C | Reserved | 保留 | 0x00 |
| 0x0E 000D | HP DC Load Status 0 (R/O) | DC 负载阻抗低字节 | 0x00 |
| 0x0E 000E | HP DC Load Status 1 (R/O) | DC 负载阻抗高字节 | 0x00 |
| 0x0E 000F | Reserved | 保留 | 0x00 |
| 0x0E 0010 | HP AC Load Status 0 (R/O) | AC 负载阻抗低字节 | 0x00 |
| 0x0E 0011 | HP AC Load Status 1 (R/O) | AC 负载阻抗高字节 | 0x00 |
| 0x0E 0012–0x0E 0019 | Reserved | 保留 | 0x00 |
| 0x0E 001A | HP Load Status (R/O) | 负载测量状态机状态与完成标志 | 0x00 |
| 0x0E 001B–0x0E FFFF | Reserved | 保留 | 0x00 |
| 0x0F 0000 | Interrupt Status 1 (R/O) | DAC 溢出、耳机插拔、XTAL、PLL、PDN_DONE | 0x00 |
| 0x0F 0001 | Interrupt Status 2 (R/O) | ASP 溢出/错误/LRCK 早迟/无 LRCK | 0x00 |
| 0x0F 0002 | Interrupt Status 3 (R/O) | XSP 溢出/错误/LRCK 早迟/无 LRCK | 0x00 |
| 0x0F 0003 | Interrupt Status 4 (R/O) | 负载测量相关状态与错误 | 0x00 |
| 0x0F 0004 | Interrupt Status 5 (R/O) | DSD 卡死/非法/静音/速率、DoP 检测 | 0x00 |
| 0x0F 0005–0x0F 000F | Reserved | 保留 | 0x00 |
| 0x0F 0010 | Interrupt Mask 1 | 0x0F 0000 各位的屏蔽 | 0xFF |
| 0x0F 0011 | Interrupt Mask 2 | 0x0F 0001 各位的屏蔽 | 0xF8 |
| 0x0F 0012 | Interrupt Mask 3 | 0x0F 0002 各位的屏蔽 | 0xF8 |
| 0x0F 0013 | Interrupt Mask 4 | 0x0F 0003 各位的屏蔽 | 0xFF |
| 0x0F 0014 | Interrupt Mask 5 | 0x0F 0004 各位的屏蔽 | 0xFF |
| 0x0F 0015–0x0F FFFF | Reserved | 保留 | 0x00 |
| 0x10 0000 | ASP Master Mode Slew Rate Control | SCLK1 压摆率 | 见 1.2（bit[3:2]=10） |
| 0x10 0001 | XSP Master Mode Slew Rate Control | DSDCLK/SCLK2 压摆率 | 见 1.2（bit[3:2]=10） |

> 总表寄存器条数：141 个（其中 0x09 000C–0x09 0041 共 54 个可编程滤波器系数寄存器），另列出 26 段 Reserved 地址范围。

### 1.2 关键寄存器逐位说明

#### 0x01 0000 Device ID A and B — 器件 ID（R/O）

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | DEVIDA | 型号第 1 位数字：4 |
| 3:0 | DEVIDB | 型号第 2 位数字：3 |

复位值：0x43（只读）

#### 0x01 0001 Device ID C and D — 器件 ID（R/O）

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | DEVIDC | 型号第 3 位数字：1 |
| 3:0 | DEVIDD | 型号第 4 位数字：3 |

复位值：0x13（只读）

#### 0x01 0002 Device ID E — 器件 ID（R/O）

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | DEVIDE | 型号第 5 位数字：1 |
| 3:0 | -- | 保留 |

复位值：0x10（只读）

#### 0x01 0004 Revision ID — 芯片版本（R/O）

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | AREVID | Alpha 版本号（与 MTLREVID 共同构成完整版本 ID，如 A0、B2） |
| 3:0 | MTLREVID | Metal 版本号 |

复位值：0x0x（只读，手册为 x）

#### 0x01 0005 Subrevision ID — 子版本（R/O）

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | -- | 保留（默认 0） |
| 6:0 | SUBREVID | CS43131 子版本级别（手册默认值列为 x） |

复位值：0x0x（只读）

#### 复位方式说明（无软复位寄存器）

| 项 | 说明 |
| --- | --- |
| 硬复位 | 拉低 RESET 引脚：所有寄存器与状态机立即恢复默认值，复位期间不能开始任何操作；VP 首次上电后必须至少复位一次 |
| 上电复位（POR） | VD 低于 POR 阈值时所有寄存器与状态机复位；VD 上升越过阈值后释放复位。VD 开启时 VL、VA 必须同时开启 |
| 软复位寄存器 | 无（手册未提供软件复位寄存器） |

#### 0x02 0000 Power Down Control — 电源管理（各电源域上下电）

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | PDN_XSP | XSP 输入通路电源：0 = 上电，1（默认）= 下电 |
| 6 | PDN_ASP | ASP 输入通路电源：0 = 上电，1（默认）= 下电 |
| 5 | PDN_DSDIF | DSD 接口电源：0 = 上电，1（默认）= 下电 |
| 4 | PDN_HP | HPOUTx 下电：0 = 上电（HP 驱动与 DACx 上电），1（默认）= 下电，音频输出会软斜坡静音 |
| 3 | PDN_XTAL | 晶振下电：0 = 上电（XTAL 驱动启动产生 MCLK），1（默认）= 下电 |
| 2 | PDN_PLL | PLL 电源：0 = 上电，1（默认）= 下电 |
| 1 | PDN_CLKOUT | CLKOUT 电源：0 = 上电，1（默认）= 下电（CLKOUT 输出低） |
| 0 | -- | 保留 |

复位值：0xFE

#### 0x08 0000 HP Output Control 1 — HP 输出使能与满量程（含 HP 输出使能）

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | HP_CLAMPA | HPOUTA 接地钳位豁免：0（默认）= PDN_HP 置位且 HP_IN_EN 清零时钳位 HPOUTA；PDN_HP 清零时不钳位。1 = 仅当 PDN_HP 置位时释放钳位 |
| 6 | HP_CLAMPB | HPOUTB 接地钳位豁免，含义同 HP_CLAMPA |
| 5:4 | OUT_FS | 输出满量程：00 = 0.5 V、01 = 1 V、10 = 1.41 V、11（默认）= 1.73 V；只能在 PDN_HP 置位时修改 |
| 3 | HP_IN_EN | HPIN 开关使能：0（默认）= 开关断开，1 = 开关闭合 |
| 2 | HP_IN_LP | HPIN 低功耗模式（仅在 HP_IN_EN = 1 时有效；须在 HP_IN_EN 之后置位、之前清零）。置位时 I2C 最高仅 400 kHz：0（默认）= 非低功耗，1 = 低功耗 |
| 1 | -- | 保留 |
| 0 | +1dB_EN | 输出满量程提升到 2 V（仅在 OUT_FS = 11 且 HV_EN = 1 时有效，其他组合忽略；只能在 PDN_HP 置位时修改）：0（默认）= 由 OUT_FS 决定，1 = 2 V |

复位值：0x3C

#### 0x0B 0000 Class H Control — 功放供电模式

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | -- | 保留 |
| 4:2 | ADPT_PWR | 功放电源自适应方式：000 保留、001 固定 Mode 0（±VP_LDO）、010 固定 Mode 1（±VCP）、011–110 保留、111（默认）随信号自适应 |
| 1 | HV_EN | 高压模式使能：0 = 禁用（VP_LDO = 2.6 V），1（默认）= 使能（VP_LDO = 3.0 V，要求 VP min ≥ 3.3 V，仅适用于 600 Ω 及以上负载） |
| 0 | EXT_VCPFILT | 外部 VCP_FILT± 电压模式：0（默认）= 禁用，1 = 由外部提供 ±3.0 V（须遵循相关上下电时序要求） |

复位值：0x1E

#### 0x01 0006 System Clocking Control — 时钟源与内部 MCLK

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:3 | -- | 保留 |
| 2 | MCLK_INT | 内部 MCLK 期望频率：0 = 24.576 MHz，1（默认）= 22.5792 MHz |
| 1:0 | MCLK_SRC_SEL | 内部 MCLK 来源：00 = Direct MCLK/XTAL 模式、01 = PLL 模式、10（默认）= RCO 模式、11 = 保留 |

复位值：0x02

#### 0x01 000B Serial Port Sample Rate — ASP 采样率

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | -- | 保留 |
| 3:0 | ASP_SPRATE | ASP 采样率（主/从模式都必须配置）：0000 = 32 kHz、0001（默认）= 44.1 kHz、0010 = 48 kHz、0011 = 88.2 kHz、0100 = 96 kHz、0101 = 176.4 kHz、0110 = 192 kHz、0111 = 352.8 kHz、1000 = 384 kHz，1001–1111 保留。若为 384 kHz 且处于 Master 模式，MCLK_INT 必须为 24.576 MHz；其他速率下可为 22.5792 或 24.576 MHz |

复位值：0x01

#### 0x01 000C Serial Port Sample Bit Size — 采样字长

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | -- | 保留 |
| 3:2 | XSP_SPSIZE | XSP 采样字长：00 = 32 bit，01（默认）= 24 bit，10–11 保留 |
| 1:0 | ASP_SPSIZE | ASP 采样字长：00 = 32 bit，01（默认）= 24 bit，10 = 16 bit，11 = 8 bit |

复位值：0x05

#### 0x02 0052 Crystal Setting — 晶振偏置电流

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:3 | -- | 保留 |
| 2:0 | XTAL_IBIAS | 晶振偏置电流强度：010 = 15.0 µA、100（默认）= 12.5 µA、110 = 7.5 µA，其他保留 |

复位值：0x04

#### 0x03 0001 ~ 0x03 001B、0x04 0002 PLL 寄存器组 — PLL 配置

| 地址 | 位 | 字段 | 说明 | 复位值 |
| --- | --- | --- | --- | --- |
| 0x03 0001 | 7:1 | -- | 保留 | 0 |
| 0x03 0001 | 0 | PLL_START | PLL 启动位：0（默认）= 未启动，1 = 启动（配置完成后置位） | 0x00 |
| 0x03 0002 | 7:0 | PLL_DIV_FRAC_0 | 反馈分频小数部分 LSB（0xFF 表示 2⁻¹⁷ + … + 2⁻²⁴） | 0x00 |
| 0x03 0003 | 7:0 | PLL_DIV_FRAC_1 | 反馈分频小数部分中间字节（0xFF 表示 2⁻⁹ + … + 2⁻¹⁶） | 0x00 |
| 0x03 0004 | 7:0 | PLL_DIV_FRAC_2 | 反馈分频小数部分 MSB（0xFF 表示 2⁻¹ + … + 2⁻⁸） | 0x00 |
| 0x03 0005 | 7:0 | PLL_DIV_INT | 反馈分频整数部分 | 0x40 |
| 0x03 0008 | 7:0 | PLL_OUT_DIV | PLL 最终输出分频值 | 0x10 |
| 0x03 000A | 7:0 | PLL_CAL_RATIO | PLL VCO 校准目标值（配置细节见 4.7.2） | 0x80 |
| 0x03 001B | 7:2 | -- | 保留 | 0 |
| 0x03 001B | 1 | PLL_MODE | PLL 频率公式中的 500/512 因子：0 = 不旁路，1（默认）= 旁路 | 0x03 |
| 0x03 001B | 0 | -- | 保留 | 1 |
| 0x04 0002 | 7:2 | -- | 保留 | 0 |
| 0x04 0002 | 1:0 | PLL_REF_PREDIV | PLL 参考分频：00 = ÷1、01 = ÷2、10（默认）= ÷4、11 = ÷8 | 0x02 |

#### 0x04 0004 CLKOUT Control — 系统时钟输出

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | -- | 保留 |
| 4:2 | CLKOUT_DIV | 内部 MCLK 到 CLKOUT 的分频：000（默认）= ÷2、001 = ÷3、010 = ÷4、011 = ÷8，100–111 保留 |
| 1:0 | CLKOUT_SEL | CLKOUT 来源：00（默认）= XTAL/MCLK 通路、01 = PLL 输出通路，10–11 保留 |

复位值：0x00

#### 0x04 0018 ASP Clock Configuration — ASP 主从与极性（音频接口格式）

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | -- | 保留 |
| 4 | ASP_M/SB | ASP 主从：0（默认）= Slave（输入），1 = Master（输出） |
| 3 | ASP_SCPOL_OUT | ASP SCLK 输出驱动极性：0 = 正常，1（默认）= 反相 |
| 2 | ASP_SCPOL_IN | ASP SCLK 输入极性（pad 到逻辑）：0 = 正常，1（默认）= 反相 |
| 1 | ASP_LCPOL_OUT | ASP LRCK 输出驱动极性：0（默认）= 正常，1 = 反相 |
| 0 | ASP_LCPOL_IN | ASP LRCK 输入极性（pad 到逻辑）：0（默认）= 正常，1 = 反相 |

复位值：0x0C

#### 0x04 0019 ASP Frame Configuration — ASP 帧格式（音频接口格式）

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | -- | 保留 |
| 4 | ASP_STP | 帧起始相位：0（默认）= LRCK/FSYNC 由高到低时帧开始，1 = 由低到高时帧开始 |
| 3 | ASP_5050 | LRCK 固定 50/50 占空比：0 = 由 ASP_LCHI/ASP_LCPR 编程占空比，1（默认）= 固定 50% |
| 2:0 | ASP_FSD | 帧起始延迟（单位 ASP_SCLK 周期）：000 = 0、001 = 0.5、010（默认）= 1.0、…、101 = 2.5，110–111 保留 |

复位值：0x0A

#### 0x04 0010 ~ 0x04 0017 ASP 分频与 LRCK 时序

| 地址 | 位 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0x04 0010 | 7:0 | ASP_N_LSB | ASP SCLK 小数分频分子 LSB；与 ASP_M_MSB/LSB 共同决定 SCLK 频率；串口上电期间不可修改；默认 ASP_N = 1 |
| 0x04 0011 | 7:0 | ASP_N_MSB | 分子 MSB |
| 0x04 0012 | 7:0 | ASP_M_LSB | 分母 LSB；默认 ASP_M = 8 |
| 0x04 0013 | 7:0 | ASP_M_MSB | 分母 MSB |
| 0x04 0014 | 7:0 | ASP_LCHI_LSB | LRCK 高电平时长 LSB（单位 ASP_SCLK 周期），必须小于 ASP_LCPR；默认 31 |
| 0x04 0015 | 7:0 | ASP_LCHI_MSB | LRCK 高电平时长 MSB |
| 0x04 0016 | 7:0 | ASP_LCPR_LSB | LRCK 周期 LSB；默认 63 |
| 0x04 0017 | 7:0 | ASP_LCPR_MSB | LRCK 周期 MSB |

复位值：0x04 0010 = 0x01、0x04 0011 = 0x00、0x04 0012 = 0x08、0x04 0013 = 0x00、0x04 0014 = 0x1F、0x04 0015 = 0x00、0x04 0016 = 0x3F、0x04 0017 = 0x00（以上寄存器在串口上电时不可修改）

#### 0x05 0000 / 0x05 0001 ASP Channel 1 / 2 Location — TDM 通道位置

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | ASP_RX_CH1 / ASP_RX_CH2 | 相对 TDM 帧起始的通道起始位置（单位 ASP_SCLK 周期）：0x00 = 从 SCLK 0 开始，…，0xFF = 从 SCLK 255 开始；默认均为 0x00 |

复位值：0x00 / 0x00

#### 0x05 000A ASP Channel 1 Size and Enable — 通道位宽与使能

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | -- | 保留 |
| 3 | ASP_RX_CH1_AP | 通道 1 有效相位（仅 50/50 模式有效）：0（默认）= LRCK 低时输入数据，1 = LRCK 高时输入 |
| 2 | ASP_RX_CH1_EN | 通道 1 使能：0 = 数据不进入内部数据通路，1 = 进入内部数据通路（位描述写「默认 0」，但默认值行/快速参考为 0x06，即 bit2 = 1，**两者矛盾，待确认**） |
| 1:0 | ASP_RX_CH1_RES | 通道 1 位宽：00 = 8 bit、01 = 16 bit、10（默认）= 24 bit、11 = 32 bit |

复位值：0x06

#### 0x05 000B ASP Channel 2 Size and Enable — 通道位宽与使能

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | -- | 保留 |
| 3 | ASP_RX_CH2_AP | 通道 2 有效相位：0 = LRCK 低时输入，1（默认）= LRCK 高时输入 |
| 2 | ASP_RX_CH2_EN | 通道 2 使能：0 = 不进入内部数据通路，1 = 进入内部数据通路（与通道 1 同样存在位描述与默认值行不一致的问题，**待确认**） |
| 1:0 | ASP_RX_CH2_RES | 通道 2 位宽：00 = 8 bit、01 = 16 bit、10（默认）= 24 bit、11 = 32 bit |

复位值：0x0E

> XSP 侧对应寄存器结构相同：0x04 0020–0x04 0029（XSP 分频/LRCK/主从/帧格式，默认 XSP_N = 1、XSP_M = 2、XSP_LCHI = 31、XSP_LCPR = 63、0x04 0028 = 0x0C、0x04 0029 = 0x0A）、0x06 0000/0x06 0001（XSP 通道位置，默认 0x00）、0x06 000A = 0x06、0x06 000B = 0x0E（XSP 通道使能默认 = 1）。

#### 0x09 0000 PCM Filter Option — PCM 滤波器与 HPF

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | FILTER_SLOW_FASTB | 快/慢滤波器选择：0（默认）= 快滤波器，1 = 慢滤波器 |
| 6 | PHCOMP_LOWLATB | 低延迟/相位补偿滤波器选择：0（默认）= 低延迟，1 = 相位补偿 |
| 5 | NOS | 非过采样（NOS）仿真模式：0（默认）= 关闭，1 = 开启（开启后忽略 FILTER_SLOW_FASTB 与 PHCOMP_LOWLATB） |
| 4:3 | -- | 保留 |
| 2 | PCM_WBF_EN | 宽带平坦（WBF）模式使能（仅当 xSP 采样率为 192 kHz 的 PCM 回放使用；必须在 PDN_HP 置位时修改）：0（默认）= 禁止，1 = 使能 |
| 1 | HIGH_PASS | 高通滤波器使能：0 = 禁用，1（默认）= 启用 |
| 0 | DEEMP_ON | 44.1 kHz 去加重：0（默认）= 禁用，1 = 启用 |

复位值：0x02

#### 0x09 0001 / 0x09 0002 PCM Volume B / A — 音量

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | PCM_VOLUME_B / PCM_VOLUME_A | PCM 通道数字音量，0.5 dB 步进：0x00 = 0 dB、0x01 = −0.5 dB、…、0x78（默认）= −60 dB、…、0xFE = −127 dB、0xFF = 数字静音。变化过程受 PCM_SZC 控制；若 PCM_VOL_BEQA = 1，两通道均由 PCM_VOLUME_A 控制 |

复位值：0x78（−60 dB）

#### 0x09 0003 PCM Path Signal Control 1 — 斜坡/静音

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | PCM_RAMP_DOWN | 滤波器模式切换前软斜坡静音：0 = 直接静音，1（默认）= 由 PCM_SZC 控制的静音/取消静音 |
| 6 | PCM_VOL_BEQA | 通道 B 音量跟随通道 A：0（默认）= 两通道独立，1 = 由 PCM_VOLUME_A 控制，忽略 PCM_VOLUME_B |
| 5:4 | PCM_SZC | 软斜坡与过零控制：00 = 立即变化、01 = 过零变化、10（默认）= 软斜坡、11 = 软斜坡 + 过零 |
| 3 | PCM_AMUTE | PCM 自动静音：0 = 禁用，1（默认）= 连续 8192 个静态（+1/0/−1）样本后静音，出现一个非静态样本即解除；各通道独立检测 |
| 2 | PCM_AMUTEBEQA | 自动静音通道跟随：0（默认）= 禁用，1 = 仅当两个通道都满足 AMUTE 条件时才静音 |
| 1 | PCM_MUTE_A | 通道 A 静音：0（默认）= 禁用，1 = 通道输出静音（受 PCM_SZC 控制） |
| 0 | PCM_MUTE_B | 通道 B 静音：0（默认）= 禁用，1 = 通道输出静音（受 PCM_SZC 控制） |

复位值：0xA8

#### 0x09 0004 PCM Path Signal Control 2 — 通道映射

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | -- | 保留 |
| 3 | PCM_INV_A | 通道 A 信号反相：0（默认）= 禁用，1 = 反相 |
| 2 | PCM_INV_B | 通道 B 信号反相：0（默认）= 禁用，1 = 反相 |
| 1 | PCM_SWAP_CHAN | 输入通道 A/B 交换（在 PCM_COPY_CHAN 之前生效）：0（默认）= 禁用，1 = 使能 |
| 0 | PCM_COPY_CHAN | 通道 A 复制到通道 B（在 PCM_SWAP_CHAN 之后生效）：0（默认）= 禁用，1 = 使能 |

复位值：0x00

#### 0x09 000A Programmable Filter Control 1 — 可编程滤波器使能

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | SOS1_CTRL | SOS1 滤波器控制：00（默认）= 禁用、01/10 保留、11 = 使能 |
| 5:4 | SOS2_CTRL | SOS2 滤波器控制：00（默认）= 禁用、01/10 保留、11 = 使能 |
| 3:2 | FOS_CTRL | FOS 滤波器控制：00（默认）= 禁用、01/10 保留、11 = 使能 |
| 1 | SOS3_ON | SOS3 滤波器使能：0（默认）= 禁用，1 = 使能 |
| 0 | -- | 保留 |

复位值：0x00

#### 0x09 000B Programmable Filter Control 2 — 系数生效使能

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | -- | 保留 |
| 3 | SOS1_COEFF_CP | 控制端口 SOS1 系数生效：0（默认）= 不生效，1 = 生效（不使用时必须清零） |
| 2 | SOS2_COEFF_CP | 控制端口 SOS2 系数生效：0（默认）= 不生效，1 = 生效（不使用时必须清零） |
| 1 | FOS_COEFF_CP | 控制端口 FOS 系数生效：0（默认）= 不生效，1 = 生效（不使用时必须清零） |
| 0 | -- | 保留 |

复位值：0x00

#### 0x07 0000 / 0x07 0001 DSD Volume B / A — DSD 音量

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | DSD_VOLUME_B / DSD_VOLUME_A | DSD 通道数字音量，0.5 dB 步进（从 0 dB 起）：0x00 = 0 dB、0x01 = −0.5 dB、…、0x78（默认）= −60 dB、…、0xFE = −127 dB、0xFF = 数字静音；变化过程受 DSD_SZC 控制。置 DSD_VOL_BEQA = 1 时通道 B 由 DSD_VOLUME_A 控制 |

复位值：0x78（−60 dB）

#### 0x07 0002 DSD Processor Path Signal Control 1 — DSD 斜坡/静音

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | DSD_RAMP_UP | 出错恢复后软斜坡取消静音：0 = 立即取消静音，1（默认）= 由 DSD_SZC 控制 |
| 6 | DSD_VOL_BEQA | DSD_VOLUME_B 跟随 DSD_VOLUME_A：0（默认）= 两通道独立，1 = 由 A 控制并忽略 B |
| 5 | DSD_SZC | 软斜坡控制：0 = 立即变化，1（默认）= 软斜坡 |
| 4 | -- | 保留 |
| 3 | DSD_AMUTE | DSD 自动静音：0 = 禁用，1（默认）= 收到 256 个重复 8 bit DSD 静音图样后静音，一个不符合图样的位即解除；各通道独立 |
| 2 | DSD_AMUTE_BEQA | 自动静音通道跟随：0（默认）= 禁用，1 = 仅当两个通道都满足 AMUTE 条件时才静音 |
| 1 | DSD_MUTE_A | 通道 A 静音：0（默认）= 禁用，1 = 静音（受 DSD_SZC 影响） |
| 0 | DSD_MUTE_B | 通道 B 静音：0（默认）= 禁用，1 = 静音（受 DSD_SZC 影响） |

复位值：0xA8

#### 0x07 0006 DSD Processor Path Signal Control 3 — DSD 电平匹配与通道映射

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | DSD_ZERODB | DSD 音量对齐 PCM：0（默认）= SACD +3.1 dB 电平（71% 调制指数）对应 PCM 0 dBFS，1 = SACD 0 dB 参考电平（50% 调制指数）对应 PCM 0 dBFS |
| 6 | DSD_HPF_EN | DSD 处理器高通滤波器：0 = 禁用，1（默认）= 启用 |
| 5 | -- | 保留 |
| 4 | SIGCTL_DSDEQPCM | 由 PCM 设置控制 DSD 信号通路控制位（DSD_RAMP_UP、DSD_VOL_BEQA、DSD_SZC、DSD_AMUTE、DSD_AMUTE_BEQA、DSD_MUTE_A/B、DSD_INV_A/B、DSD_SWAP_CHAN、DSD_COPY_CHAN 均等于对应 PCM_x 位）：0（默认）= 禁用，1 = 启用 |
| 3 | DSD_INV_A | DSD 通道 A 反相：0（默认）= 禁用，1 = 反相 |
| 2 | DSD_INV_B | DSD 通道 B 反相：0（默认）= 禁用，1 = 反相 |
| 1 | DSD_SWAP_CHAN | 输入 A/B 通道交换（先于 DSD_COPY_CHAN 与 DSD_INV_x 生效）：0（默认）= 禁用，1 = 使能 |
| 0 | DSD_COPY_CHAN | 通道 A 复制到 B（在 DSD_SWAP_CHAN 之后、DSD_INV_x 之前生效）：0（默认）= 禁用，1 = 使能 |

复位值：0x40

#### 0x0F 0000 Interrupt Status 1（R/O）— 状态标志

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | DAC_OVFL_INT | DAC 调制器溢出 |
| 6 | HPDETECT_PLUG_INT | 检测到耳机插入事件 |
| 5 | HPDETECT_UNPLUG_INT | 检测到耳机拔出事件 |
| 4 | XTAL_READY_INT | PDN_XTAL 清零后 XTAL 就绪 |
| 3 | XTAL_ERROR_INT | PDN_XTAL 清零后检测到 XTAL 错误 |
| 2 | PLL_READY_INT | PLL_START 置位后 PLL 就绪 |
| 1 | PLL_ERROR_INT | PLL_START 置位后检测到 PLL 错误 |
| 0 | PDN_DONE_INT | PDN_HP 请求的处理已完成 |

复位值：0x00（只读）

#### 0x0F 0004 Interrupt Status 5（R/O）— DSD/DoP 状态

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | DSD_STUCK_INT | 至少一个 DSD 输入通道卡在 0 或 1 |
| 6 | DSD_INVAL_A_INT | 通道 A 输入超过 +3.1 dB SACD 最大峰值 |
| 5 | DSD_INVAL_B_INT | 通道 B 输入超过 +3.1 dB SACD 最大峰值 |
| 4 | DSD_SILENCE_A_INT | 通道 A 含 DSD 静音图样 |
| 3 | DSD_SILENCE_B_INT | 通道 B 含 DSD 静音图样 |
| 2 | DSD_RATE_ERROR_INT | DSD 数据速率错误（与 DSD_SPEED 设置不符；漏掉 DoP 头也会触发） |
| 1 | DOP_MRK_DET_INT | 检测到有效的 DoP 标记序列 |
| 0 | DOP_ON_INT | DoP 解码器已上电 |

复位值：0x00（只读）

#### 0x0F 0010 ~ 0x0F 0014 Interrupt Mask 1–5 — 中断屏蔽

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 各 | <对应状态位名>_INT_MASK | 0 = 不屏蔽（Unmasked，事件置位状态位并拉低 INT），1（默认）= 屏蔽（Masked，状态位仍会置位但 INT 不受影响） |

复位值：0x0F 0010 = 0xFF、0x0F 0011 = 0xF8、0x0F 0012 = 0xF8、0x0F 0013 = 0xFF、0x0F 0014 = 0xFF（复位后所有中断默认被屏蔽）

**中断状态清除方式（重要）**：

- 状态位为粘滞（sticky）、**读清除（read-to-clear）**：一旦置位，须读取该寄存器且对应中断条件已消失时才清除；若条件仍存在，即使读了寄存器、INT 已释放，状态位仍保持置位。
- INT 为开漏输出；只有当所有「未屏蔽且已置位」的状态位都被读取后，INT 才会释放。
- 初始化要求：在使能某个模块后、开始正常工作前，必须先读一遍全部中断状态寄存器（手册示例为读 0x0F 0000 与 0x0F 0001）以清除残留状态位，否则后续解除屏蔽会导致 INT 被立即拉低。

#### 0x10 0000 / 0x10 0001 ASP / XSP Master Mode Slew Rate Control — 主模式压摆率

| 地址 | 位 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0x10 0000 | 7:4 | -- | 保留 |
| 0x10 0000 | 3:2 | SCLK1_SLEW_RATE | SCLK1 压摆率：00 保留、01 适用于时钟 > 12.288 MHz、10（默认）适用于 ≤ 12.288 MHz、11 保留 |
| 0x10 0000 | 1:0 | -- | 保留 |
| 0x10 0001 | 7:4 | -- | 保留 |
| 0x10 0001 | 3:2 | DSDCLK_SCLK2_SLEW_RATE | DSDCLK/SCLK2 压摆率：00 保留、01 适用于时钟 > 12.288 MHz、10（默认）适用于 ≤ 12.288 MHz、11 保留 |
| 0x10 0001 | 1:0 | -- | 保留 |

复位值：手册该行 8 位默认值排版与位域表不一致（印作 `0 0 1 0 1 0 1 0`，但与「7:4、1:0 为保留且默认 0」冲突）；按位描述，默认应为 bit[3:2] = 10（**整字节复位值待确认**）。

#### 0x0D 0000 HP Detect — 耳机插拔检测

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | HPDETECT_CTRL | 检测控制（内部弱电流源上拉在所有模式均使能）：00（默认）= 禁用（检测数字电路下电，不向状态寄存器上报，插拔中断也清零）、01–10 保留、11 = 使能 |
| 5 | HPDETECT_INV | 检测信号反相：0（默认）= 不反相，1 = 反相 |
| 4:3 | HPDETECT_RISE_DBC_TIME | Tip 检测上升沿去抖：00（默认）= 0 ms、01 = 250 ms、10 = 500 ms、11 = 1.0 s |
| 2:1 | HPDETECT_FALL_DBC_TIME | Tip 检测下降沿去抖：00 = 0 ms、01 = 250 ms、10（默认）= 500 ms、11 = 1.0 s |
| 0 | -- | 保留 |

复位值：0x04

#### 0x0D 0001 HP Status（R/O）— 插拔去抖状态

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | -- | 保留 |
| 6 | HPDETECT_PLUG_DBC | 插入去抖状态：0（默认）= 条件不存在，1 = 条件存在（HPDETECT_INV 会反转含义） |
| 5 | HPDETECT_UNPLUG_DBC | 拔出去抖状态：0（默认）= 条件不存在，1 = 条件存在（HPDETECT_INV 会反转含义） |
| 4:0 | -- | 保留 |

复位值：0x00（只读）

#### 0x0E 0000 HP Load 1 — 负载测量控制

| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | HPLOAD_EN | 负载测量使能：0（默认）= 禁用，1 = 使能 |
| 6:5 | -- | 保留 |
| 4 | HPLOAD_CHN_SEL | 测量通道：0（默认）= HPOUTA，1 = HPOUTB |
| 3:2 | -- | 保留 |
| 1 | HPLOAD_AC_START | AC 测量触发：0→1 启动；测量完成后须手动写回 0 才能再次触发（默认 0） |
| 0 | HPLOAD_DC_START | DC 测量触发：0→1 启动；测量完成后须手动写回 0 才能再次触发（默认 0） |

复位值：0x00

> 负载测量其余寄存器：0x0E 0003/0x0E 0004（HPLOAD_MEAS_FREQ_LSB/MSB，AC 测量频率，24.576 MHz 时 5.86 Hz/lsb、22.5792 MHz 时 5.94 Hz/lsb，范围 20 Hz–20 kHz，默认 0x00）、0x0E 000D/0x0E 000E（RL_DC_STAT_0/1，DC 阻抗，高字节 + 低字节[7:3] 为整数部分、[2:0] 权重 0.5/0.25/0.125）、0x0E 0010/0x0E 0011（RL_AC_STAT_0/1，AC 阻抗，格式同上）、0x0E 001A（HP Load Status：7 HPLOAD_DC_ONCE、6 HPLOAD_BUSY、3 HPLOAD_AC_DONE、2 HPLOAD_AC_BUSY、1 HPLOAD_DC_DONE、0 HPLOAD_DC_BUSY）。以上均只列于此，未逐位展开。

#### 驱动初始化必须配置的其它寄存器（手册 Ex. 5-7「Startup to I2S Playback」给出的参考流程）

| 顺序 | 寄存器 | 值 | 说明 |
| --- | --- | --- | --- |
| 1 | RESET 引脚 | 上电后拉低再释放 | 等待 1.5 ms |
| 2 | 0x02 0052 Crystal Setting | 0x04 | XTAL_IBIAS = 12.5 µA（22.5792 MHz 晶振） |
| 3 | 0x0F 0000 | 读 | 清除粘滞中断状态 |
| 4 | 0x0F 0010 Interrupt Mask 1 | 0xE7 | 解除 XTAL_READY / XTAL_ERROR 屏蔽 |
| 5 | 0x02 0000 Power Down Control | 0xF6 | PDN_XTAL = 0，启动 XTAL 驱动（其余保持下电） |
| 6 | 0x01 000B | 0x01 | ASP 采样率 44.1 kHz |
| 7 | 0x01 000C | 0x04 或 0x05 | ASP 采样字长 |
| 8 | 0x04 0010–0x04 0019、0x05 0000/0x05 0001、0x05 000A/0x05 000B | 见手册 Ex. 5-7 第 11–18 步 | ASP 分频、主从、帧格式、通道位置/使能（如 0x40018 = 0x1C 设为 Master） |
| 9 | 0x01 0006 System Clocking Control | 0x04 | MCLK_SRC_SEL = XTAL，MCLK_INT = 22.5792 MHz |
| 10 | 0x01 000D Pad Interface Configuration | 0x02 | ASP_3ST = 0，Master 模式输出串口时钟 |
| 11 | 0x09 0000 PCM Filter Option | 0x02 | 高通滤波器使能、去加重关 |
| 12 | 0x09 0001 / 0x09 0002 PCM Volume | 0x00 | 0 dB |
| 13 | 0x09 0003 / 0x09 0004 | 0xEC / 0x00 | 路径信号控制 |
| 14 | 0x0B 0000 Class H Control | 0x1E | 自适应电源 + 高压模式 + 内部 VCPFILT |
| 15 | 0x08 0000 HP Output Control 1 | 0x30（HP_IN_EN = 0） | 输出满量程 1.73 Vrms，HPIN 关闭 |
| 16 | 0x0D 0000 HP Detect | 0x04 → 0xC4 | 先配置去抖，再置 HPDETECT_CTRL = 11 使能 |
| 17 | 0x0F 0010 / 0x0F 0011 | 0x87 / 0x07 | 解除耳机插拔与 ASP 中断屏蔽 |
| 18 | 0x0F 0000、0x0F 0001 | 读 | 使能中断前再次清除粘滞位 |
| 19 | 0x02 0000 Power Down Control | 按 Ex. 5-5 | 先 PDN_ASP = 0（上电 ASP），再 PDN_HP = 0（上电 HP 功放） |
| 20 | HP 上电时序（Ex. 5-5） | — | ① 写 0x01 0010 = 0x99、0x08 0032 = 0x20（Pop-free 设置）；② 等待；③ 上电 HP 后等 12 ms；④ 写 0x08 0032 = 0x00、0x01 0010 = 0x00 恢复默认 |

### 1.3 未文档化地址与存疑点（供写驱动参考）

| 项 | 说明 |
| --- | --- |
| 0x01 0010、0x08 0032、0x08 0046 | 手册应用章节的 Pop-free 上下电序列（Ex. 5-5 ~ 5-8）中要求写入（如 0x99、0x20、0x21、0x00），但寄存器章节（Section 6/7）把它们归入 Reserved 范围且无任何位定义说明，**功能未知（待确认）** |
| 0x18 0005 等 0x18 00xx 系列 | 手册第 5.10/5.11 节「使用 PCM 反相位 / 通道交换位」的序列中出现的地址（如 0x180005、0x180025、0x180026、0x180035、0x180036），同样未在寄存器章节定义，**功能未知（待确认）** |
| 0x10 0000 / 0x10 0001 复位值 | 手册默认值行排版与位域定义矛盾，整字节复位值**待确认**（位域默认 bit[3:2] = 10） |
| 0x05 000A / 0x05 000B ASP_RX_CHn_EN 默认值 | 位描述写「0（默认）= 数据不进入内部数据通路」，但默认值行与快速参考给的是 0x06 / 0x0E（bit2 = 1），**手册自相矛盾，待确认**（驱动中建议显式写 1 使能） |
| 同一总线可挂载的 CS43131 数量 | 手册未明示；由地址末两位推出最多 4 片，**待确认** |
| 中断状态位写入清除 | 手册明确为读清除（sticky、read-to-clear），无写 1 清除机制 |

> 手册依据：DS1155F2（2022 年 12 月，Cirrus Logic）；寄存器章节 Section 6 Register Quick Reference（第 106–112 页）与 Section 7 Register Descriptions（第 113–139 页），I2C 地址与总线特性见 Section 4.13（第 59–61 页）与 Table 3-22（第 26 页），初始化序列取自 Section 5.13 例 5-7（第 74–76 页）与例 5-5（第 66 页）；未逐位展开的寄存器仅列在上表中。
## 2. AXP2101（PMIC）

| 项 | 值 |
| --- | --- |
| 数据手册 | axp2101.pdf Rev 1.4（Revision History 最后一版，2022-10-13；版权页 2022） |
| 7 位地址 | 0x34（由手册给出的 8 位地址 0x68/0x69 反推；**待确认**） |
| 8 位地址（写/读） | 0x68 / 0x69（手册 6.3 原文：“default address 0x68/0x69”） |
| 地址配置 | **无地址配置手段**：手册未提供地址选择寄存器，也未提供地址选择引脚；上电后 SCK/SDA 由 IO 电源上拉，SCK、SDA 各需 2.2k 上拉。另支持 RSB（Allwinner 平台），地址 0x01D1 或 0x0273（客户定制）。 |
| 总线速率上限 | **手册未给出 TWSI 时钟频率上限**（电气特性 “TWI INTERFACE (SCL, SDA)” 只给 VIH=1.3V、VIL=0.8V@上拉 1.8V、VOL=0.4V@5mA 灌电流）。（待确认） |
| 同一总线可挂数量 | 手册未说明（待确认）；因无地址配置手段，默认只能挂 1 片。 |

> 原文（6.3 Serial Interface Communication, p.18）：“AXP2101 supports TWSI protocol and performs as a TWSI slave device with default address 0x68/0x69.”
> 手册未给出 7 位写法。若 0x68/0x69 是 8 位写/读地址对，则 7 位地址为 0x34；**建议驱动侧先做 I2C 扫描确认 0x34 / 0x68 到底哪个应答**。
> IRQ 引脚：开漏输出（Open-drain interrupt Output），低有效；手册要求经 4.7k 电阻上拉到逻辑电源。中断置位后 IRQ 被拉低，主机扫描状态寄存器后**向状态位写 "1" 清除**（RW1C）。

### 2.1 寄存器地址总表

> 来源：6.13.1 Register List（p.29–31）+ 6.13.2 Register Description（p.31–56）。手册总表用区间（如 0X04-0X07），本表按 6.13.2 逐个地址展开，共 88 个地址，无遗漏、未合并。
> “复位”列含义：`POR`=上电复位值；`System Reset`=系统复位（PWROK 拉低/软复位）值；`EFUSE`=由出厂/定制值决定，无固定默认值；`/`=无复位定义。

| 地址 | 名称 | 功能 | R/W | 复位 |
| --- | --- | --- | --- | --- |
| 0x00 | PMU status1 | VBUS/BATFET/电池在位/热调节/限流等状态 | R | POR 0x00 |
| 0x01 | PMU status2 | 电池电流方向/系统开关机/充电阶段状态 | R | POR 0x00 |
| 0x04 | DATA_BUFFER 0 | 通用 8 位数据缓存 | RW | POR 0x00 |
| 0x05 | DATA_BUFFER 1 | 通用 8 位数据缓存 | RW | POR 0x00 |
| 0x06 | DATA_BUFFER 2 | 通用 8 位数据缓存 | RW | POR 0x00 |
| 0x07 | DATA_BUFFER 3 | 通用 8 位数据缓存 | RW | POR 0x00 |
| 0x10 | PMU common configuration | 公共配置：关断放电、PWROK 重启、PWRON 16s 关机、软复位/软关机 | RW | POR 0x30（bit5/bit4=1） |
| 0x12 | BATFET control | 关机且仅电池供电时 BATFET 使能 | RW | EFUSE |
| 0x13 | Die temperature control | 芯片过温保护档位与温度检测使能 | RW | POR 0x03 |
| 0x14 | Minimum system voltage control | 充电 Vsys 最低电压（DPM） | RW | POR 0x60 |
| 0x15 | Input voltage limit control | 输入电压限制 VINDPM | RW | POR 0x06 |
| 0x16 | Input current limit control | 输入电流限制 IINLIM | RW | POR 0x04 |
| 0x17 | Reset the fuel gauge | 复位电量计 | RW | POR 0x00 |
| 0x18 | Charger, fuel gauge, watchdog on/off control | 电芯充电使能、纽扣电池充电使能、电量计使能、看门狗使能 | RW | 混合（见 2.2） |
| 0x19 | Watchdog control | 看门狗超时动作与超时时间 | RW | POR 0x06 |
| 0x1A | Low Battery warning threshold setting | 低电量告警阈值 level1/level2 | RW | POR 0xA1 |
| 0x20 | PWRON status | 记录本次开机来源 | R | System Reset 0x00 |
| 0x21 | PWROFF status | 记录本次关机来源 | R | POR 0x00 |
| 0x22 | PWROFF_EN | 各关机来源使能、按键关机后动作（关机/重启） | RW | 混合 |
| 0x23 | PWROFF of DCDC OVP/UVP control | DCDC 过压/欠压触发整机关断的使能 | RW | POR 0x3F |
| 0x24 | Vsys voltage for PWROFF threshold setting | 电池欠压关机阈值 VOFF | RW | EFUSE（默认 2.6V） |
| 0x25 | PWROK setting and PWROFF sequence control | PWROK 检查/延时/关机顺序 | RW | 混合 |
| 0x26 | Sleep and wakeup control | SLEEP 使能、唤醒使能、唤醒源与唤醒后电压策略 | RW | 混合 |
| 0x27 | IRQLEVEL/OFFLEVEL/ONLEVEL setting | 电源键“短按中断/关机长按/开机长按”时长档位 | RW | POR 0x14 |
| 0x28 | Fast pwron setting 0 | DCDC1–4 快速开机启动顺序码 | RW | POR 0x00 |
| 0x29 | Fast pwron setting 1 | ALDO1–3、DCDC5 快速开机启动顺序码 | RW | POR 0x00 |
| 0x2A | Fast pwron setting 2 | ALDO4、BLDO1–2、CPUSLDO 快速开机启动顺序码 | RW | POR 0x00 |
| 0x2B | Fast pwron setting 3 | 快速开机/快速唤醒使能 + DLDO1–2 启动顺序码 | RW | POR 0x00 |
| 0x30 | ADC Channel enable control 0 | 各 ADC 通道使能 | RW | POR 0x03 |
| 0x34 | vbat_h | 电池电压 ADC 高 6 位 + ch_dbg_en 调试选择 | RW/RO | POR 0x00 |
| 0x35 | vbat_l | 电池电压 ADC 低 8 位 | RO | POR 0x00 |
| 0x36 | ts_h | TS 电压 ADC 高 6 位 + 关机低频采样使能 + ch_dbg_en[2] | RW/RO | POR 0x40 |
| 0x37 | ts_l | TS 电压 ADC 低 8 位 | RO | POR 0x00 |
| 0x38 | vbus_h | VBUS 电压 ADC 高 6 位 | RO | POR 0x00 |
| 0x39 | vbus_l | VBUS 电压 ADC 低 8 位 | RO | POR 0x00 |
| 0x3A | vsys_h | VSYS 电压 ADC 高 6 位 | RO | POR 0x00 |
| 0x3B | vsys_l | VSYS 电压 ADC 低 8 位 | RO | POR 0x00 |
| 0x3C | tdie_h | 芯片温度 ADC 高 6 位 | RO | POR 0x00 |
| 0x3D | tdie_l | 芯片温度 ADC 低 8 位 | RO | POR 0x00 |
| 0x40 | IRQ Enable 0 | 中断使能：电量计 SOC 告警、电池温度（充/放） | RW | System Reset（bit1:0=0b，其余 1b） |
| 0x41 | IRQ Enable 1 | 中断使能：VBUS/电池插拔、PWRON 短按/长按/边沿 | RW | System Reset（bit1:0=0b，其余 1b） |
| 0x42 | IRQ Enable 2 | 中断使能：看门狗、LDO 过流、充电起停、芯片过温、安全定时器、电池过压 | RW | System Reset（bit7=0b、bit5 保留，其余 1b）→ 0x5F |
| 0x48 | IRQ Status 0 | 中断状态：SOC、电池温度告警（RW1C） | RW1C | POR/System Reset 0x00 |
| 0x49 | IRQ Status 1 | 中断状态：VBUS/电池插拔、PWRON 事件（RW1C） | RW1C | POR/System Reset 0x00 |
| 0x4A | IRQ Status 2 | 中断状态：看门狗/LDO 过流/充电/过温/电池过压（RW1C） | RW1C | POR/System Reset 0x00 |
| 0x50 | TS pin CTRL | TS 引脚功能选择、TS 恒流源开关与电流档位 | RW | POR（bit1:0=10b，其余 EFUSE） |
| 0x52 | TS_HYSL2H setting | TS 由低温返回正常的迟滞 | RW | POR 0x02 |
| 0x53 | TS_HYSH2L setting | TS 由高温返回正常的迟滞 | RW | POR 0x01 |
| 0x54 | VLTF_CHG setting | 充电低温保护阈值（也是 JEITA T1） | RW | POR 0x29 |
| 0x55 | VHTF_CHG setting | 充电高温保护阈值（也是 JEITA T4） | RW | POR 0x58 |
| 0x56 | VLTF_WORK setting | 放电（工作）低温保护阈值 | RW | POR 0x3E |
| 0x57 | VHTF_WORK setting | 放电（工作）高温保护阈值 | RW | POR 0x4C |
| 0x58 | JEITA standard Enable control | JEITA 标准使能 | RW | EFUSE |
| 0x59 | JEITA CV configuration | JEITA Cool/Warm 区充电电流与电压降额 | RW | POR 0x14 |
| 0x5A | JEITA Cool configuration | JEITA Cool 温度点 T2 阈值 | RW | POR 0x37 |
| 0x5B | JEITA Warm configuration | JEITA Warm 温度点 T3 阈值 | RW | POR 0x1E |
| 0x5C | ts_cfg_data_h | TS 电压 ADC 高 6 位（MCU 配置值） | RW | POR 0x02 |
| 0x5D | ts_cfg_data_l | TS 电压 ADC 低 8 位（MCU 配置值） | RO | POR 0x58 |
| 0x61 | Iprechg charger setting | 预充电电流 IPRECHG | RW | POR 0x05 |
| 0x62 | ICC charger setting | 恒流充电电流 ICHG | RW | {EFUSE,0b,EFUSE} |
| 0x63 | Iterm charger setting and control | 充电终止电流 ITERM 及终止使能 | RW | 混合（bit4=1b，bit3:0=0101b） |
| 0x64 | CV charger voltage setting | 充电目标电压 VREG | RW | POR 0x03 |
| 0x65 | Thermal regulation threshold setting | 充电热调节温度阈值 | RW | POR 0x02 |
| 0x67 | Charger timeout setting and control | 充电安全定时器（预充/充满） | RW | POR 0xE6 |
| 0x68 | Battery detection control | 电池在位检测使能 | RW | POR 0x01 |
| 0x69 | CHGLED setting and control | CHGLED 引脚功能与输出控制 | RW | 混合 |
| 0x6A | Button battery charge termination voltage setting | 纽扣（备份）电池充电终止电压 | RW | POR 0x03 |
| 0x80 | DCDCS ON/OFF and DVM control | DCDC1–5 使能、DVM 斜率、强制 CCM | RW | 混合（bit6:5=0b，bit4:0=EFUSE） |
| 0x81 | DCDCS force PWM control | DCDC1–4 PWM/PFM 模式、扩频、UVP 去抖 | RW | 混合（bit1:0=00b，其余 System Reset 0b） |
| 0x82 | DCDC1 voltage setting | DCDC1 输出电压 1.5~3.4V | RW | EFUSE |
| 0x83 | DCDC2 voltage setting | DCDC2 输出电压 + DVM 使能 | RW | EFUSE |
| 0x84 | DCDC3 voltage setting | DCDC3 输出电压 + DVM 使能 | RW | EFUSE |
| 0x85 | DCDC4 voltage setting | DCDC4 输出电压 | RW | EFUSE |
| 0x86 | DCDC5 voltage setting | DCDC5 输出电压 | RW | EFUSE |
| 0x90 | LDOS ON/OFF control 0 | ALDO1–4、BLDO1–2、CPUSLDO、DLDO1 使能 | RW | EFUSE |
| 0x91 | LDOS ON/OFF control 1 | DLDO2 使能 | RW | EFUSE |
| 0x92 | ALDO1 voltage setting | ALDO1 输出电压 0.5~3.5V | RW | EFUSE |
| 0x93 | ALDO2 voltage setting | ALDO2 输出电压 0.5~3.5V | RW | EFUSE |
| 0x94 | ALDO3 voltage setting | ALDO3 输出电压 0.5~3.5V | RW | EFUSE |
| 0x95 | ALDO4 voltage setting | ALDO4 输出电压 0.5~3.5V | RW | EFUSE |
| 0x96 | BLDO1 voltage setting | BLDO1 输出电压 0.5~3.5V | RW | EFUSE |
| 0x97 | BLDO2 voltage setting | BLDO2 输出电压 0.5~3.5V | RW | EFUSE |
| 0x98 | CPUSLDO voltage setting | CPUSLDO 输出电压 0.5~1.4V | RW | EFUSE |
| 0x99 | DLDO1 voltage setting | DLDO1 输出电压 0.5~3.3V | RW | EFUSE |
| 0x9A | DLDO2 voltage setting | DLDO2 输出电压 0.5~1.4V | RW | EFUSE |
| 0xA1 | Battery parameter | 电池参数 ROM（电量计电池模型数据） | RO | POR 0xXX |
| 0xA2 | Fuel gauge control | 电量计 ROM/SRAM 选择、brom 写入使能 | RW | POR 0x00 |
| 0xA4 | Battery percentage data | 电池剩余电量百分比 SOC | RO | POR 0x00 |

### 2.2 关键寄存器逐位说明

> 本节按功能分类，类内按地址升序。位表“复位值”取自手册 Reset/Default 列；`POR`=上电复位，`System Reset`=系统复位，`EFUSE`=出厂/定制值（无固定默认）。手册中未命名的位用“（手册未命名）”标注，不臆测功能。

**A. 器件 ID / 版本 / 软复位 / 工作模式**

- **器件 ID / 版本寄存器：无。** 手册 6.13.1 寄存器表从 0x00 起，0x02/0x03 不存在，全文无 Chip ID / Device ID / Version 寄存器，也无 ID 字段。
- **软复位**：由 0x10 bit1（重启 SoC 系统）实现；电量计另有独立复位 0x17。
- **工作模式（SLEEP / 唤醒）**：0x26。

#### 0x10 PMU common configuration — 公共配置（含软复位/软关机）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | （手册未命名） | RW，默认 `0b` |
| 5 | Internal off-discharge enable for DCDC & LDO & SWITCH | 0: disable 1: enable（默认 `1b`） |
| 4 | （手册未命名） | RW，默认 `1b` |
| 3 | PWROK PIN pull low to Restart the System | 0: disable 1: enable |
| 2 | PWRON 16s to shut the PMIC enable | 长按 PWRON 16s 强制关断 PMIC，0: disable 1: enable |
| 1 | Restart the SoC System, POWOFF/POWON and reset the related registers | RWAC，写 1 重启系统并复位相关寄存器，0: normal 1: reset |
| 0 | Soft PWROFF | RWAC，写 1 软关机，0: Normal 1: PWROFF |

复位值：`0b0011_0000` = 0x30（bit5、bit4 = 1）

#### 0x17 Reset the fuel gauge — 复位电量计
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | — | RO，默认 0 |
| 3 | reset the gauge | RWAC，0: normal 1: reset |
| 2 | reset the gauge besides registers | RW，0: normal 1: reset（复位除寄存器外的电量计数据） |
| 1:0 | — | RO，默认 0 |

复位值：0x00

#### 0x26 Sleep and wakeup control — 睡眠与唤醒（工作模式）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | — | R0，默认 0 |
| 4 | IRQ Pin low to Wakeup | 0: disable 1: enable（IRQ 低电平 >16ms 可唤醒，见 6.5.4.4） |
| 3 | PWROK be low-level enable when Wakeup | 0: disable 1: enable（默认 `1b`） |
| 2 | DCDC/LDO Voltage Select when Wakeup | 0: The Default 1: The voltage before wakeup |
| 1 | Wake Up enable | RWLC，0: disable 1: enable |
| 0 | SLEEP enable | RWLC，写 1 进入 SLEEP；PMU 会先记录 0x80/0x90/0x91 的配置 |

复位值：bit3 = `1b`，bit2 = `0b`，bit1/bit0 System Reset `0b`，7:5 = 0

#### 0x19 Watchdog control — 看门狗（工作模式相关）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | — | RO，默认 0 |
| 5:4 | Watchdog Reset Configuration | 00: IRQ only；01: IRQ and System Reset；10: IRQ, System Reset and Pull down PWROK 1s；11: IRQ, System Reset, DCDC/LDO PWROFF & PWRON |
| 3 | watchdog clear signal | RWAC，0: normal 1: clear |
| 2:0 | TWSI watchdog timer configuration | 000: 1s 001: 2s 010: 4s 011: 8s 100: 16s 101: 32s 110: 64s 111: 128s |

复位值：bit5:4 = `0b`，bit2:0 = `110b`（64s）→ 0x06；使能位见 0x18 bit0（默认 0b，关闭）

**B. 各电源域输出使能与电压设置**

> 手册 6.6 表 6-1 给出各域用途/最大负载：DCDC1 IO/USB 2000mA、DCDC2 CPU 2000mA、DCDC3 VSYS 2000mA、DCDC4 DDR 1500mA、DCDC5 N/A 1000mA、ALDO1–4/BLDO1–2/DC1SW-DLDO1/DC4SW-DLDO2 各 300mA、CPUSLDO 30mA、RTCLDO1/2 30mA。
> **注意：RTCLDO1/2（1.8V/2.5V/3V/3.3V）在 6.13 寄存器章节中没有对应的电压设置寄存器**（仅 6.13.1 未列出），其配置属“定制”（customization）。（待确认）

#### 0x80 DCDCS ON/OFF and DVM control — DCDC 使能与 DVM
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | — | RO，默认 0b |
| 6 | force DCDC work in CCM mode | 0: disable 1: enable |
| 5 | DVM voltage ramp control | 0: 15.625 us/step 1: 31.250 us/step |
| 4 | DCDC5 enable | 0: disable 1: enable |
| 3 | DCDC4 enable | 0: disable 1: enable |
| 2 | DCDC3 enable | 0: disable 1: enable |
| 1 | DCDC2 enable | 0: disable 1: enable |
| 0 | DCDC1 enable | 0: disable 1: enable |

复位值：bit6:5 System Reset `0b`；bit4:0 = `EFUSE`

#### 0x81 DCDCS force PWM control — DCDC PWM/PFM 模式
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | DCDC frequency spread enable | 0: disable 1: enable |
| 6 | DCDC frequency spread range control | 0: 50KHz 1: 100kHz |
| 5 | DCDC4 PWM/PFM Control | 0: Auto Switch 1: Always PWM |
| 4 | DCDC3 PWM/PFM Control | 0: Auto Switch 1: Always PWM |
| 3 | DCDC2 PWM/PFM Control | 0: Auto Switch 1: Always PWM |
| 2 | DCDC1 PWM/PFM Control | 0: Auto Switch 1: Always PWM |
| 1:0 | DCDC UVP debounce time configuration | 00: 60us 01: 120us 10: 180us 11: 240us |

复位值：bit7:2 System Reset `0b`，bit1:0 POR `00b` → 0x00

#### 0x82 DCDC1 voltage setting — DCDC1 输出电压
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | — | RO，默认 0 |
| 4:0 | DCDC1 output voltage configuration | 1.5~3.4V，100mV/step，20 档；00000: 1.5V，00001: 1.6V，……，10011: 3.4V；10100~11111: reserved |

复位值：`EFUSE`（System Reset）

#### 0x83 DCDC2 voltage setting — DCDC2 输出电压
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | DCDC2 DVM enable control | 0: disable 1: enable |
| 6:0 | DCDC2 output voltage configuration | 0.50~1.20V/10mV/step（71 档）+ 1.22~1.54V/20mV/step（17 档）；0000000: 0.50V，0000001: 0.51V，……，1000110: 1.20V，1000111: 1.22V，1001000: 1.24V，……，1010111: 1.54V；1011000~1111111: reserved |

复位值：bit7 System Reset `0b`；bit6:0 = `EFUSE`

#### 0x84 DCDC3 voltage setting — DCDC3 输出电压
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | DCDC3 DVM enable control | 0: disable 1: enable |
| 6:0 | DCDC3 output voltage configuration | 与 0x83 完全相同的分档（0.50~1.20V/10mV、1.22~1.54V/20mV） |

复位值：bit7 System Reset `0b`；bit6:0 = `EFUSE`

#### 0x85 DCDC4 voltage setting — DCDC4 输出电压
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | — | RO，默认 0 |
| 6:0 | DCDC4 output voltage configuration | 0.50~1.20V/10mV/step（71 档）+ 1.22~1.84V/20mV/step（32 档）；0000000: 0.50V，……，1000110: 1.20V，1000111: 1.22V，1001000: 1.24V，……，1100110: 1.84V；1100111~1101000: reserved |

复位值：`EFUSE`（System Reset）

#### 0x86 DCDC5 voltage setting — DCDC5 输出电压
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | — | RO，默认 0 |
| 5 | slow down dcdc5 frequency compensation enable | 0: disable 1: enable |
| 4:0 | DCDC5 output voltage configuration | 1.4~3.7V，100mV/step，24 档；00000: 1.4V，00001: 1.5V，……，10111: 3.7V；11001: 1.2V；11000~11111: reserved |

复位值：bit5 System Reset `0b`；bit4:0 = `EFUSE`

#### 0x90 LDOS ON/OFF control 0 — LDO 使能（0）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | dldo1 enable | 0: disable 1: enable |
| 6 | cpusldo enable | 0: disable 1: enable |
| 5 | bldo2 enable | 0: disable 1: enable |
| 4 | bldo1 enable | 0: disable 1: enable |
| 3 | aldo4 enable | 0: disable 1: enable |
| 2 | aldo3 enable | 0: disable 1: enable |
| 1 | aldo2 enable | 0: disable 1: enable |
| 0 | aldo1 enable | 0: disable 1: enable |

复位值：全部 `EFUSE`（System Reset）

#### 0x91 LDOS ON/OFF control 1 — LDO 使能（1）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:1 | — | RO，默认 0 |
| 0 | dldo2 enable | 0: disable 1: enable |

复位值：bit0 = `EFUSE`

#### 0x92 / 0x93 / 0x94 / 0x95 / 0x96 / 0x97 voltage setting — ALDO1–4 / BLDO1–2 输出电压
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | — | RO，默认 0 |
| 4:0 | <域> output voltage configuration | 0.5~3.5V，100mV/step，31 档；00000: 0.5V，00001: 0.6V，……，11110: 3.5V；11111: reserved |

复位值：`EFUSE`（System Reset）。逐地址：0x92=ALDO1、0x93=ALDO2、0x94=ALDO3、0x95=ALDO4、0x96=BLDO1、0x97=BLDO2。

#### 0x98 CPUSLDO voltage setting — CPUSLDO 输出电压
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | — | RO，默认 0 |
| 4:0 | cpusldo output voltage configuration | 0.50~1.40V，50mV/step，20 档；00000: 0.50V，00001: 0.55V，……，10011: 1.40V；10100~11111: reserved |

复位值：`EFUSE`（System Reset）

#### 0x99 DLDO1 voltage setting — DLDO1 输出电压
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | — | RO，默认 0 |
| 4:0 | dldo1 output voltage configuration | 手册表头写 “0.5~3.4V，100mV/step，29 档”，但档位表最高为 11100: 3.3V；00000: 0.5V，00001: 0.6V，……，11100: 3.3V；11101~11111: reserved。（表头 3.4V 与档位表不一致，**待确认**） |

复位值：`EFUSE`（System Reset）

#### 0x9A DLDO2 voltage setting — DLDO2 输出电压
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | — | RO，默认 0 |
| 4:0 | dldo2 output voltage configuration | 0.50~1.40V，50mV/step，20 档；00000: 0.50V，00001: 0.55V，……，10011: 1.40V；10100~11111: reserved |

复位值：`EFUSE`（System Reset）

#### 0x28 / 0x29 / 0x2A / 0x2B Fast pwron setting 0–3 — 快速开机启动顺序
| 寄存器 | 位 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0x28 | 7:6 / 5:4 / 3:2 / 1:0 | DCDC4 / DCDC3 / DCDC2 / DCDC1 Fast Power On Start Sequence | 00~10: Start Sequence Code；11: disable |
| 0x29 | 7:6 / 5:4 / 3:2 / 1:0 | ALDO3 / ALDO2 / ALDO1 / DCDC5 Fast Power On Start Sequence | 00~10: Start Sequence Code；11: disable |
| 0x2A | 7:6 / 5:4 / 3:2 / 1:0 | CPUSLDO / BLDO2 / BLDO1 / ALDO4 Fast Power On Start Sequence | 00~10: Start Sequence Code；11: disable |
| 0x2B | 7 | Fast Power On Enable | 0: disable 1: enable |
| 0x2B | 6 | Fast Wake up Enable | 0: disable 1: enable |
| 0x2B | 5:4 | — | RO，默认 0b |
| 0x2B | 3:2 / 1:0 | DLDO2 / DLDO1 Fast Power On Start Sequence | 00~10: Start Sequence Code；11: disable |

复位值：0x28/0x29/0x2A/0x2B 全部位 `POR 0b`（0x2B bit5:4 为 RO）→ 均 0x00。
启动顺序码含义（6.6）：共 8 级 0~7，0 = 第一步启动，1 = 第二步，……，7 = 不启动。

**C. 电池充电参数与电池检测**

#### 0x18 Charger, fuel gauge, watchdog on/off control — 充电/电量计/看门狗使能
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | — | RO，默认 0 |
| 3 | Gauge Module enable | 0: disable 1: enable |
| 2 | Button Battery charge enable | 0: disable 1: enable（纽扣/备份电池充电） |
| 1 | **Cell Battery charge enable** | 0: disable 1: enable（**充电总开关**） |
| 0 | Watchdog Module enable | 0: disable 1: enable |

复位值：bit3 = `POR 1b`；bit2/bit1/bit0 = `System Reset`（位表默认分别为 0b / 1b / 0b）→ 0x0A。
驱动注意：充电使能位 reg18H[1] 默认 1，即上电后 VBUS 插入即会充电（见 6.7.2 充电条件）。

#### 0x14 Minimum system voltage control — 充电 Vsys 最低电压（DPM）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | — | RO，默认 0 |
| 6:4 | Linear Charger Vsys voltage dpm | 4.1+N*0.1 V；000: 4.1V 001: 4.2V 010: 4.3V 011: 4.4V 100: 4.5V 101: 4.6V 110: 4.7V 111: 4.8V |
| 3:0 | — | RO，默认 0 |

复位值：bit6:4 = `110b`（4.7V）→ 0x60

#### 0x61 Iprechg charger setting — 预充电电流 IPRECHG
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | — | RO，默认 0 |
| 3:0 | Precharge current limit: 25*N mA | 0000: 0mA 0001: 25mA 0010: 50mA 0011: 75mA 0100: 100mA 0101: 125mA 0110: 150mA 0111: 175mA 1000: 200mA；1001~1111: reserved |

复位值：`0101b` = 125mA → 0x05

#### 0x62 ICC charger setting — 恒流充电电流 ICHG
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | — | RO，默认 0 |
| 4:0 | constant current charge current limit | 25*N mA（N≤8）；200+100*(N-8) mA（N>8）；00000: 0mA 00100: 100mA 00101: 125mA 00110: 150mA 00111: 175mA 01000: 200mA 01001: 300mA 01010: 400mA 01011: 500mA 01100: 600mA 01101: 700mA 01110: 800mA 01111: 900mA 10000: 1000mA；others: reserved |

复位值：`{EFUSE, 0b, EFUSE}`（手册写法）。6.7.1 特性段称默认 300mA（对应 01001b），但位表未直接给出固定默认值 —— **驱动须显式写入充电电流**（待确认）。

#### 0x63 Iterm charger setting and control — 充电终止电流 ITERM
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | — | RO，默认 0b |
| 4 | Charging termination of current enable | 0: disable 1: enable（默认 `1b`，System Reset） |
| 3:0 | Termination current limit: 25*N mA | 0000: 0mA 0001: 25mA 0010: 50mA 0011: 75mA 0100: 100mA 0101: 125mA 0110: 150mA 0111: 175mA 1000: 200mA；1001~1111: reserved |

复位值：bit4 = `1b`，bit3:0 = `0101b` = 125mA → 0x15（bit7:5 = 0）

#### 0x64 CV charger voltage setting — 充电目标电压 VREG
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:3 | — | RO，默认 0 |
| 2:0 | Charge voltage limit | 000: 5.0V 001: 4.0V 010: 4.1V 011: 4.2V 100: 4.35V 101: 4.4V；11X: reserved |

复位值：`011b` = 4.2V → 0x03。
**注意**：`000` 档手册原文写作 **5.0V**（其余档位为 4.0/4.1/4.2/4.35/4.4V 递增），疑为 4.0V 之笔误；Rev 1.4 修订记录中确有 “Update reg 64”（**待确认**）。

#### 0x65 Thermal regulation threshold setting — 充电热调节阈值
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:2 | — | RO，默认 0 |
| 1:0 | Thermal regulation threshold | 00: 60deg 01: 80deg 10: 100deg 11: 120deg |

复位值：`10b` = 100deg → 0x02

#### 0x67 Charger timeout setting and control — 充电安全定时器
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | safety timer1/2 setting during DPM or thermal regulation | 0: safety timer not slowed during input DPM or thermal regulation；1: safety timer slowed during input DPM or thermal regulation（默认 `1b`） |
| 6 | charge done safe timer enable | 0: disable 1: enable（默认 `1b`） |
| 5:4 | charge done safety timer configuration | 00: 5hours 01: 8hours 10: 12hours 11: 20hours |
| 3 | — | RO，默认 0 |
| 2 | pre-charge safe timer enable | 0: disable 1: enable（默认 `1b`） |
| 1:0 | pre-charge safe timer configuration | 00: 40mins 01: 50mins 10: 60mins 11: 70mins |

复位值：0xE6（bit7=1b、bit6=1b、bit5:4=10b、bit3=0、bit2=1b、bit1:0=10b）

#### 0x68 Battery detection control — 电池在位检测
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:1 | — | RO，默认 0 |
| 0 | battery detection enable | 0: disable 1: enable（默认 `1b`；禁用后 PMU 认为电池始终在位） |

复位值：0x01。检测结果见 0x00 bit3（Battery present state）。

#### 0x13 Die temperature control — 芯片过温保护
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:3 | — | RO，默认 0 |
| 2:1 | DIE Over Temperature Protection Level1 Configuration | 00: 115deg 01: 125deg 10: 135deg 11: reserved |
| 0 | DIE Temperature Detect Enable | 0: disable 1: enable |

复位值：bit2:1 = `01b`（125deg），bit0 = `1b` → 0x03。
补充（6.7.4.3）：芯片温度升到 TSHUT(145 ) 时发 IRQ 并关机；降到迟滞阈值 120 以下不会自动开机。

#### 0x6A Button battery charge termination voltage setting — 备份电池充电终止电压
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:3 | — | RO，默认 0 |
| 2:0 | Button Battery charge termination voltage | 2.6~3.3V，100mV/step，8 档；000: 2.6V 001: 2.7V 010: 2.8V 011: 2.9V 100: 3.0V 101: 3.1V 110: 3.2V 111: 3.3V |

复位值：`011b` = 2.9V → 0x03。备份电池充电使能见 0x18 bit2，充电电流固定 100uA（6.12.2）。

#### 0x50 TS pin CTRL — TS（电池温度）引脚控制
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | — | RO，默认 0 |
| 4 | TS PIN function select | 0: TS pin is the battery temperature sensor input and will affect the charger；1: TS pin is the external fixed input and doesn't affect the charger |
| 3:2 | TS current source on/off enable | 00: off；01: on when TS channel of ADC is enabled；10: on only when TS channel is working and off when others channel is working；11: always on |
| 1:0 | current source to TS pin configuration | 00: 20uA 01: 40uA 10: 50uA 11: 60uA |

复位值：bit4/bit3:2 = `EFUSE`；bit1:0 = `10b`（50uA）。
6.7.4.4 建议：25 环境用 10Kohm NTC，恒流源设 50uA；不接 NTC 时可外部 10Kohm 下拉到 GND 或用寄存器设为外部固定输入。

#### 0x52 TS_HYSL2H setting — TS 低温→正常迟滞
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | hysteresis for TS from low go to normal，Thys = N*16mV | 默认 32mV |

复位值：`0x02`（2*16mV = 32mV）

#### 0x53 TS_HYSH2L setting — TS 高温→正常迟滞
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | hysteresis for TS from high go to normal，Thys = N*4mV | 默认 4mV |

复位值：`0x01`（1*4mV = 4mV）

#### 0x54 / 0x55 / 0x56 / 0x57 VLTF/VHTF 充放电温度保护阈值
| 寄存器 | 位 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0x54 | 7:0 | VLTF in voltage of charge configuration，VLTF = N*32 mV | 充电低温阈值，也是 JEITA 的 T1；默认约 0 |
| 0x55 | 7:0 | VHTF in voltage of charge configuration，VHTF = N*2 mV | 充电高温阈值，也是 JEITA 的 T4；默认约 55 |
| 0x56 | 7:0 | VLTF in voltage of work configuration，VLTF = N*32 mV | 放电（工作）低温阈值；默认约 -10 |
| 0x57 | 7:0 | VHTF in voltage of work configuration，VHTF = N*2 mV | 放电（工作）高温阈值；默认约 60 |

复位值：0x54 = `0x29`，0x55 = `0x58`，0x56 = `0x3E`，0x57 = `0x4C`

#### 0x58 JEITA standard Enable control — JEITA 使能
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:1 | — | RO，默认 0 |
| 0 | JEITA Standard Enable | 0: disable 1: enable |

复位值：`EFUSE`

#### 0x59 JEITA CV configuration — JEITA 降额配置
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | — | RO，默认 0 |
| 6 | Current fall of Warm in JEITA Standard | 0: 100% 1: 50% |
| 5 | — | RO，默认 0 |
| 4 | Current fall of Cool in JEITA Standard | 0: 100% 1: 50% |
| 3:2 | Voltage fall of Warm in JEITA Standard | 00: 0mV 01: 4.1/4.2/4.35/4.4V −1 档 10: −2 档 11: reserved |
| 1:0 | Voltage fall of Cool in JEITA Standard | 00: 0mV 01: 4.1/4.2/4.35/4.4V −1 档 10: −2 档 11: reserved |

复位值：bit6 = `0b`，bit4 = `1b`，bit3:2 = `01b`，bit1:0 = `00b` → 0x14

#### 0x5A JEITA Cool configuration — JEITA Cool 温度点 T2
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | Cool Temperature(T2) in voltage of charge configuration，VHTF = N*16 mV | 默认约 10 |

复位值：`0x37`

#### 0x5B JEITA Warm configuration — JEITA Warm 温度点 T3
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | Warm Temperature(T3) in voltage of charge configuration，VHTF = N*8 mV | 默认约 45 |

复位值：`0x1E`

#### 0x5C ts_cfg_data_h — TS 电压 ADC 数据（高 6 位）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | — | RO，默认 0 |
| 5:0 | ts_cfg_data[13:8] | 当 `ts_ch_en = 0b`（TS ADC 通道关闭）时，由 MCU 写入的 TS 电压配置值 |

复位值：`0x02`（bit5:0 = 2h，bit7:6 = 0）

#### 0x5D ts_cfg_data_l — TS 电压 ADC 数据（低 8 位）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | ts_cfg_data[7:0] | 同上，与 0x5C 组成 14 位 |

复位值：`0x58`。手册 R/W 列标为 **RO**，但描述称“configured by MCU”，二者矛盾（**待确认**）。

**D. 电量计 / ADC**

- **电池电流**：手册**无电池电流 ADC 通道**（表 6-5 仅 VBAT/VBUS/VSYS/TS/芯片温度 5 个通道），也无电流数据寄存器。仅有 0x01 bit6:5 的“电流方向”状态位。（→ 该类别“电池电流”项：**无**）
- **SOC**：由 E-Gauge 直接给出百分比，见 0xA4。

#### 0x30 ADC Channel enable control 0 — ADC 通道使能
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | — | R0，默认 0 |
| 5 | general purpose ADC channel enable | 0: disable 1: enable |
| 4 | die temperature measure ADC channel enable | 0: disable 1: enable（默认 0b，驱动若要读芯片温度须置 1） |
| 3 | system voltage voltage measure ADC channel enable | 0: disable 1: enable（VSYS） |
| 2 | vbus voltage measure ADC channel enable | 0: disable 1: enable |
| 1 | TS pin measure ADC channel enable | 0: disable 1: enable（默认 `1b`） |
| 0 | battery voltage measure ADC channel enable | 0: disable 1: enable（默认 `1b`） |

复位值：`0x03`

#### 0x34–0x3D ADC data — ADC 数据寄存器
| 寄存器 | 位 | 字段 | R/W | 复位 |
| --- | --- | --- | --- | --- |
| 0x34 vbat_h | 7:6 | ch_dbg_en[1:0]（000 disable；001 vbat 全通道；010 ts；011 vbus；100 vsys；101 tdie；110 gpadc；111 reserved） | RW | POR 0b |
| 0x34 vbat_h | 5:0 | vbat[13:8] | RO | POR 0b |
| 0x35 vbat_l | 7:0 | vbat[7:0] | RO | POR 0b |
| 0x36 ts_h | 7 | ADC in low frequency sample mode when PWROFF and Battery only enable(64s) | RW | POR 1b |
| 0x36 ts_h | 6 | ch_dbg_en_h = ch_dbg_en[2] | RW | POR 0b |
| 0x36 ts_h | 5:0 | ts[13:8] | RO | POR 0b |
| 0x37 ts_l | 7:0 | ts[7:0] | RO | POR 0b |
| 0x38 vbus_h | 7:6 / 5:0 | — / vbus[13:8] | RO | POR 0b |
| 0x39 vbus_l | 7:0 | vbus[7:0] | RO | POR 0b |
| 0x3A vsys_h | 7:6 / 5:0 | — / vsys[13:8] | RO | POR 0b |
| 0x3B vsys_l | 7:0 | vsys[7:0] | RO | POR 0b |
| 0x3C tdie_h | 7:6 / 5:0 | — / tdie[13:8] | RO | POR 0b |
| 0x3D tdie_l | 7:0 | tdie[7:0] | RO | POR 0b |

复位值：见上表逐位（0x34=0x00，0x36=0x40，其余 0x00）。
**读取顺序要求（手册 6.10 注）**：ADC 数据为 14 位，必须先读高 6 位寄存器、再读低 8 位，才能得到完整同一时刻的数据。
**换算（表 6-5）**：VBAT / VBUS / VSYS 通道 0000H→0mV、0001H→1mV、0002H→2mV、……、1FFFH→8.191V；TS 通道 0000H→0mV、0001H→0.5mV、0002H→1mV、……、1FFFH→4.0955V；芯片温度通道 0000H→0mV、0001H→0.1mV、0002H→2mV、……、1FFFH→0.8191V。
（表 6-5 中每档步进与 1FFFH 满量程不能严格自洽，14 位满码为 0x3FFF；驱动请以实测标定为准 —— **待确认**）

#### 0x1A Low Battery warning threshold setting — 低电量告警阈值
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | low battery warning level2 | 5–20%，1%/step；0000: 5%，0001: 6%，……，1111: 20% |
| 3:0 | low battery warning level1 | 0–15%，1%/step；0000: 0%，0001: 1%，……，1111: 15% |

复位值：bit7:4 = `1010b`（15%），bit3:0 = `0001b`（1%）→ 0xA1

#### 0xA1 Battery parameter — 电池参数 ROM
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | Battery parameter ROM | 电量计电池模型参数（出厂 0xXX，手册未给固定值） |

复位值：`0xXX`（手册原样写 xx）。写入方法见 6.11：每次启动先在 0xA2 使能 brom，再写 0xA1。

#### 0xA2 Fuel gauge control — 电量计控制
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | reserved | RO，默认 0b |
| 5 | reserved | RW，默认 0b |
| 4 | ROM or SRAM select | 1: select sram；0: select rom |
| 3:1 | reserved | RO，默认 0b |
| 0 | brom writer control | 1: enable 0: disable |

复位值：0x00

#### 0xA4 Battery percentage data — 电池电量百分比 SOC
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | battery percentage | 剩余电量百分比（0–100%） |

复位值：`0x00`（POR）。R/W 列标为 **RO**（6.13.1 总表标 RW，二者不一致，**待确认**）。使能见 0x18 bit3。

**E. 电源键 PWRON**

> 6.5.4.1：PWRON 引脚（内部 100k 上拉到 VINT）可定制为 PWRON 或 EN 功能，默认 PWRON；按键接在 PWRON 与 GND 之间。芯片可自动识别长按 / 短按 / 下降沿 / 上升沿四种状态。
> 6.5.4.2 开机源：① POK 按住超过 **ONLEVEL**；② VBUS 低→高；③ VBAT 低→高；④ IRQ 低电平 >16ms；⑤ 电池充到 VBAT>3.3V。②③④⑤ 均“可定制”。
> 6.5.4.3 关机源：① POK 按住超过 **OFFLEVEL**（由 0x22 bit1 使能、bit0 决定关机还是重启）；② 写 0x10 bit0=1；③ VSYSGOOD 高→低（VSYS<VOFF 或 VBUS>7V）；④ DCDC 输出比设定低 15%（0x23 bit4:0）；⑤ DCDC 输出远高于设定（0x23 bit5）；⑥ 芯片温度超 warning level2（145，0x22 bit2）。

#### 0x27 IRQLEVEL/OFFLEVEL/ONLEVEL setting — 按键时长档位
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | — | R0，默认 0 |
| 5:4 | IRQLEVEL configuration | 00: 1s 01: 1.5s 10: 2s 11: 2.5s（PWRON 低电平超过此时长发短按中断） |
| 3:2 | OFFLEVEL configuration | 00: 4s 01: 6s 10: 8s 11: 10s（长按超过此时长触发关机） |
| 1:0 | ONLEVEL configuration | 00: 128ms 01: 512ms 10: 1s 11: 2s（长按超过此时长触发开机） |

复位值：bit5:4 = `01b`（1.5s，POR），bit3:2 = `01b`（6s，POR），bit1:0 = `EFUSE` → 0x14（bit1:0 待 EFUSE 定）

#### 0x22 PWROFF_EN — 关机来源使能
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:3 | — | RO，默认 0 |
| 2 | DIE Over-Temperature(LEVEL2) as POWEROFF Source enable | 0: disable 1: enable（默认 `1b`） |
| 1 | PWRON > OFFLEVEL as POWEROFF Source enable | 0: disable 1: enable（按键长按关机使能） |
| 0 | Function Select when btn_pwroff_en=1 and button power-off occur | 0: Power-off 1: Restart |

复位值：bit2 = `POR 1b`；bit1、bit0 = `EFUSE`

#### 0x20 PWRON status — 开机来源记录（只读）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | — | RO，默认 0 |
| 5 | POWERON always high when EN Mode as POWERON Source | 0: no 1: yes |
| 4 | Battery Insert and Good as POWERON Source | 0: no 1: yes |
| 3 | Battery Voltage > 3.3V when Charged as Source | 0: no 1: yes |
| 2 | Vbus Insert and Good as POWERON Source | 0: no 1: yes |
| 1 | IRQ PIN Pull-down as POWERON Source | 0: no 1: yes |
| 0 | POWERON low for on level when POWERON Mode as POWERON Source | 0: no 1: yes |

复位值：System Reset，全部 `0b` → 0x00

#### 0x21 PWROFF status — 关机来源记录（只读）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | Die Over Temperature as POWEROFF Source | 0: no 1: yes |
| 6 | DCDC Over Voltage as POWEROFF Source | 0: no 1: yes |
| 5 | DCDC Under Voltage as POWEROFF Source | 0: no 1: yes |
| 4 | VBUS Over Voltage as POWEROFF Source | 0: no 1: yes |
| 3 | Vsys Under Voltage as POWEROFF Source | 0: no 1: yes |
| 2 | POWERON always low when EN Mode as POWEROFF Source | 0: no 1: yes |
| 1 | Software configuration as POWEROFF Source | 0: no 1: yes |
| 0 | POWERON Pull down for off level when POWERON Mode as POWEROFF Source | 0: no 1: yes |

复位值：POR，全部 `0b` → 0x00

#### 0x23 PWROFF of DCDC OVP/UVP control — DCDC 过压/欠压关断使能
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | — | RO，默认 0 |
| 5 | DCDC 120%(130%) high voltage turn off PMIC function | 0: disable 1: enable |
| 4 | DCDC5 85% low voltage turn off PMIC function | 0: disable 1: enable |
| 3 | DCDC4 85% low voltage turn off PMIC function | 0: disable 1: enable |
| 2 | DCDC3 85% low voltage turn off PMIC function | 0: disable 1: enable |
| 1 | DCDC2 85% low voltage turn off PMIC function | 0: disable 1: enable |
| 0 | DCDC1 85% low voltage turn off PMIC function | 0: disable 1: enable |

复位值：bit5:0 全部 `1b`（POR）→ 0x3F

#### 0x24 Vsys voltage for PWROFF threshold setting — 电池欠压关机阈值 VOFF
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:3 | — | RO，默认 0 |
| 2:0 | Battery Voltage for POWEROFF | 2.6~3.3V，0.1V/step，8 档；000: 2.6V，001: 2.7V，……，111: 3.3V |

复位值：`EFUSE`（6.5.4.3 称默认 VOFF = 2.6V）

#### 0x25 PWROK setting and PWROFF sequence control — PWROK 与关机时序
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | — | R0，默认 0 |
| 4 | Check the PWROK Pin enable after all dcdc/ldo output valid 128ms | 0: disable 1: enable（默认 `1b`） |
| 3 | POWEROFF Delay 4ms after PWROK disable | 0: disable 1: enable（默认 `1b`） |
| 2 | POWEROFF Sequence Control | 0: At the same time 1: the reverse of the Startup |
| 1:0 | Delay of PWROK after all power output good | 00: 8ms 01: 16ms 10: 32ms 11: 64ms |

复位值：bit4 = `1b`、bit3 = `1b`、bit2 = `0b`（POR）；bit1:0 = `EFUSE` → 0x18（bit1:0 待 EFUSE 定）

**F. 中断（IRQ）**

> 6.12.1：PMU 中断控制器监视过压、过流、PWRON 引脚信号、过温等事件。事件发生且对应使能位（0x40/0x41/0x42）为 1 时，对应状态位（0x48/0x49/0x4A）置 1，**IRQ 引脚被拉低**。主机检测到 IRQ 后扫描状态寄存器并**向状态位写 "1" 清除**（RW1C）。
> IRQ 引脚为**开漏输出、低有效**，需外接 4.7k 上拉到逻辑电源；VIH=1.3V、VIL=0.8V（逻辑 IO 特性）。**手册未提及中断极性/开漏可配置位**——极性固定低有效。

#### 0x40 IRQ Enable 0 — 中断使能 0（电量计 SOC 与电池温度）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | SOC drop to Warning Level2 IRQ (socwl2_irq) enable | 0: disable 1: enable |
| 6 | SOC drop to Warning Level1 IRQ (socwl1_irq) enable | 0: disable 1: enable |
| 5 | Gauge Watchdog Timeout IRQ (gwdt_irq) enable | 0: disable 1: enable |
| 4 | Gauge New SOC IRQ (lowsoc_irq) enable | 0: disable 1: enable |
| 3 | Battery Over Temperature in Charge mode IRQ (bcot_irq) enable | 0: disable 1: enable |
| 2 | Battery Under Temperature in Charge mode IRQ (bcut_irq) enable | 0: disable 1: enable |
| 1 | Battery Over Temperature in Work mode IRQ (bwot_irq) enable | 0: disable 1: enable |
| 0 | Battery Under Temperature in Work mode IRQ (bwut_irq) enable | 0: disable 1: enable |

复位值：System Reset，全部 `1b` → 0xFF

#### 0x41 IRQ Enable 1 — 中断使能 1（插拔与电源键）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | VBUS Insert IRQ (vinsert_irq) enable | 0: disable 1: enable（默认 `1b`） |
| 6 | VBUS Remove IRQ (vremove_irq) enable | 0: disable 1: enable（默认 `1b`） |
| 5 | Battery Insert IRQ (binsert_irq) enable | 0: disable 1: enable（默认 `1b`） |
| 4 | Battery Remove IRQ (bremove_irq) enable | 0: disable 1: enable（默认 `1b`） |
| 3 | POWERON Short PRESS IRQ (ponsp_irq_en) enable | 0: disable 1: enable（默认 `1b`） |
| 2 | POWERON Long PRESS IRQ (ponlp_irq) enable | 0: disable 1: enable（默认 `1b`） |
| 1 | POWERON Negative Edge IRQ (ponne_irq_en) enable | 0: disable 1: enable（默认 **`0b`**） |
| 0 | POWERON Positive Edge IRQ (ponpe_irq_en) enable | 0: disable 1: enable（默认 **`0b`**） |

复位值：bit7:2 = `1b`，bit1:0 = `0b`（System Reset）→ 0xFC
（注意：负/正边沿中断默认关闭；长短按中断默认开启 —— 电源键驱动若依赖边沿中断需自行置位）

#### 0x42 IRQ Enable 2 — 中断使能 2（系统与充电）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | Watchdog Expire IRQ (wdexp_irq) enable | 0: disable 1: enable（默认 `0b`） |
| 6 | LDO Over Current IRQ (ldooc_irq) enable | 0: disable 1: enable（默认 `1b`） |
| 5 | Reserved | RO，默认 0 |
| 4 | Battery charge done IRQ (chgdn_irq) enable | 0: disable 1: enable（默认 `1b`） |
| 3 | Charger start IRQ (chgst_irq) enable | 0: disable 1: enable（默认 `1b`） |
| 2 | DIE Over Temperature level1 IRQ (dotl1_irq) enable | 0: disable 1: enable（默认 `1b`） |
| 1 | Charger Safety Timer1/2 expire IRQ (chgte_irq) enable | 0: disable 1: enable（默认 `1b`） |
| 0 | Battery Over Voltage Protection IRQ (bovp_irq) enable | 0: disable 1: enable（默认 `1b`） |

复位值：bit7 = `0b`，bit5 = 0（RO Reserved），bit6、bit4:0 = `1b`（System Reset）→ 0x5F

#### 0x48 IRQ Status 0 — 中断状态 0（RW1C，写 1 清除）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | SOC drop to Warning Level IRQ | 0: no irq 1: irq；当 SOC ≥ Warning Level 或 SOC < shutdown Level 时清除 |
| 6 | SOC drop to Shutdown Level IRQ | 0: no irq 1: irq；当 SOC ≥ Shutdown Level 时清除 |
| 5 | Gauge Watchdog Timeout IRQ | 0: no irq 1: irq |
| 4 | Gauge New SOC IRQ | 0: no irq 1: irq |
| 3 | Battery Over Temperature in Charge mode IRQ | 0: no irq 1: irq；电池温度恢复正常时清除 |
| 2 | Battery Under Temperature in Charge mode IRQ | 0: no irq 1: irq；电池温度恢复正常时清除 |
| 1 | Battery Over Temperature in Work mode IRQ | 0: no irq 1: irq；电池温度恢复正常时清除 |
| 0 | Battery Under Temperature in Work mode IRQ | 0: no irq 1: irq；电池温度恢复正常时清除 |

复位值：全部 `0b`（POR；bit1/bit0 为 System Reset）→ 0x00

#### 0x49 IRQ Status 1 — 中断状态 1（RW1C）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | VBUS Insert IRQ | 0: no irq 1: irq；VBUS Remove 时清除 |
| 6 | VBUS Remove IRQ | 0: no irq 1: irq；VBUS Insert 时清除 |
| 5 | Battery Insert IRQ | 0: no irq 1: irq；Battery Remove 时清除 |
| 4 | Battery Remove IRQ | 0: no irq 1: irq；Battery Insert 时清除 |
| 3 | POWERON Short PRESS IRQ | 0: no irq 1: irq |
| 2 | POWERON Long PRESS IRQ | 0: no irq 1: irq |
| 1 | POWERON Negative Edge IRQ | 0: no irq 1: irq |
| 0 | POWERON Positive Edge IRQ | 0: no irq 1: irq |

复位值：全部 `0b`（bit7:4 POR，bit3:0 System Reset）→ 0x00。
（6.5.3 提到 VBUS 插入时 reg49H[7] 会被置 1）

#### 0x4A IRQ Status 2 — 中断状态 2（RW1C）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | Watchdog Expire IRQ | 0: no irq 1: irq |
| 6 | LDO Over Current IRQ | 0: no irq 1: irq；LDO 电流恢复正常时清除 |
| 5 | Reserved | RO，默认 0 |
| 4 | Battery charge done IRQ | 0: no irq 1: irq；Battery charge start 时清除 |
| 3 | Battery charge start IRQ | 0: no irq 1: irq；Battery charge done 时清除 |
| 2 | DIE Over Temperature level1 IRQ | 0: no irq 1: irq；芯片温度恢复正常时清除 |
| 1 | Charger Safety Timer1/2 expire IRQ | 0: no irq 1: irq |
| 0 | Battery Over Voltage Protection IRQ | 0: no irq 1: irq；电池电压恢复正常时清除 |

复位值：全部 `0b`（bit6 为 System Reset，其余 POR）→ 0x00

**G. VBUS / 充电器插入检测与状态**

#### 0x15 Input voltage limit control — 输入电压限制 VINDPM
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | — | RO |
| 3:0 | VINDPM configuration: 3.88+N*0.08 V | 0000: 3.88V 0001: 3.96V 0010: 4.04V 0011: 4.12V 0100: 4.20V 0101: 4.28V 0110: 4.36V 0111: 4.44V 1000: 4.52V 1001: 4.60V 1010: 4.68V 1011: 4.76V 1100: 4.84V 1101: 4.92V 1110: 5.00V 1111: 5.08V |

复位值：`0110b` = 4.36V → 0x06。VBUS 到达 VINDPM 时充电电流自动下降（6.5.3.2）。

#### 0x16 Input current limit control — 输入电流限制 IINLIM
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:3 | — | RO |
| 2:0 | Input current limit | 000: 100mA 001: 500mA 010: 900mA 011: 1000mA 100: 1500mA 101: 2000mA；110-111: reserved |

复位值：`100b` = 1500mA → 0x04

#### 0x00 bit5 — VBUS good indication
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 5 | VBUS good indication | 0: not good 1: good |

判定条件（6.5.3.1）：① VBUS 电压低于 VACOV；② 拉 IBADBUS（典型 30mA）时 VBUS 仍高于 VVBUSMIN。满足则 reg00H[5] = 1，表示输入源可用、可启动 buck。

**H. 其它驱动初始化应关注的寄存器**

#### 0x12 BATFET control — BATFET 控制
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | — | RO，默认 0 |
| 3 | BATFET enable when POWEROFF and Battery only | 0: BATFET disable 1: BATFET enable |
| 2:0 | — | RO，默认 0 |

复位值：bit3 = `EFUSE`。6.5.2：仅电池且 VBAT > VBAT_DPLZ(2.5V) 时 BATFET 默认关闭，需按 PWRON 或插适配器才能开机。

#### 0x69 CHGLED setting and control — 充电指示灯
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | — | RO，默认 0 |
| 5:4 | CHGLED pin output when the register of chgled_func is set to 10b | 00: Hiz；01: Low/Hiz 25%/75% duty 1Hz；10: Low/Hiz 25%/75% duty 4Hz；11: drive low |
| 3 | — | RO，默认 0 |
| 2:1 | CHGLED pin display function configuration | 00: display with type A function；01: display with type B function；10: output controlled by the register of chgled_out_ctrl；11: reserved |
| 0 | CHGLED pin enable | 0: disable CHGLED pin function 1: enable CHGLED pin function |

复位值：bit5:4 = `00b`（System Reset），bit2:1 = `EFUSE`，bit0 = `POR 1b` → 0x01（bit2:1 待 EFUSE 定）。
CHGLED 为开漏/推挽输出，内部上拉到 LDO，驱动能力 >10mA；**LED 在 CHGLED 为低时点亮**。类型 A/B 的具体状态见手册表 6-4。

#### 0x04–0x07 DATA_BUFFER 0–3 — 通用数据缓存
| 寄存器 | 位 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0x04 / 0x05 / 0x06 / 0x07 | 7:0 | data buffer | RW，POR `00h`，无特定功能，可作断点标志/透传存储 |

复位值：0x00

> 手册依据：AXP2101 Rev 1.4（2022-10-13）；寄存器章节 6.13 Register（6.13.1 Register List p.29–31，6.13.2 Register Description p.31–56）；未逐位展开的寄存器仅列在上表中。

> 补充说明（展开覆盖面）：2.2 已逐位展开 0x00/0x01/0x04–0x07/0x10/0x12/0x13/0x14/0x15/0x16/0x17/0x18/0x19/0x1A/0x20/0x21/0x22/0x23/0x24/0x25/0x26/0x27/0x28–0x2B/0x30/0x34–0x3D/0x40/0x41/0x42/0x48/0x49/0x4A/0x50/0x52–0x5D/0x61/0x62/0x63/0x64/0x65/0x67/0x68/0x69/0x6A/0x80/0x81/0x82/0x83/0x84/0x85/0x86/0x90/0x91/0x92–0x9A/0xA1/0xA2/0xA4，共 88 个地址全覆盖；其中 **0x34–0x3D（ADC 数据）与 0x52–0x5D（TS/JEITA 阈值）只做到字段级（未按位枚举每个保留位）**。手册中确实不存在、写为“无”的项：器件 ID/版本寄存器、电池电流 ADC 通道/寄存器、RTCLDO1/2 电压设置寄存器、TWSI 速率上限。
## 3. BMI270（IMU）

| 项 | 值 |
| --- | --- |
| 数据手册 | bmi270.pdf Rev 1.3 (Nov 2020), Document number BST-BMI270-DS000-05 |
| 器件 | BMI270，6 轴 IMU：16 位三轴加速度计 + 16 位三轴陀螺仪 |
| 7 位地址 | 0x68（默认，SDO 接 GND）/ 0x69（SDO 接 VDDIO） |
| 8 位地址（写/读） | 0xD0 / 0xD1（地址 0x68）；0xD2 / 0xD3（地址 0x69） |
| 地址配置 | 仅支持 7 位地址模式。由 SDO 引脚（PIN1）电平决定 bit0：手册原文"The default I²C address of the device is 0b1101000 (0x68). It is used if the SDO pin is pulled to 'GND'. The alternative address 0b1101001 (0x69) is selected by pulling the SDO pin to 'VDDIO'." |
| 总线速率上限 | I²C 标准模式 100 kHz、快速模式 400 kHz、快速模式+ 1000 kHz（fSCL max 1000 kHz）。手册 §6.5 原文写作 "fast mode (40kHz)"，疑为 400 kHz 笔误；同表 fSCL Max = 1000 kHz（待确认） |
| 引脚连接 | 本设计：SCL=GPIO3、SDA=GPIO4（P4 I2C-1）；INT 脚接 GPIO12（LP 域）。SDO/SA0 由硬件决定地址，驱动需按实际接法选 0x68 或 0x69 |
| 同一总线可挂几个 | 最多 2 个（只有 0x68 / 0x69 两个地址可选）；同一总线上第 3 个 BMI270 无法寻址 |

补充（接口时序，驱动需遵守）：
- 每次寄存器读取含 dummy 字节：I²C 为 0 个、SPI 为 1 个。
- 写访问间隔：正常模式 / fast startup 模式 ≥ 2 µs（tIDLE_wacc_nm）；suspend 模式 / 低功耗模式 ≥ 450 µs（tIDLE_wacc_sum）。
- 新传输开始前的等待：低功耗模式 400 µs，正常模式 1.3 µs。
- 上电或软复位后，寄存器在 450 µs 后才可访问。
- 地址自动递增：除 **FIFO_DATA(0x26)** 与 **INIT_DATA(0x5E)** 外，其余寄存器支持 auto-increment（这两个寄存器"trap the address"）。
- 影子（shadowed）寄存器组：STATUS、DATA_x、SENSORTIME_x、TEMPERATURE_x、SC_OUT_x、FIFO_LENGTH_x、FEATURES——burst 读时同组寄存器内容被锁存以保证一致性。
- Clear-on-read 寄存器：ERR_REG、STATUS.drdy_acc/drdy_gyr/drdy_aux、EVENT、INT_STATUS_0、INT_STATUS_1。

### 3.1 寄存器地址总表

> 下表中"寄存器名"保留手册原文英文。0x01、0x27–0x2E、0x31–0x3F（FEATURES 之外的别名区）、0x50、0x51、0x5A、0x5D、0x60–0x67、0x6F、0x78–0x7B、0x7F 在手册寄存器映射表中标注为 reserved（无寄存器名）。
> 共 **86** 个具名寄存器地址（FEATURES 块按 1 项计；若把 FEATURES 的 16 个字节寄存器拆开则为 101 项）。

| 地址 | 名称 | 功能 |
| --- | --- | --- |
| 0x00 | CHIP_ID | 芯片识别码，固定 0x24 |
| 0x02 | ERR_REG | 传感器错误状态（fatal / internal / fifo / aux） |
| 0x03 | STATUS | 传感器状态标志（data ready、cmd_rdy、aux_busy） |
| 0x04 | DATA_0 | AUX_X 低字节 |
| 0x05 | DATA_1 | AUX_X 高字节 |
| 0x06 | DATA_2 | AUX_Y 低字节 |
| 0x07 | DATA_3 | AUX_Y 高字节 |
| 0x08 | DATA_4 | AUX_Z 低字节 |
| 0x09 | DATA_5 | AUX_Z 高字节 |
| 0x0A | DATA_6 | AUX_R 低字节 |
| 0x0B | DATA_7 | AUX_R 高字节 |
| 0x0C | DATA_8 | ACC_X 低字节（acc_x_7_0） |
| 0x0D | DATA_9 | ACC_X 高字节（acc_x_15_8） |
| 0x0E | DATA_10 | ACC_Y 低字节 |
| 0x0F | DATA_11 | ACC_Y 高字节 |
| 0x10 | DATA_12 | ACC_Z 低字节 |
| 0x11 | DATA_13 | ACC_Z 高字节 |
| 0x12 | DATA_14 | GYR_X 低字节 |
| 0x13 | DATA_15 | GYR_X 高字节 |
| 0x14 | DATA_16 | GYR_Y 低字节 |
| 0x15 | DATA_17 | GYR_Y 高字节 |
| 0x16 | DATA_18 | GYR_Z 低字节 |
| 0x17 | DATA_19 | GYR_Z 高字节 |
| 0x18 | SENSORTIME_0 | 传感器时间 <7:0> |
| 0x19 | SENSORTIME_1 | 传感器时间 <15:8> |
| 0x1A | SENSORTIME_2 | 传感器时间 <23:16>（24 位计数器，分辨率 39.0625 µs） |
| 0x1B | EVENT | 传感器事件标志（por_detected、error_code），clear-on-read |
| 0x1C | INT_STATUS_0 | 高级特性中断状态（sig_motion/step/activity/wrist/no_motion/any_motion），clear-on-read |
| 0x1D | INT_STATUS_1 | 数据类中断状态（ffull/fwm/err/aux_drdy/gyr_drdy/acc_drdy），clear-on-read |
| 0x1E | SC_OUT_0 | 计步值 byte-0（LSB） |
| 0x1F | SC_OUT_1 | 计步值 byte-1 |
| 0x20 | WR_GEST_ACT | 手腕手势与活动识别输出 |
| 0x21 | INTERNAL_STATUS | 内部初始化状态（message 字段，init_ok=0x01） |
| 0x22 | TEMPERATURE_0 | 温度值低字节（分辨率 1/2^9 K/LSB） |
| 0x23 | TEMPERATURE_1 | 温度值高字节 |
| 0x24 | FIFO_LENGTH_0 | FIFO 填充字节数 <7:0> |
| 0x25 | FIFO_LENGTH_1 | FIFO 填充字节数 <13:8> |
| 0x26 | FIFO_DATA | FIFO 数据输出寄存器（地址不自增，需 burst 读） |
| 0x2F | FEAT_PAGE | 特性寄存器分页选择（8 页，每页映射 16 个特性寄存器） |
| 0x30–0x3F | FEATURES[0..15] | 特性配置输入 / 特性结果输出寄存器组（受 FEAT_PAGE 分页控制，共 16 字节） |
| 0x40 | ACC_CONF | 加速度计 ODR / 带宽（acc_bwp）/ 滤波性能模式 |
| 0x41 | ACC_RANGE | 加速度计量程选择（±2/4/8/16 g） |
| 0x42 | GYR_CONF | 陀螺仪 ODR / 带宽（gyr_bwp）/ 噪声与滤波性能模式 |
| 0x43 | GYR_RANGE | 陀螺仪量程选择（±125…±2000 dps）及 OIS 量程 |
| 0x44 | AUX_CONF | 辅助接口 ODR 与 trigger-readout 偏移 |
| 0x45 | FIFO_DOWNS | FIFO 加速度/陀螺仪降采样与滤波数据选择 |
| 0x46 | FIFO_WTM_0 | FIFO 水印阈值 LSB（fifo_water_mark_7_0） |
| 0x47 | FIFO_WTM_1 | FIFO 水印阈值 MSB（fifo_water_mark_12_8） |
| 0x48 | FIFO_CONFIG_0 | FIFO 满时停止写入 / 是否回传 sensortime 帧 |
| 0x49 | FIFO_CONFIG_1 | FIFO 帧内容配置（header/aux/acc/gyr 使能、INT tag） |
| 0x4A | SATURATION | 各轴原始数据饱和标志 |
| 0x4B | AUX_DEV_ID | 辅助接口从机 I²C 地址（i2c_device_addr） |
| 0x4C | AUX_IF_CONF | 辅助接口配置（burst 长度、手动/自动模式） |
| 0x4D | AUX_RD_ADDR | 辅助接口读地址（手动模式下触发读） |
| 0x4E | AUX_WR_ADDR | 辅助接口写地址（手动模式下触发写） |
| 0x4F | AUX_WR_DATA | 辅助接口写数据 |
| 0x52 | ERR_REG_MSK | 选择哪些 ERR_REG 标志可触发 error 中断 |
| 0x53 | INT1_IO_CTRL | INT1 引脚电气配置（lvl/od/output_en/input_en） |
| 0x54 | INT2_IO_CTRL | INT2 引脚电气配置（lvl/od/output_en/input_en） |
| 0x55 | INT_LATCH | 中断锁存/非锁存模式选择 |
| 0x56 | INT1_MAP_FEAT | 高级特性中断映射到 INT1 |
| 0x57 | INT2_MAP_FEAT | 高级特性中断映射到 INT2 |
| 0x58 | INT_MAP_DATA | 数据类中断（full/wm/drdy/err）映射到 INT1/INT2 |
| 0x59 | INIT_CTRL | 启动/完成配置文件加载 |
| 0x5B | INIT_ADDR_0 | 初始化数据基地址 bit3..0（分块写入时递增） |
| 0x5C | INIT_ADDR_1 | 初始化数据基地址 bit11..4（分块写入时递增） |
| 0x5E | INIT_DATA | 初始化数据写入端口（地址不自增） |
| 0x5F | INTERNAL_ERROR | 内部错误标志（int_err_1/2、feat_eng_disabled） |
| 0x68 | AUX_IF_TRIM | 辅助接口 ASDA 上拉配置（NVM backed） |
| 0x69 | GYR_CRT_CONF | 陀螺仪组件重校准（CRT）状态 |
| 0x6A | NVM_CONF | NVM 编程使能 |
| 0x6B | IF_CONF | 串行接口设置（spi3 / spi3_ois / ois_en / aux_en） |
| 0x6C | DRV | 输出 pad 驱动强度（NVM backed） |
| 0x6D | ACC_SELF_TEST | 加速度计自检配置与触发 |
| 0x6E | GYR_SELF_TEST_AXES | 陀螺仪各轴自检结果状态 |
| 0x70 | NV_CONF | NVM 支持配置位（spi_en、I²C 看门狗、acc_off_en） |
| 0x71 | OFFSET_0 | 加速度计 X 轴偏置补偿（off_acc_x） |
| 0x72 | OFFSET_1 | 加速度计 Y 轴偏置补偿（off_acc_y） |
| 0x73 | OFFSET_2 | 加速度计 Z 轴偏置补偿（off_acc_z） |
| 0x74 | OFFSET_3 | 陀螺仪 X 轴偏置补偿（gyr_usr_off_x_7_0） |
| 0x75 | OFFSET_4 | 陀螺仪 Y 轴偏置补偿（gyr_usr_off_y_7_0） |
| 0x76 | OFFSET_5 | 陀螺仪 Z 轴偏置补偿（gyr_usr_off_z_7_0） |
| 0x77 | OFFSET_6 | 陀螺仪偏置高 2 位 + gyr_off_en / gyr_gain_en |
| 0x7C | PWR_CONF | 电源配置（adv_power_save、fifo_self_wake_up、fup_en） |
| 0x7D | PWR_CTRL | 电源控制（acc_en、gyr_en、aux_en、temp_en） |
| 0x7E | CMD | 命令寄存器（softreset、fifo_flush、nvm_prog、g_trigger、usr_gain） |

**另一套独立地址空间：OIS 寄存器映射**（由 OIS 接口/SPI 访问，与主寄存器映射**不是**同一空间，内容也不互为拷贝）：

| 地址 | 名称 | 功能 |
| --- | --- | --- |
| 0x0C–0x17 | OIS_DATA_0..11 | OIS 低延迟数据通路发布的 ACC_X..GYR_Z（布局同主映射 DATA_8..DATA_19） |
| 0x40 | OIS_CTRL_S | 通过 OIS 接口控制 IMU（bit7 acc_en、bit6 gyr_en） |
| 其余 | — | reserved |

### 3.2 关键寄存器逐位说明

#### 0x00 CHIP_ID — 芯片识别码
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | chip_id | 固定值 0x24，用于确认通信正常 |

复位值：0x24（只读）

#### 0x02 ERR_REG — 传感器错误状态
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | fatal_err | 致命错误（Boot / power-system），芯片不在可操作状态。只能由 POR 或软复位清除 |
| 4:1 | internal_err | 内部错误（需联系 Bosch），只读 |
| 6 | fifo_err | FIFO 溢出且为 streaming 模式无法 skip 时报错；**读后清零** |
| 7 | aux_err | 检测到辅助 I²C-Master 错误；**读后清零** |

复位值：0x00（只读）

#### 0x03 STATUS — 传感器状态标志
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 2 | aux_busy | 1 表示辅助接口正在操作中（由 AUX_RD_ADDR / AUX_WR_ADDR 或 FCU 触发） |
| 4 | cmd_rdy | 命令解码器状态：0 = 命令执行中；1 = 可接受新命令（须轮询等待 CMD 完成） |
| 5 | drdy_aux | 辅助传感器数据就绪，读任一 AUX DATA 寄存器后复位 |
| 6 | drdy_gyr | 陀螺仪数据就绪，读任一 GYR DATA 寄存器后复位 |
| 7 | drdy_acc | 加速度计数据就绪，读任一 ACC DATA 寄存器后复位（读 DATA_9 清除） |

复位值：0x10（只读）

#### 0x1B EVENT — 传感器事件标志
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | por_detected | 上电或软复位后为 1，读 STATUS 后变 0 |
| 4:2 | error_code | 持续性错误码：0x00 no_error、0x01 acc_err（ACC_CONF 非法）、0x02 gyr_err（GYR_CONF 非法）、0x03 acc_and_gyr_err |

复位值：0x01（只读，读到 bit0 后清除）

#### 0x1C INT_STATUS_0 — 高级特性中断状态（clear on read）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | sig_motion_out | 显著运动（significant motion）输出 |
| 1 | step_counter_out | 计步水印或步态检测输出 |
| 2 | activity_out | 活动识别输出 |
| 3 | wrist_wear_wakeup_out | 佩戴唤醒输出 |
| 4 | wrist_gesture_out | 手腕手势输出 |
| 5 | no_motion_out | 静止（no motion）检测输出 |
| 6 | any_motion_out | 运动（any motion）检测输出 |
| 7 | reserved | — |

复位值：0x00（只读）

#### 0x1D INT_STATUS_1 — 数据类中断状态（clear on read）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | ffull_int | FIFO 满中断 |
| 1 | fwm_int | FIFO 水印中断 |
| 2 | err_int | 错误中断 |
| 3,4 | reserved | — |
| 5 | aux_drdy_int | 辅助数据就绪中断 |
| 6 | gyr_drdy_int | 陀螺仪数据就绪中断 |
| 7 | acc_drdy_int | 加速度计数据就绪中断 |

复位值：0x00（只读，读到 bit0 后清除）

#### 0x21 INTERNAL_STATUS — 内部初始化状态
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 3:0 | message | 0x00 not_init（ASIC 未初始化）、**0x01 init_ok**、0x02 init_err、0x03 drv_err、0x04 sns_stop、0x05 nvm_error、0x06 start_up_error、0x07 compat_error |
| 4 | Reserved | — |
| 5 | axes_remap_error | 轴重映射错误（X/Y/Z 不能映射到同一轴） |
| 6 | odr_50hz_error | 需要 50 Hz 数据的特性未满足最小带宽条件 |
| 7 | Reserved | — |

复位值：0x00（只读）。**驱动用它判断配置文件是否加载成功：message = 0x01（bit0=1）即成功。**

#### 0x40 ACC_CONF — 加速度计 ODR / 带宽 / 滤波模式
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 3:0 | acc_odr | ODR 选择：0x01=25/32 Hz、0x02=25/16、0x03=25/8、0x04=6.25、0x05=12.5、0x06=25、0x07=50、0x08=100、0x09=200、0x0A=400、0x0B=800、0x0C=1600 Hz；0x0D–0x0F reserved |
| 6:4 | acc_bwp | 带宽参数（决定滤波配置 / 欠采样平均）：0x00 osr4_avg1、0x01 osr2_avg2、0x02 norm_avg4、0x03 cic_avg8、0x04 res_avg16、0x05 res_avg32、0x06 res_avg64、0x07 res_avg128（acc_filter_perf=1 时 0x04–0x07 为 Reserved） |
| 7 | acc_filter_perf | 滤波性能模式：0x00 = ulp（省电，允许欠采样）、0x01 = hp（性能优先、抗混叠） |

复位值：0xA8
- 低功耗模式示例（手册 §3 例 2）：写 0x17 → acc_filter_perf=0，acc_bwp=2（2 次重复），acc_odr=0x07（50 Hz）
- 正常/性能模式示例（手册 §3 例 3/4）：写 0xA8 → acc_filter_perf=1，acc_bwp=normal，acc_odr=100 Hz

#### 0x41 ACC_RANGE — 加速度计量程
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 1:0 | acc_range | 0x00 = ±2 g（16384 LSB/g）、0x01 = ±4 g（8192 LSB/g）、0x02 = ±8 g（4096 LSB/g）、0x03 = ±16 g（2048 LSB/g） |
| 7:2 | reserved | — |

复位值：0x02（±8 g）

#### 0x42 GYR_CONF — 陀螺仪 ODR / 带宽 / 性能模式
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 3:0 | gyr_odr | ODR 选择：0x06=25、0x07=50、0x08=100、0x09=200、0x0A=400、0x0B=800、0x0C=1600、0x0D=3200 Hz；0x00–0x05 与 0x0E/0x0F reserved（6.4 kHz 需走 FIFO 读出，见 §4.7） |
| 5:4 | gyr_bwp | 带宽系数（3 dB 截止）：0x00 osr4、0x01 osr2、0x02 norm、0x03 reserved |
| 6 | gyr_noise_perf | 噪声性能：0x00 = ulp、0x01 = hp（低噪声高性能） |
| 7 | gyr_filter_perf | 滤波性能：0x00 = ulp、0x01 = hp（抗混叠） |

复位值：0xA9
- 低功耗：gyr_filter_perf=0、gyr_noise_perf=0（见手册 Power Modes 表）
- 正常模式示例：写 0xA9 → gyr_filter_perf=1、gyr_bwp=normal、gyr_odr=200 Hz
- 性能模式示例：写 0xE9 → 再加 gyr_noise_perf=1

#### 0x43 GYR_RANGE — 陀螺仪量程
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 2:0 | gyr_range | 0x00 = ±2000 dps（16.4 LSB/dps）、0x01 = ±1000 dps（32.8 LSB/dps）、0x02 = ±500 dps（65.6 LSB/dps）、0x03 = ±250 dps（131.2 LSB/dps）、0x04 = ±125 dps（262.4 LSB/dps） |
| 3 | ois_range | OIS 通路量程：0x00 = ±250 dps（131.2 LSB/dps）、0x01 = ±2000 dps（16.4 LSB/dps） |

复位值：0x00（±2000 dps）

> 说明：手册 §1 规格表中给出的灵敏度为 ±2g=16384 LSB/g、±4g=8192、±8g=4096、±16g=2048 LSB/g；陀螺仪 ±2000dps=16.384、±1000dps=32.768、±500dps=65.536、±250dps=131.072、±125dps=262.144 LSB/dps（与寄存器表内数值一致，寄存器表内为 16.4/32.8/… 的四舍五入写法）。

#### 0x0C–0x13 ACC 数据寄存器 — 加速度输出
| 地址 | 位 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0x0C (DATA_8) | 7:0 | acc_x_7_0 | ACC_X 低字节 |
| 0x0D (DATA_9) | 7:0 | acc_x_15_8 | ACC_X 高字节 |
| 0x0E (DATA_10) | 7:0 | acc_y_7_0 | ACC_Y 低字节 |
| 0x0F (DATA_11) | 7:0 | acc_y_15_8 | ACC_Y 高字节 |
| 0x10 (DATA_12) | 7:0 | acc_z_7_0 | ACC_Z 低字节 |
| 0x11 (DATA_13) | 7:0 | acc_z_15_8 | ACC_Z 高字节 |

复位值：0x00（只读）

#### 0x12–0x17 GYR 数据寄存器 — 陀螺仪输出
| 地址 | 位 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0x12 (DATA_14) | 7:0 | gyr_x_7_0 | GYR_X 低字节 |
| 0x13 (DATA_15) | 7:0 | gyr_x_15_8 | GYR_X 高字节 |
| 0x14 (DATA_16) | 7:0 | gyr_y_7_0 | GYR_Y 低字节 |
| 0x15 (DATA_17) | 7:0 | gyr_y_15_8 | GYR_Y 高字节 |
| 0x16 (DATA_18) | 7:0 | gyr_z_7_0 | GYR_Z 低字节 |
| 0x17 (DATA_19) | 7:0 | gyr_z_15_8 | GYR_Z 高字节 |

复位值：0x00（只读）

**数据格式**：加速度与陀螺仪均为 16 位二进制补码（two's complement），MSB/LSB 分两个寄存器存放。**读取必须从 LSB 开始**（读 LSB 会锁定对应 MSB 的内容，shadowing 机制）。手册 §3 示例：`burst_read(addr=0x0C, bytes=12)` 一次读出 ACC+GYR 全部 12 字节（0x0C..0x17）。

**量程换算**：`物理值 = (int16)(MSB<<8 | LSB) / 灵敏度`。灵敏度随量程变化，见 0x41 / 0x43 两节。

**陀螺仪后处理（手册 §4.6 "Gyroscope Data Post-Processing" 原文）**：
```
Ratex = DATA_15<<8 + DATA_14 - GYR_CAS.factor_zx * (DATA_19<<8 + DATA_18) / 29
Ratey = DATA_17<<8 + DATA_16
Ratez = DATA_19<<8 + DATA_18
```
注：`GYR_CAS.factor_zx` 为 7 位补码有符号值（位于 FEATURES page 0 的 0x3C），不做后处理时需自行实现符号扩展。

#### 0x45 FIFO_DOWNS — FIFO 降采样与数据源选择
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 2:0 | gyr_fifo_downs | 陀螺仪降采样因子 = 2^downs，downs ∈ {0..7} |
| 3 | gyr_fifo_filt_data | 0x00 = unfiltered（未滤波，采样率 6400 Hz）、0x01 = filtered |
| 6:4 | acc_fifo_downs | 加速度降采样因子 = 2^downs，downs ∈ {0..7} |
| 7 | acc_fifo_filt_data | 0x00 = unfiltered（未滤波，采样率 1600 Hz）、0x01 = filtered |

复位值：0x88

#### 0x46 / 0x47 FIFO_WTM_0 / FIFO_WTM_1 — FIFO 水印阈值
| 寄存器 | 位 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0x46 | 7:0 | fifo_water_mark_7_0 | 水印阈值低 8 位 |
| 0x47 | 4:0 | fifo_water_mark_12_8 | 水印阈值高 5 位 |

说明：当 FIFO 中的字节数达到 `fifo_water_mark_7_0 + fifo_water_mark_12_8 × 256` 时触发水印中断。
复位值：FIFO_WTM_0 = 0x00；FIFO_WTM_1 = 0x02

#### 0x48 FIFO_CONFIG_0 — FIFO 行为配置
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | fifo_stop_on_full | 0x00 = disable（满了继续覆盖最旧数据）、0x01 = enable（满了停止写入） |
| 1 | fifo_time_en | 0x00 = disable、0x01 = enable（在最后一个有效数据帧后回传 sensortime 帧） |

复位值：0x02（即 fifo_time_en = 1）

#### 0x49 FIFO_CONFIG_1 — FIFO 帧内容配置
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 1:0 | fifo_tag_int1_en | INT1 tag：0x00 int_edge（上升沿）、0x01 int_level（电平）、0x02 acc_sat（加速度饱和）、0x03 gyr_sat（陀螺仪饱和） |
| 3:2 | fifo_tag_int2_en | INT2 tag：同上四种取值（0x00 int_edge、0x01 int_level、0x02 acc_sat、0x03 gyr_sat） |
| 4 | fifo_header_en | 0x00 = disable（无 header，所有使能传感器 ODR 必须相同）、0x01 = enable（存储 header） |
| 5 | fifo_aux_en | 0x00/0x01 = 辅助传感器数据（三轴）不存/存入 FIFO |
| 6 | fifo_acc_en | 0x00/0x01 = 加速度数据不存/存入 FIFO |
| 7 | fifo_gyr_en | 0x00/0x01 = 陀螺仪数据不存/存入 FIFO |

复位值：0x10（即 fifo_header_en = 1）

**FIFO header 格式（手册 §4.7，header 模式）**：
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | fh_mode | 0b10 = 数据帧（regular）、0b01 = 控制帧、0b00/0b11 = reserved |
| 5:2 | fh_parm | 数据帧：使能的传感器位；控制帧：控制操作码。fh_parm=0b0000 在 regular 模式非法，header=0x80 表示未初始化帧（读超量时返回，含 1 字节 payload 0x00） |
| 1:0 | fh_ext | INT2 与 INT1 的 tag |

**FIFO 读/溢出要点**：`FIFO_DATA(0x26)` 地址不自增，必须 burst 读；帧被完整读完才从 FIFO 删除，部分读取会整帧重传（header 模式含 header）。读超量时 headerless 模式返回 0x8000，header 模式返回 0x80。
**FIFO 复位**：写 `CMD(0x7E) = 0xB0`（fifo_flush）。自动复位发生在：headerless 模式下使能/禁用传感器、header 与 headerless 模式切换、辅助数据帧长度变化。
**FIFO 中断**：FIFO 满（阈值 = 只剩最后两帧时）与水印中断，需经 `INT_MAP_DATA` 映射到 INT 引脚。锁存模式下只有状态寄存器被读取且填充电平低于对应阈值时才清除。

#### 0x4A SATURATION — 原始数据饱和标志
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | acc_x | ACC X 轴原始数据饱和（达到 0x8001 或 0x7FFF） |
| 1 | acc_y | ACC Y 轴饱和 |
| 2 | acc_z | ACC Z 轴饱和 |
| 3 | gyr_x | GYR X 轴饱和 |
| 4 | gyr_y | GYR Y 轴饱和 |
| 5 | gyr_z | GYR Z 轴饱和 |

复位值：0x00（只读，与 DATA_0..19 同步更新）

#### 0x52 ERR_REG_MSK — 错误中断屏蔽
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | fatal_err | 1 = 用 fatal error 触发 error 中断 |
| 4:1 | internal_err | 1 = 用 internal error 触发 error 中断 |
| 6 | fifo_err | 1 = 用 fifo error 触发 error 中断 |
| 7 | aux_err | 1 = 用 aux interface error 触发 error 中断 |

复位值：0x00（'1' 表示用于产生 error 中断，'0' 表示不用于产生）

#### 0x53 INT1_IO_CTRL — INT1 引脚电气配置
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | reserved | — |
| 1 | lvl | 中断有效电平：0x00 = active_low（低有效）、0x01 = active_high（高有效） |
| 2 | od | 输出形式：0x00 = push_pull（推挽）、0x01 = open_drain（开漏） |
| 3 | output_en | 输出使能：0x00 = off、0x01 = on（**要用 INT1 必须先置 1**） |
| 4 | input_en | 输入使能：0x00 = off、0x01 = on（FIFO tag 同步外部中断时需置 1） |

复位值：0x00

#### 0x54 INT2_IO_CTRL — INT2 引脚电气配置
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | reserved | — |
| 1 | lvl | 中断有效电平：0x00 = active_low、0x01 = active_high |
| 2 | od | 输出形式：0x00 = push_pull、0x01 = open_drain |
| 3 | output_en | 输出使能：0x00 = off、0x01 = on |
| 4 | input_en | 输入使能：0x00 = off、0x01 = on |

复位值：0x00

#### 0x55 INT_LATCH — 中断锁存模式
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | int_latch | 0x00 = none（非锁存，适合边沿触发中断的 MCU）、0x01 = permanent（永久锁存，适合电平触发） |
| 7:1 | reserved | — |

复位值：0x00

> 锁存模式下：INT_STATUS_0/INT_STATUS_1 中已置位的状态与所选引脚，在读取对应状态寄存器后被复位；若触发条件仍成立则会再次置位。若非锁存模式，引脚在触发条件消失时即复位，而状态位保持到被主机读取。
> **注意（手册 §4.9 原文）**：锁存模式下若使用两个中断引脚，INT_STATUS_0 的中断应全部映射到一个引脚、INT_STATUS_1 的中断全部映射到另一个引脚。只用单个引脚时可任意映射。

#### 0x56 INT1_MAP_FEAT — 高级特性中断映射到 INT1
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | sig_motion_out | 显著运动输出 |
| 1 | step_counter_out | 计步水印 / 步态检测输出 |
| 2 | activity_out | 活动识别输出 |
| 3 | wrist_wear_wakeup_out | 佩戴唤醒输出 |
| 4 | wrist_gesture_out | 手腕手势输出 |
| 5 | no_motion_out | 静止检测输出 |
| 6 | any_motion_out | 运动检测输出 |
| 7 | reserved | — |

复位值：0x00（置 1 = 映射该中断到 INT1）

#### 0x57 INT2_MAP_FEAT — 高级特性中断映射到 INT2
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | sig_motion_out | 显著运动输出 |
| 1 | step_counter_out | 计步水印 / 步态检测输出 |
| 2 | activity_out | 活动识别输出 |
| 3 | wrist_wear_wakeup_out | 佩戴唤醒输出 |
| 4 | wrist_gesture_out | 手腕手势输出 |
| 5 | no_motion_out | 静止检测输出 |
| 6 | any_motion_out | 运动检测输出 |
| 7 | reserved | — |

复位值：0x00（置 1 = 映射该中断到 INT2）

#### 0x58 INT_MAP_DATA — 数据类中断映射（两个引脚）
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | ffull_int1 | FIFO 满中断映射到 INT1 |
| 1 | fwm_int1 | FIFO 水印中断映射到 INT1 |
| 2 | drdy_int1 | 数据就绪中断映射到 INT1 |
| 3 | err_int1 | 错误中断映射到 INT1 |
| 4 | ffull_int2 | FIFO 满中断映射到 INT2 |
| 5 | fwm_int2 | FIFO 水印中断映射到 INT2 |
| 6 | drdy_int2 | 数据就绪中断映射到 INT2 |
| 7 | err_int2 | 错误中断映射到 INT2 |

复位值：0x00（置 1 = 映射）

> 数据就绪（drdy）中断在非锁存模式下会在 1/(6400 Hz) 后自动清除；需要保持则用锁存模式。`INT_STATUS_1.acc_drdy_int` 读 INT_STATUS_1 清除；`STATUS.drdy_acc` 读 DATA_8..DATA_13 中任一寄存器清除。

#### 0x59 INIT_CTRL — 启动配置加载
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | init_ctrl | 0x00 = 准备配置加载；0x01 = 完成配置加载。**该操作在每次 POR 或软复位后只能执行一次** |

复位值：0x00

#### 0x5B / 0x5C INIT_ADDR_0 / INIT_ADDR_1 — 初始化数据基地址
| 寄存器 | 位 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0x5B | 3:0 | base_0_3 | 初始化数据基地址 bit 3..0 |
| 0x5C | 7:0 | base_11_4 | 初始化数据基地址 bit 11..4 |

复位值：均为 0x00。若主机单次 burst 写能力不足 8 kB，可分块写入，**每块写完后按"第一块写入长度(字节)/2"递增这两个寄存器**。若主机可单次 8 kB burst 写，则忽略这两个寄存器。

#### 0x5E INIT_DATA — 初始化数据写入端口
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | data | 配置数据字节写入端口。**地址不自增（address trap），burst 写不会跨寄存器** |

复位值：0x00

#### 0x5F INTERNAL_ERROR — 内部错误标志
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 1 | int_err_1 | 内部错误：处理时间过长、处理停止 |
| 2 | int_err_2 | 内部错误：致命错误、处理停止 |
| 4 | feat_eng_disabled | 主机在传感器运行期间禁用了特性引擎 |
| 其余 | reserved | 保留位读值应忽略 |

复位值：0x00（只读）

#### 0x6B IF_CONF — 串行接口设置
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | spi3 | 主接口 SPI 模式：0x00 = spi4（4 线）、0x01 = spi3（3 线） |
| 1 | spi3_ois | OIS 接口 SPI 模式：0x00 = spi4、0x01 = spi3 |
| 4 | ois_en | OIS 接口使能（优先级低于 aux_en） |
| 5 | aux_en | AUX 接口使能（优先级高于 ois_en） |

复位值：0x00

#### 0x70 NV_CONF — NVM 支持配置位
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | spi_en | 自动配置模式下禁用 I²C、启用 SPI：0x00 = disabled（I²C 使能）、0x01 = enabled（I²C 禁用） |
| 1 | i2c_wdt_sel | I²C 看门狗超时时长：0x00 = short（1.25 ms）、0x01 = long（40 ms） |
| 2 | i2c_wdt_en | I²C 接口模式下的 SDI 引脚看门狗：0x00 = Disable、0x01 = Enable |
| 3 | acc_off_en | 0x00 = Disabled、0x01 = Enabled（把 OFFSET 寄存器中 off_acc_[xyz] 定义的偏置加到滤波与未滤波的加速度数据上） |

复位值：0x00

#### 0x71–0x77 OFFSET_0..6 — 偏置补偿（NVM backed）
| 地址 | 位 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0x71 | 7:0 | off_acc_x | 加速度计 X 轴偏置补偿 |
| 0x72 | 7:0 | off_acc_y | 加速度计 Y 轴偏置补偿 |
| 0x73 | 7:0 | off_acc_z | 加速度计 Z 轴偏置补偿 |
| 0x74 | 7:0 | gyr_usr_off_x_7_0 | 陀螺仪 X 轴偏置补偿低 8 位 |
| 0x75 | 7:0 | gyr_usr_off_y_7_0 | 陀螺仪 Y 轴偏置补偿低 8 位 |
| 0x76 | 7:0 | gyr_usr_off_z_7_0 | 陀螺仪 Z 轴偏置补偿低 8 位 |
| 0x77 | 1:0 | gyr_usr_off_x_9_8 | 陀螺仪 X 轴偏置补偿高 2 位 |
| 0x77 | 3:2 | gyr_usr_off_y_9_8 | 陀螺仪 Y 轴偏置补偿高 2 位 |
| 0x77 | 5:4 | gyr_usr_off_z_9_8 | 陀螺仪 Z 轴偏置补偿高 2 位 |
| 0x77 | 6 | gyr_off_en | 0x00 = Disabled、0x01 = Enabled（把 gyr_usr_off_[xyz] 偏置加到滤波与未滤波的陀螺仪数据上） |
| 0x77 | 7 | gyr_gain_en | 0x00 = Disabled、0x01 = Enabled（按 §4.14 灵敏度误差补偿修正增益） |

复位值：均为 0x00

#### 0x7C PWR_CONF — 电源配置
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | adv_power_save | 0x00 = aps_off（禁用 advanced power save）、0x01 = aps_on（启用 advanced power save） |
| 1 | fifo_self_wake_up | 0x00 = fsw_off（低功耗模式下禁止读 FIFO）、0x01 = fsw_on（FIFO 中断触发后允许在低功耗模式下读 FIFO） |
| 2 | fup_en | 快速上电（fast power up）：0x00 = fup_off、0x01 = fup_on |
| 7:3 | reserved | — |

复位值：0x03（POR 或软复位后 adv_power_save = 1，fifo_self_wake_up = 1）

> 手册 §3 强调：**POR/软复位后器件默认处于 advanced power save 模式**，读 FIFO 前必须把 adv_power_save 清 0；配置写入需要 ≥ 450 µs 的写间隔。初始化流程第一步就是写 0x7C = 0x00。

#### 0x7D PWR_CTRL — 电源控制
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 0 | aux_en | 0x00 = aux_off（禁用辅助传感器）、0x01 = aux_on（启用） |
| 1 | gyr_en | 0x00 = gyr_off（禁用陀螺仪）、0x01 = gyr_on（启用） |
| 2 | acc_en | 0x00 = acc_off（禁用加速度计）、0x01 = acc_on（启用） |
| 3 | temp_en | 0x00 = temp_off（禁用温度传感器）、0x01 = temp_on（启用） |
| 7:4 | reserved | — |

复位值：0x00（默认所有传感器关闭）

> 手册 §3 示例的常用值：`0x04` = 仅加速度（低功耗例）、`0x0E` = 加速度+陀螺仪+温度（正常/性能例）。

#### 0x7E CMD — 命令寄存器
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | cmd | 0x02 = g_trigger（触发特殊陀螺仪操作）、0x03 = usr_gain（应用新陀螺仪增益）、0xA0 = nvm_prog（把 NVM backed 寄存器写入 NVM）、0xB0 = fifo_flush（清空 FIFO）、0xB6 = **softreset**（复位，所有用户配置恢复默认值） |

复位值：0x00（只写；**读该寄存器始终返回 0x00**）
执行前应轮询 `STATUS.cmd_rdy`（= 1 表示命令解码器空闲可接受新命令）。NVM 写入只能在 `PWR_CONF.adv_power_save = 0` 时发起，且在 NVM 写进行中不得访问 NVM shadow 寄存器。

#### 0x30–0x3F FEATURES[0..15] — 特性/输出寄存器块
**机制**：`FEAT_PAGE(0x2F).page[2:0]` 选择 0–7 页，每页把 16 个特性寄存器映射到 0x30–0x3F（每寄存器 16 位，按小端 2 字节访问）。复位后 page = 0。特性输出同时出现在对应的专用寄存器（如 INT_STATUS_0 / SC_OUT_x / WR_GEST_ACT）。

| 页 | 0x30 | 0x32 | 0x34 | 0x36 | 0x38 | 0x3A | 0x3C | 0x3E |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | SC_OUT_0_1 | SC_OUT_2_3 | ACT_OUT | WR_GEST_OUT | GYR_GAIN_STATUS | Reserved | GYR_CAS | Reserved |
| 1 | Reserved | G_TRIG_1 | GEN_SET_1 | GYR_GAIN_UPD_1 | GYR_GAIN_UPD_2 | GYR_GAIN_UPD_3 | ANYMO_1 | ANYMO_2 |
| 2 | NOMO_1 | NOMO_2 | SIGMO_1 | Reserved | Reserved | Reserved | Reserved | SIGMO_2 |
| 3 | SC_1 | SC_2 | SC_3 | SC_4 | SC_5 | SC_6 | SC_7 | SC_8 |
| 4 | SC_9 | SC_10 | SC_11 | SC_12 | SC_13 | SC_14 | SC_15 | SC_16 |
| 5 | SC_17 | SC_18 | SC_19 | SC_20 | SC_21 | SC_22 | SC_23 | SC_24 |
| 6 | SC_25 | SC_26 | SC_27 | WR_GEST_1 | WR_GEST_2 | WR_GEST_3 | WR_GEST_4 | Reserved |
| 7 | WR_WAKEUP_1 | WR_WAKEUP_2 | WR_WAKEUP_3 | WR_WAKEUP_4 | WR_WAKEUP_5 | WR_WAKEUP_6 | WR_WAKEUP_7 | Reserved |

关键字段示例（手册原文值）：
- Page 1 `GEN_SET_1(0x34)`：复位 0x0088；bit1:0 map_x_axis、bit2 map_x_axis_sign、bit4:3 map_y_axis、bit5 map_y_axis_sign、bit7:6 map_z_axis、bit8 map_z_axis_sign、bit9 gyr_self_off、bit10 nvm_prog_prep。
- Page 1 `ANYMO_1(0x3C)`：复位 0xE005；bit12:0 duration（50 Hz 采样数，默认 5 = 100 ms）、bit13 select_x、bit14 select_y、bit15 select_z。
- Page 1 `ANYMO_2(0x3E)`：复位 0x38AA；bit10:0 threshold（0…1 g，默认 0xAA = 83 mg）、bit14:11 out_conf（输出分配到 INT_STATUS_0 的 bit0–7）、bit15 enable。
- Page 2 `NOMO_1(0x30)` 复位 0xE005 / `NOMO_2(0x32)` 复位 0x3090（阈值默认 0x90 = 70 mg）；`SIGMO_1(0x34)` 复位 0x00FA（block_size，默认 0xFA = 5 s）；`SIGMO_2(0x3E)` 复位 0x0002（enable / out_conf）。
- Page 0 `ACT_OUT(0x34)` bit1:0 act_out：0x00 still、0x01 walking、0x02 running、0x03 unknown。
- Page 0 `WR_GEST_OUT(0x36)` bit2:0 wr_gest_out：0x00 unknown_gesture、0x01 push_arm_down、0x02 pivot_up、0x03 wrist_shake_jiggle、0x04 flick_in、0x05 flick_out。
- Page 0 `GYR_GAIN_STATUS(0x38)`：bit0 sat_x、bit1 sat_y、bit2 sat_z、bit5:3 g_trig_status（0x00 no_err、0x01 precon_err、0x02 dl_err、0x03 abort_err）。
- Page 0 `GYR_CAS(0x3C)` bit6:0 factor_zx（陀螺仪 CAS 后处理系数，7 位补码）。
- Page 6 `SC_26(0x32)`：bit9:0 watermark_level（隐含水印倍数 20，分辨率 20 步）、bit10 en_detector、bit11 en_counter、bit12 en_activity、bit13 reset_counter；`SC_27(0x34)` bit3:0 out_conf_step_detector。

复位值：Page 0 的 SC_OUT_0_1 / SC_OUT_2_3 / ACT_OUT / WR_GEST_OUT / GYR_GAIN_STATUS / GYR_CAS 与 Page 1–7 的多数字段复位为 0x0000（Page 1 GEN_SET_1 复位 0x0088，ANYMO_1 复位 0xE005，ANYMO_2 复位 0x38AA 等例外值见上）。手册未在逐位表中给出全部字段的独立复位值，**其余字段的复位值请以手册 §5 "Register (0x30) FEATURES[16]" 原表为准（待确认）**。

#### 0x44 AUX_CONF / 0x4B–0x4F 辅助接口寄存器（工程未用，列此备查）
| 地址 | 位 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0x44 | 3:0 | aux_odr | 辅助传感器轮询率，编码同 ACC_CONF.acc_odr（0x06 = 25 Hz …，默认 0x06） |
| 0x44 | 7:4 | aux_offset | trigger-readout 偏移，单位 2.5 ms；为 0 时偏移最大（读后立即触发） |
| 0x4B | 7:1 | i2c_device_addr | 辅助传感器 I²C 地址（复位 0x10，寄存器值 0x20） |
| 0x4C | 1:0 | aux_rd_burst | 自动读 burst 长度：0x00 BL1、0x01 BL2、0x02 BL6、0x03 BL8 |
| 0x4C | 3:2 | man_rd_burst | 手动读 burst 长度：同上 |
| 0x4C | 6 | aux_fcu_write_en | 为需要 trigger 的辅助传感器启用 FCU 写命令 |
| 0x4C | 7 | aux_manual_en | 1 = 手动模式（读写需手工触发）；0 = 自动模式（FCU 周期性触发） |
| 0x4D | 7:0 | read_addr | 辅助接口读地址（手动模式下写它即触发读） |
| 0x4E | 7:0 | write_addr | 辅助接口写地址（手动模式下写它即触发写） |
| 0x4F | 7:0 | write_data | 辅助接口写数据 |

复位值：AUX_CONF = 0x46；AUX_DEV_ID = 0x20；AUX_IF_CONF = 0x83；AUX_RD_ADDR = 0x42；AUX_WR_ADDR = 0x4C；AUX_WR_DATA = 0x02

#### 其它只列总表的寄存器（未逐位展开）
以下地址在 3.1 总表中已完整列出，本文未逐位展开（如需使用请查手册 §5 对应小节）：
- `0x18–0x1A SENSORTIME_0..2`：24 位 sensor time，分辨率 39.0625 µs，到 0xFFFFFF 回绕。
- `0x1E/0x1F SC_OUT_0/1`：计步值 byte-0/byte-1（只读）。
- `0x20 WR_GEST_ACT`：bit2:0 wr_gest_out、bit4:3 act_out（编码同 FEATURES page 0）。
- `0x22/0x23 TEMPERATURE_0/1`：16 位温度，分辨率 1/2^9 K/LSB，0x0000 → 23 °C，0x8000 为无效值；陀螺仪处于 normal 模式时每 10 ms 更新，否则每 1.28 s 更新。复位值 0x00 / 0x80。
- `0x24/0x25 FIFO_LENGTH_0/1`：FIFO 当前填充字节数（bit13..0；0x000 表示空）。
- `0x68 AUX_IF_TRIM`：bit1:0 asda_pupsel（0x00 off / 0x01 40k / 0x02 10k / 0x03 2k）、bit2 spare3。复位 0x01。
- `0x69 GYR_CRT_CONF`：bit2 crt_running、bit3 rdy_for_dl。复位 0x00。
- `0x6A NVM_CONF`：bit1 nvm_prog_en。复位 0x00。
- `0x6C DRV`：bit2:0 io_pad_drv1、bit3 io_pad_i2c_b1、bit6:4 io_pad_drv2、bit7 io_pad_i2c_b2。复位 0xFF。
- `0x6D ACC_SELF_TEST`：bit0 acc_self_test_en、bit2 acc_self_test_sign、bit3 acc_self_test_amp。复位 0x00。
- `0x6E GYR_SELF_TEST_AXES`：bit0 gyr_st_axes_done、bit1/2/3 gyr_axis_x/y/z_ok（只读）。复位 0x00。
- `0x2F FEAT_PAGE`：bit2:0 page。复位 0x00。

### 3.3 初始化必需的配置写入序列（configuration file 机制）

BMI270 与其他 IMU 最大的不同：**陀螺仪/加速度计的正常工作依赖一段约 8 kB 的二进制"配置文件（configuration file）"，必须在每次 POR 或软复位后由主机写入器件**。手册 §3 与 §4.4 给出完整流程，对应的 `bmi270_config_file` 二进制在 Bosch 的 GitHub（BMI270-Sensor-API 的 `bmi270.c`）提供，**手册不展开这段二进制的内容**。

**手册 §4.4 "Power-On-Reset (POR) and Device Initialization" 原文步骤**：

| 步骤 | 操作 | 涉及寄存器 |
| --- | --- | --- |
| 0 | POR 后 VDD/VDDIO 达到目标值，450 µs 后所有寄存器可访问 | — |
| 1 | 禁用 advanced power save：写 `PWR_CONF.adv_power_save = 0b0`（即 0x7C ← 0x00） | 0x7C |
| 2 | 等待 450 µs（或 12 LSB 的 SENSORTIME_0） | 0x18 |
| 3 | 写 `INIT_CTRL.init_ctrl = 0x00`，准备配置加载 | 0x59 |
| 4 | 向 `INIT_DATA` **burst 写 8 kB 初始化数据**（从配置数据 byte 0 开始）。10 MHz SPI 下约需 6.6 ms | 0x5E |
| 4b | （可选）burst 回读 `INIT_DATA`，与写入数据比对校验 | 0x5E |
| 4c | 若主机 burst 写能力 < 8 kB，可分块写；**每块之间把 INIT_ADDR_0/INIT_ADDR_1 递增"第一块写入长度(字节)/2"** | 0x5B、0x5C |
| 5 | 写 `INIT_CTRL.init_ctrl = 0x01`，完成配置加载。**此操作在每次 POR/软复位后不得执行多于一次** | 0x59 |
| 6 | 等待直到 `INTERNAL_STATUS.message = 0b0001`（最多 20 ms） | 0x21 |
| 7 | 初始化完成，器件自动进入 "Configuration mode"，此后可切换其它功耗模式并正常使用 | — |

**手册 §3 给出的等价流程图（Quick Start Guide 例 1）**：
```
write_reg(addr=0x7C, val=0x00)      # disable PWR_CONF.adv_power_save
sleep(450us)
write_reg(addr=0x59, val=0x00)      # INIT_CTRL = 0x00（prepare config load）
init_array[] = read_file(init_file)
burst_write_reg(addr=0x5E, val=init_array[])   # 向 INIT_DATA burst 写
write_reg(addr=0x59, val=0x01)      # INIT_CTRL = 0x01（complete config load）
# 等待 >= 140 ms，然后：
init_status = read_reg(0x21)        # 检查 INTERNAL_STATUS.0 == 1 ？OK : ERROR
```

> 硬性说明：配置二进制的内容手册未展开，**本文按要求不展开其每一位**。驱动实现时直接使用 Bosch 官方 `bmi270_config_file` 数组。手册未在正文说明是否必须额外开启某个寄存器位才能接收配置数据（流程仅涉及 0x7C / 0x59 / 0x5B / 0x5C / 0x5E / 0x21），也未给出配置文件的校验和字段说明（**待确认**）。

**软复位（手册 §4.17 "Soft-Reset"）**：
- 向 `CMD(0x7E)` 写 `0xB6`（softreset）即可，任意操作模式下均可用。
- 软复位基本等价于一次断电重启，延迟后所有用户配置恢复 NVM 中保存的默认值。
- **不得在 NVM 写操作进行中执行**。
- 软复位后访问 SPI/I²C 的时序约束与上电相同（即需重新走上面的初始化序列；NVM 读期间写入传感器寄存器会被丢弃、读操作会返回 STATUS 寄存器内容）。

**上电后的功耗模式与初始化后的建议流程**（手册 §4.5 Power Modes）：
- 复位后默认：Suspend 模式（acc_en=0、gyr_en=0、adv_power_save=1），典型电流 3.5 µA。
- 完成配置加载后自动进入 Configuration mode（典型 120 µA）。
- 正常使用示例（手册 §3 例 3）：`0x7D ← 0x0E`（ACC+GYR+TEMP 使能，AUX 关）、`0x40 ← 0xA8`（ACC 100 Hz/正常带宽/性能）、`0x42 ← 0xA9`（GYR 200 Hz/正常带宽/性能）、`0x7C ← 0x02`（关掉 adv_power_save、保留 fifo_self_wakeup），然后从 0x0C burst 读 12 字节。

### 3.4 本设计的 INT 引脚（GPIO12）配置建议路径

驱动要把中断引到 P4 的 GPIO12，按手册必须依次配置：
1. `INT1_IO_CTRL(0x53)` 或 `INT2_IO_CTRL(0x54)`：`output_en = 1` 开启输出；按硬件上下拉选 `lvl`（0 = 低有效 / 1 = 高有效）与 `od`（0 = 推挽 / 1 = 开漏）。若硬件接了外部上拉，通常配 `lvl=0`（低有效）+ `od=1`（开漏）。
2. `INT_LATCH(0x55).int_latch`：P4 用边沿触发选 0（非锁存），用电平触发选 1（锁存）。
3. 映射中断源：
   - 数据类（drdy / FIFO full / FIFO watermark / error）→ `INT_MAP_DATA(0x58)`；
   - 高级特性类（any/no motion、wrist、step、sig motion、activity）→ `INT1_MAP_FEAT(0x56)` 或 `INT2_MAP_FEAT(0x57)`。
4. 若要使用任意运动/静止唤醒等特性，还需先在 `FEAT_PAGE(0x2F)` 选中对应页、在 `FEATURES(0x30–0x3F)` 中使能该特性（out_conf/enable 位）。
5. 中断源识别：读 `INT_STATUS_0(0x1C)` / `INT_STATUS_1(0x1D)`（clear-on-read）。

> 注意：本设计有 4 个 I²C-1 从机（BMI270、CS43131、AXP2101、PCF85063），BMI270 只能取 0x68 或 0x69，**SDO 引脚的实际接法决定地址，需与硬件确认后再写入驱动**（待确认）。

> 手册依据：BMI270 Data sheet Rev 1.3（Nov 2020），Document number BST-BMI270-DS000-05；寄存器章节 §5 Register Description（p.75–130，其中寄存器映射表 p.76–80、逐寄存器说明 p.81–130）、初始化流程见 §3 Quick Start Guide（p.19–24）与 §4.4（p.27）、中断引脚配置见 §4.9（p.59）、I²C 地址与时序见 §6.5（p.138–141）、软复位见 §4.17（p.74）；未逐位展开的寄存器仅列在上表中。
## 4. PCF85063TP（RTC）

| 项 | 值 |
| --- | --- |
| 数据手册 | PCF85063TP.pdf Rev. 4（2015-05-06） |
| 7 位地址 | 0x51 |
| 8 位地址（写/读） | 0xA2 / 0xA3 |
| 地址配置 | 固定不可配置：手册只为 PCF85063TP 保留**一个** I2C 从地址 `1010001b`，无地址选择引脚、无地址配置寄存器 |
| 总线速率上限 | 400 kbit/s（手册标注 400 kHz I2C-bus interface，适用 VDD = 1.8 V ~ 5.5 V） |

> ⚠️ **选型限制（硬件决策项，投板前必须确认）**
> 本芯片**没有 Alarm，也没有 Timer / Watchdog**。手册 Table 35 选型表原文：**PCF85063TP** = *basic functions only, **no alarm***，Alarm/Timer/Watchdog 一栏为 `-`；带 Alarm + Timer + Watchdog 的是 **PCF85063A**，但其封装为 SO8 / DFN2626-10 / TSSOP8，与 TP 的 **HXSON8** 不兼容，**不能直接替换**。
> 因此本芯片 INT 引脚可用的中断源只有 **MI（每分钟）/ HMI（每 30 秒）/ CIE（偏移校正脉冲）**，**无法实现「闹钟定时唤醒」**。若产品需要闹钟唤醒，须在投板前改用 PCF85063A，或改用主控自身的 RTC 定时器方案。

补充说明：

- 同一总线可挂几个：**1 个**（地址固定且唯一，同型号器件无法在同一总线上共存）。
- 本芯片共 **11 个 8 位寄存器**，地址 0x00–0x0A。
- 寄存器地址自动递增：每读/写一个数据字节后内置地址寄存器自动 +1，**到 0x0A 后回卷到 0x00**。
- 寄存器地址为 **4 位**值，其高 4 位未使用（即传输时高 4 位写 0）。
- 读/写时间寄存器（0x04–0x0A）时全部时间计数器被冻结，防止进位期间读写出错；单次访问须在 1 秒内完成，且 秒→年 应在**一次连续访问**中读完/写完。
- 复位后器件状态：时间 00:00:00，日期 2000-01-01，星期 Saturday。

### 4.1 寄存器地址总表

| 地址 | 名称 | 功能 |
| --- | --- | --- |
| 0x00 | Control_1 | 控制与状态寄存器 1：外部时钟测试模式、STOP 停表、软件复位、校正中断使能、12/24 小时制、振荡器负载电容选择 |
| 0x01 | Control_2 | 控制与状态寄存器 2：分钟中断、半分钟中断、定时器标志、CLKOUT 输出频率选择 |
| 0x02 | Offset | 偏移寄存器：时钟精度微调（老化/温度补偿），含模式位与二位补码偏移值 |
| 0x03 | RAM_byte | 8 位自由 RAM 字节，可任意使用（例如存系统状态） |
| 0x04 | Seconds | 秒计数器（BCD 00–59）；bit7 为 OS 振荡器停止标志 |
| 0x05 | Minutes | 分钟计数器（BCD 00–59） |
| 0x06 | Hours | 小时计数器（BCD，12 或 24 小时制） |
| 0x07 | Days | 日期计数器（BCD 01–31，含闰年补偿） |
| 0x08 | Weekdays | 星期计数器（0–6） |
| 0x09 | Months | 月份计数器（BCD 01–12） |
| 0x0A | Years | 年份计数器（BCD 00–99） |

> 注：手册只有上述 11 个寄存器。**本型号没有闹钟寄存器，没有定时器（倒计时）寄存器，没有独立的看门狗寄存器**——详见 4.2 末尾的说明。

### 4.2 关键寄存器逐位说明

#### 0x00 Control_1 — 控制与状态寄存器 1
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | EXT_TEST | 外部时钟测试模式。0 = 正常模式（默认）；1 = 进入测试模式，此时 CLKOUT 引脚变为**输入**，内部时钟改由 CLKOUT 引脚上的信号驱动 |
| 6 | - | 未实现，固定 0 |
| 5 | STOP | RTC 时钟停止位。0 = RTC 时钟运行（默认）；1 = RTC 时钟停止，所有 RTC 分频链触发器被异步置 0。STOP 置 1 也会让低于 8 kHz 的 CLKOUT 输出变低电平 |
| 4 | SR | 软件复位。0 = 无软件复位（默认）；1 = 启动软件复位。**该位读出时恒为 0**。软件复位必须向本寄存器写入 `01011000b`（0x58），即 bit6、bit4、bit3 为 1、其余为 0 |
| 3 | - | 未实现，固定 0 |
| 2 | CIE | 校正中断使能（correction interrupt enable）。0 = 不产生校正中断（默认）；1 = 每个校正周期在 INT 引脚上产生中断脉冲 |
| 1 | 12_24 | 小时制选择。0 = 24 小时制（默认）；1 = 12 小时制 |
| 0 | CAP_SEL | 内部振荡器电容选择（对应晶体负载电容）。0 = 7 pF（默认）；1 = 12.5 pF |

复位值：0x00

#### 0x01 Control_2 — 控制与状态寄存器 2
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | - | 未实现，固定 00 |
| 5 | MI | 分钟中断（minute interrupt）。0 = 禁止（默认）；1 = 使能，每分钟在 INT 上产生一次中断脉冲 |
| 4 | HMI | 半分钟中断（half minute interrupt）。0 = 禁止（默认）；1 = 使能，每 30 秒产生一次中断脉冲 |
| 3 | TF | 定时器标志（timer flag）。0 = 未产生定时器中断（默认）；1 = MI 或 HMI 首次触发时置 1，并**保持置位直到被命令清除**（不会自动清除） |
| 2:0 | COF[2:0] | CLKOUT 输出频率选择，见下表。默认 000 |

复位值：0x00

**MI / HMI 组合效果（Table 10）**

| MI | HMI | 结果 |
| --- | --- | --- |
| 0 | 0 | 不产生中断 |
| 1 | 0 | 每分钟一次中断 |
| 0 | 1 | 每 30 秒一次中断 |
| 1 | 1 | 每 30 秒一次中断（两者叠加后不可区分） |

MI/HMI 与秒计数器同步；使能 MI 后首次中断在 1~59 秒之间出现，使能 HMI 后首次中断在 1~29 秒之间出现，之后周期稳定。MI/HMI **只应在偏移寄存器 MODE = 0（正常模式）下使用**；正常模式下 INT 上的中断脉冲宽度为 1/64 s。若 OFFSET[6:0] ≠ 00h，中断周期会受偏移校正影响。

**COF[2:0] 与 CLKOUT 频率（Table 11）**

| COF[2:0] | CLKOUT 频率 | 典型占空比 | STOP 位的影响 |
| --- | --- | --- | --- |
| 000（默认） | 32768 Hz | 60:40 ~ 40:60 | 无影响 |
| 001 | 16384 Hz | 50:50 | 无影响 |
| 010 | 8192 Hz | 50:50 | 无影响 |
| 011 | 4096 Hz | 50:50 | CLKOUT = LOW |
| 100 | 2048 Hz | 50:50 | CLKOUT = LOW |
| 101 | 1024 Hz | 50:50 | CLKOUT = LOW |
| 110 | 1 Hz | 50:50 | CLKOUT = LOW |
| 111 | CLKOUT = LOW（关闭） | - | - |

CLKOUT 引脚为**推挽（push-pull）输出，上电即使能**；置 COF[2:0] = 111 可将其关闭，关闭时输出低电平。除 32.768 kHz 外，其余频率占空比均为 50:50。1 Hz 输出会受偏移校正脉冲影响。

#### 0x02 Offset — 偏移（老化/校准）寄存器
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | MODE | 偏移模式。0 = 正常模式（normal mode，默认）：每 **2 小时**做一次偏移校正（之后每分钟施加一次校正脉冲直到用完）；1 = 粗调模式（手册原文 "course mode"）：每 **4 分钟**做一次校正（之后每秒施加一次，单秒最多 60 个脉冲） |
| 6:0 | OFFSET[6:0] | 偏移值，**二位补码**编码，范围 +63 LSB ~ -64 LSB。MODE = 0 时每 LSB = **4.34 ppm**；MODE = 1 时每 LSB = **4.069 ppm**。默认 0（不校正） |

复位值：0x00

- 校正通过**增加或减少时钟校正脉冲**实现，改变的是单个秒的周期，不改变振荡器频率。
- 校准后可达约 ±2 ppm 精度（0.5 个偏移 LSB）。1 ppm 对应每天 0.0864 秒偏差。
- MODE = 1 校正更频繁，但功耗更高。
- 校正脉冲可在 INT 上监视：置 CIE（Control_1 bit2）= 1，每个校正周期在 INT 上产生一个脉冲（MODE = 0 时脉冲宽 1/64 s；MODE = 1 时脉冲宽 1/1024 s，多个脉冲间隔 1/512 s）。

#### 0x03 RAM_byte — 8 位通用 RAM
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:0 | B[7:0] | 自由 RAM 字节，内容 00000000b ~ 11111111b，可作任意用途（如系统状态字节） |

复位值：0x00

#### 0x04 Seconds — 秒寄存器
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | OS | 振荡器停止标志（oscillator stop）。0 = 时钟完整性有保证；1 = 时钟完整性无保证，振荡器已停止或曾被中断。该位**保持置位直到被命令清除**（非自动清除）；若清除后仍置 1，说明振荡器未运行。上电到晶体稳定期间（约 200 ms ~ 2 s，随晶体/温度/电压而变）视为振荡器停止 |
| 6:4 | SECONDS（十位） | 实际秒的十位，BCD，0–5 |
| 3:0 | SECONDS（个位） | 实际秒的个位，BCD，0–9 |

有效值 00–59（BCD）。
复位值：0x80（OS = 1，秒 = 00）

#### 0x05 Minutes — 分寄存器
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7 | - | 未实现，固定 0 |
| 6:4 | MINUTES（十位） | 实际分钟的十位，BCD，0–5 |
| 3:0 | MINUTES（个位） | 实际分钟的个位，BCD，0–9 |

有效值 00–59（BCD）。
复位值：0x00

#### 0x06 Hours — 小时寄存器
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | - | 未实现，固定 00 |
| — | — | **以下为 12 小时制（Control_1 的 12_24 = 1）** |
| 5 | AMPM | AM/PM 指示。0 = AM（默认）；1 = PM |
| 4 | HOURS（十位） | 12 小时制下小时的十位，BCD，0–1 |
| 3:0 | HOURS（个位） | 12 小时制下小时的个位，BCD，0–9 |
| — | — | **以下为 24 小时制（Control_1 的 12_24 = 0，默认）** |
| 5:4 | HOURS（十位） | 24 小时制下小时的十位，BCD，0–2 |
| 3:0 | HOURS（个位） | 24 小时制下小时的个位，BCD，0–9 |

小时制由 Control_1 的 12_24 位决定。
复位值：0x00（默认 24 小时制，即 00 时）

#### 0x07 Days — 日期寄存器
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:6 | - | 未实现，固定 00 |
| 5:4 | DAYS（十位） | 实际日期的十位，BCD，0–3 |
| 3:0 | DAYS（个位） | 实际日期的个位，BCD，0–9 |

有效值 01–31（BCD）。闰年补偿：若年份计数器的值能被 4 整除（含 00 年），芯片自动为 2 月加第 29 天。
复位值：0x01（日期 = 1）

#### 0x08 Weekdays — 星期寄存器
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:3 | - | 未实现，固定 00000 |
| 2:0 | WEEKDAYS | 实际星期值，0–6，编码见下表。手册注明该定义可由用户重新指派 |

| 值 | 星期 | 值 | 星期 |
| --- | --- | --- | --- |
| 0 | Sunday | 4 | Thursday |
| 1 | Monday | 5 | Friday |
| 2 | Tuesday | 6 | Saturday（默认值） |
| 3 | Wednesday | | |

复位值：0x06（WEEKDAYS = 6 = Saturday）

#### 0x09 Months — 月寄存器
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:5 | - | 未实现，固定 000 |
| 4 | MONTHS（十位） | 实际月份的十位，BCD，0–1 |
| 3:0 | MONTHS（个位） | 实际月份的个位，BCD，0–9 |

有效值 01–12（BCD）。复位值 0x01 = January。
复位值：0x01

#### 0x0A Years — 年寄存器
| 位 | 字段 | 说明 |
| --- | --- | --- |
| 7:4 | YEARS（十位） | 实际年份的十位，BCD，0–9 |
| 3:0 | YEARS（个位） | 实际年份的个位，BCD，0–9 |

有效值 00–99（BCD），表示 2000–2099 年。
复位值：0x00

#### 闹钟寄存器 — 无

手册**没有**任何闹钟（Alarm）寄存器：11 个寄存器中不含 Alarm_Second / Alarm_Minute / Alarm_Hour / Alarm_Day / Alarm_Weekday，也没有闹钟使能位或屏蔽位。手册附录的 NXP RTC 选型表（Table 35）对本型号的描述为 “basic functions only, **no alarm**”，Alarm/Timer/Watchdog 一栏为 “-”。

因此本芯片**无法实现“到点唤醒”**；INT 引脚可用的中断源只有下面列出的三类。若项目需要闹钟唤醒，需改用带闹钟的型号（如同系列 PCF85063A）或用 MCU 侧定时器 + 周期性读时间实现。

#### 定时器寄存器 — 无

手册**没有**独立的定时器（Timer / 倒计时）寄存器：没有 Timer_mode、Timer_value、Timer 时钟源/分频/使能等寄存器。选型表 Table 35 中本型号 Alarm/Timer/Watchdog 一栏为 “-”。

注意术语陷阱：本手册中出现的 “timer flag”（TF，Control_2 bit3）与 “timer” 指的是 **MI / HMI 这两个预定义的分/半分钟中断定时器**，不是可编程倒计时器。

#### 中断相关（跨寄存器汇总）

**中断使能**

| 位置 | 字段 | 作用 |
| --- | --- | --- |
| Control_2 (0x01) bit5 | MI | 使能分钟中断（每分钟一次） |
| Control_2 (0x01) bit4 | HMI | 使能半分钟中断（每 30 秒一次） |
| Control_1 (0x00) bit2 | CIE | 使能偏移校正中断（每个校正周期一个脉冲） |

没有总开关式的 “INT enable” 位，也没有闹钟/定时器中断使能位。

**中断标志 / 状态**

| 位置 | 字段 | 置位条件 | 清除方式 |
| --- | --- | --- | --- |
| Control_2 (0x01) bit3 | TF | MI 或 HMI 首次触发时置 1 | 手册原文为 “remains set until it is cleared by command”（保持置位直到被命令清除），**非自动清除、非读清除**；手册未逐字给出“写 0 清除”的操作描述（待确认） |
| Seconds (0x04) bit7 | OS | 振荡器停止/被中断时置 1 | 同上，“remains set until cleared by command”，由软件写清除（待确认具体写值） |

本芯片**没有**独立的中断状态寄存器，标志位分散在 Control_2 与 Seconds 中。清除方式依据手册原文均为 “cleared by command”，即通过 I2C 写寄存器并由软件清除，**没有读到“读 1 清除”或“读寄存器自动清除”的说明**。

**INT 引脚（引脚 3）极性 / 输出类型配置**

- 类型：**开漏输出（open-drain）**，低电平有效；需外部上拉电阻。手册引脚说明原文：“INT 3 output interrupt output (open-drain)”。
- 本芯片**没有** INT 极性和输出类型（开漏/推挽）的配置位——不存在类似其它 RTC 的 T/TP 极性选择位或 push-pull 使能位。极性固定为低有效、类型固定为开漏。
- 中断脉冲宽度：MODE = 0 时 MI/HMI 脉冲宽 1/64 s；校正中断脉冲 MODE = 0 时宽 1/64 s，MODE = 1 时宽 1/1024 s（多个脉冲间隔 1/512 s）。

对应本项目：PCF85063TP 的 INT 接 ESP32-P4 的 GPIO11，应把 GPIO11 配成**带上拉的上降沿/低电平触发输入**，并在读取标志位后由软件清除 TF。

#### CLKOUT 输出配置

CLKOUT（引脚 7）为推挽输出，上电即使能，由 Control_2 (0x01) 的 COF[2:0] 配置；可输出 32.768 kHz（默认）至 1 Hz，置 111 关闭（输出低电平）。频率表见上文 0x01 Control_2 小节。STOP 位置 1 时会关断低于 8 kHz 的 CLKOUT 输出（输出持续低电平）。

#### 老化 / 校准（Offset）寄存器

即 0x02 Offset，逐位说明见上文 0x02 小节。通过 MODE + OFFSET[6:0]（二位补码）实现精度微调、老化调整与温度补偿。

#### RAM / 其它寄存器

- 0x03 RAM_byte：8 位自由 RAM，逐位说明见上文 0x03 小节。
- 除上述之外手册未定义其它寄存器。手册原文：地址寄存器递增到 0x0A 后“wrap around to address 00h”，即 0x0B 及以上不可寻址。

> 手册依据：PCF85063TP 数据手册 Rev. 4（2015-05-06）；寄存器章节 Section 8（8.1 Registers organization ~ 8.3.7 Register Years，第 5–21 页）、I2C 从地址见 Section 9.5.1 Addressing（第 25 页）；未逐位展开的寄存器仅列在上表中。

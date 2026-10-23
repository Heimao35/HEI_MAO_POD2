/**
 * @file u4_power.h
 * @brief ESP32-U4WDH CHIP_PU（P4 GPIO54）复位时序。
 *
 * 背景：U4 由 AXP2101 ALDO3（3.3 V，额定 300 mA）供电，而 ESP32 蓝牙
 * 射频初始化/发射峰值通常需要 >=500 mA。实板日志显示 U4 在 BTDM_INIT /
 * phy_init 阶段反复 Brownout，随后不断重启，P4 SDIO 读取超时。
 *
 * 本模块只做两件事：
 * 1. P4 启动期间把 U4 保持在复位，避免 U4 蓝牙峰值与 P4 显示/PSRAM/
 *    音频初始化同时发生；P4 主要初始化完成后再释放 CHIP_PU。
 * 2. U4 持续无响应时由链路 worker 脉冲复位，尝试从一次异常状态恢复。
 *
 * 这些措施降低峰值叠加、提供恢复路径，但**不能替代电源整改**：
 * ALDO3 的额定电流仍低于 ESP32 建议值，需要示波器测量 U4 3.3 V 在
 * 蓝牙初始化时的跌落，并考虑加大 bulk 电容或改用更大电流的 DCDC 轨。
 */

#ifndef U4_POWER_H
#define U4_POWER_H

#ifdef __cplusplus
extern "C" {
#endif

/** 配置 GPIO54 为输出并拉低 CHIP_PU（U4 保持复位）。 */
void u4_power_hold_reset(void);

/** 释放 CHIP_PU（输出高，U4 开始启动）。 */
void u4_power_release(void);

/** 拉低 CHIP_PU low_ms 毫秒后再释放；只能在任务上下文调用。 */
void u4_power_pulse_reset(unsigned low_ms);

#ifdef __cplusplus
}
#endif

#endif /* U4_POWER_H */

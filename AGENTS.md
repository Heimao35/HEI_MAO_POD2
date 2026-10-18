# Project memory and maintenance rules

- ESP32-P4 multimedia player: 32 MB PSRAM and 32 MB Flash; ESP-IDF v5.5.4.
- Preserve existing UI layout, gestures and animation behavior unless the user explicitly asks to change them. Add detailed comments to UI-related changes. Charging indicators remain gold; the normal UI palette is white.
- PMIC is AXP2101. SDA = GPIO48, SCL = GPIO47, IRQ = GPIO10; 7-bit I2C address = 0x34. Touch already uses I2C0 on GPIO7/8; the PMIC uses I2C1.
- Read `docs/PMIC_AXP2101.md` before changing power management. It records the user's wiring, datasheet register mappings, power limits, implementation scope and verification status.
- DCDC4 can be set to 1.8 V; ALDO3 can be set to 3.3 V but is rated at only 300 mA, below Espressif's >=500 mA ESP32 supply recommendation. Do not silently treat the LDO's protection current as its output rating.
- Authorized PMIC work includes fuel-gauge/VBUS/charging monitoring, the larger status-bar battery icon, and startup power for DCDC4=1.8 V (DAC) and ALDO3=3.3 V (ESP32-U4WDH). Verify both voltage fields before enabling outputs, preserve unrelated register bits, and do not power-cycle already-configured rails during monitoring retries. Other rails, charging current/voltage, battery model and hardware power-key policy remain unchanged unless requested.
- PMIC IRQ is active-low/open-drain. Keep blocking I2C and file access outside GPIO ISRs and the LVGL lock. Publish UI state while holding the LVGL lock. Unknown battery data must not be replaced by a demo percentage.
- User bench feedback (2026-10-18): DCDC4, ALDO3 and RTC supplies work. No exact RTC voltage or radio-load measurements were provided. AXP2101 soft shutdown is reg0x10 bit0 (bit1 restarts); RTCLDO is retained. Shutdown/sleep control remains unimplemented; see docs/PMIC_AXP2101.md before designing lock/shutdown/wakeup behavior.

# HEI_MAO_POD2 代码编写规范

> **适用范围**：本仓库内的所有固件代码（C）。写新代码、改旧代码、由 agent 生成代码时均须遵守。
> **配套文档**：[hardware-pinmap.md](hardware-pinmap.md)（引脚）、[peripheral-registers.md](peripheral-registers.md)（寄存器）、[architecture.md](architecture.md)（结构与实现思路）。

| 版本 | 更新日期 |
| --- | --- |
| v1.0 | 2026-09-24 |

---

## 1. 事实基础（写代码前须知道）

| 项 | 现状 |
| --- | --- |
| 框架 | ESP-IDF **5.5.4**（P4 v3.x 芯片要求 ≥5.5.3），target `esp32p4` |
| 语言 | C（IDF 默认标准），不使用 C++ |
| UI | **LVGL 9** + `esp_lvgl_port`（`lv_display_t` / `lv_indev_t` 风格 API） |
| 内存 | PSRAM 已启用（200 MHz、XIP from PSRAM）、L2 Cache 256 KB |
| 主任务栈 | `CONFIG_ESP_MAIN_TASK_STACK_SIZE = 10240` |
| 组件 | 本地驱动放 `components/<name>/`，第三方走 IDF Component Manager（`managed_components/` 不入库） |
| 配置 | `sdkconfig.defaults` **入库**；`sdkconfig` 与 `managed_components/` 不入库 |

---

## 2. 命名与格式

- 系统调用/HAL 之外的标识符一律 **snake_case**；宏与常量 **UPPER_SNAKE_CASE**。
- 结构体 `typedef struct { ... } xxx_t;`；枚举 `xxx_enum_t`，成员 `XXX_YYY`。
- 对外符号（跨组件调用）加统一前缀 **`hm_`**（HEI_MAO）；文件内函数一律 `static` 且不带前缀。
- 头文件用 `#pragma once`；**头文件里不定义变量**（要共享就 `extern` + 在 .c 中定义）。
- 缩进 **4 空格**（与现有 `main/main.c` 一致），不使用 Tab；单行不超过 120 列。
- 注释用**中文**（与现有代码一致），原则是解释「为什么」而不是复述「做了什么」；对外 API 在头文件里用 Doxygen 风格（`@brief` / `@param` / `@return`）。
- 注释里出现的**引脚号、寄存器地址必须与两份权威文档一致**；不确定就写「见 xxx.md §n」而不要复述数字。

---

## 3. ESP-IDF 惯用法（强制）

- **错误处理**：初始化路径用 `ESP_ERROR_CHECK()` 或 `ESP_RETURN_ON_ERROR()` / `ESP_GOTO_ON_ERROR()`（与 `main.c` 一致）；**禁止静默丢弃返回值**。资源清理用 `goto err;` 单出口模式（如 `main.c` 的 `app_lcd_init`）。
- **日志**：文件顶部 `static const char *TAG = "audio";`，用 `ESP_LOGI/W/E`；**禁止 `printf`**。TAG 用短小写模块名。
- **可调参数**：放 `Kconfig` / `Kconfig.projbuild`，不要在代码里散落魔法数字；引脚号只在初始化处出现一次，来源必须是 `hardware-pinmap.md`。
- **函数长度**：单个函数建议 ≤ 80 行；超过说明该拆了。
- **禁止**：`delay`/`vTaskDelay` 出现在中断或 LVGL 锁内；在头文件里 `#include` 无关的大头文件。

---

## 4. 并发、中断与 LVGL

- **LVGL 独占**：所有 LVGL API 必须在 `lvgl_port_lock()` / `lvgl_port_unlock()` 之间调用；**其它任务不得直接读写 UI 对象**，只能投递事件/消息。
- **任务**：用 `xTaskCreatePinnedToCore()`，**显式指定核心与栈大小**（如 `4096`/`8192`），并写注释说明该任务干什么、周期多长。
- **模块间通信**：队列或事件循环；**禁止跨任务裸读写全局变量**（同任务内可）。
- **中断**：ISR 只做投递（`xQueueSendFromISR` / `xTaskNotifyFromISR`）；业务逻辑放任务里。ISR 内**禁止** I2C 事务、日志、内存分配、LVGL 调用。
- **超时**：所有阻塞调用都要给超时（`pdMS_TO_TICKS()`），不用 `portMAX_DELAY` 除非确实要永久等待（并注释原因）。

---

## 5. 内存

- **大缓冲**（LVGL 帧缓冲、音频缓冲、曲库/文件缓冲）用 `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`；需要 DMA 的缓冲用 `MALLOC_CAP_DMA`（DMA 缓冲**不能**落在 PSRAM）。
- 分配必须判空，失败要 `ESP_LOGE` 并回退或返回错误，不要直接解引用。
- 频繁分配/释放的路径避免碎片：优先池化或静态分配。
- 长时间运行的功能要能观察到内存水位（沿用已开启的 `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` / LVGL sysmon 等既有手段）。

---

## 6. 外设驱动

- **I2C**：用新驱动（`driver/i2c_master.h`，与现有触摸驱动一致）；每个器件 `i2c_master_bus_add_device()` 一次并复用句柄；地址一律 **7 位**（见寄存器手册的 7/8 位陷阱）。
- **寄存器访问**：写任何寄存器前先查 `peripheral-registers.md`；文档中标注 `（待确认）` 的字段，必须回查原始手册或实测后再用。
- **时序**：复位、上电、配置上传等时序要求（如 BMI270 的 8 KB 配置上传、CS43131 的 20 步初始化）**必须按手册章节实现**并注释出处。
- **新驱动落位**：一个外设 = `components/<name>/`（`include/<name>.h` + `<name>.c` + `CMakeLists.txt`），参照 `components/esp_lcd_touch_cst820/` 的现有结构；**不要往 `main/` 堆驱动代码**（`main/CMakeLists.txt` 需要显式列 SRCS，加文件容易失控）。
- **`main/` 只做编排**：`app_main()` 负责按顺序初始化各组件、创建任务，业务逻辑放组件里。

---

## 7. 文档联动（硬性）

| 改动内容 | 必须同步更新 |
| --- | --- |
| 任何引脚号、外设接线 | [hardware-pinmap.md](hardware-pinmap.md)（并在其 §4 变更记录追加一行） |
| 寄存器用法、新发现的寄存器行为 | [peripheral-registers.md](peripheral-registers.md) |
| 模块划分、实现方案、协议约定 | [architecture.md](architecture.md) |
| 新增代码规范 | 本文件 + 版本表 |

**禁止**在代码注释里写"某引脚是 GPIOxx"这类会过期的硬编码说明而不改文档。

---

## 8. Git

- 提交信息用中文，一行说清做了什么；一个提交只做一件事。
- `build/`、`managed_components/`、`sdkconfig` 等已在 `.gitignore` 中排除，不要 `git add -f` 它们。
- 数据手册等大文件放在**仓库之外**（如 `F:\PROJECT\HEI_MAO_POD_2\DOC\`），不要入库。

---

## 9. 变更记录

| 日期 | 版本 | 变更 |
| --- | --- | --- |
| 2026-09-24 | v1.0 | 首版：命名与格式、IDF 惯用法、并发与 LVGL、内存、外设驱动、文档联动、Git |

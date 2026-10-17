# 封面模糊背景调研（仅调研，未实现）

核实日期：2026-10-17。工程依赖：LVGL 9.4.0、esp_lvgl_port 2.7.2、ESP-IDF 5.5.4；屏幕 460×460、RGB565，当前软件绘制启用，`CONFIG_LV_USE_PPA` 未启用。

## 结论

可以做成“240×240 清晰封面 + 460×460 模糊封面背景”，但当前 LVGL 普通 image/canvas 接口没有直接对任意封面启用高斯模糊的开关。适合在后台切歌时生成一次模糊像素，LVGL 仅负责显示。背景的模糊程度、亮度、过渡方式和布局需由用户确定后再实现；本次没有新增背景对象、处理任务或像素缓冲。

## 当前版本接口

| 能力 | 已核实的接口及限制 |
| --- | --- |
| 缩放 | `lv_image_set_scale()`；256 表示原尺寸。240→460 的理论值为 490.667，491 约为 460.3 像素；有定点精度和取整，若必须精确输出 460×460，可预先软件重采样至该尺寸。 |
| 抗锯齿 | `lv_image_set_antialias()` 用于缩放/旋转质量，不等于可调半径的高斯模糊。 |
| Canvas 模糊 | v9.4.0 的 `lv_canvas.h` 未提供旧版本资料中的 `lv_canvas_blur_hor()` / `lv_canvas_blur_ver()`。不能直接照搬这些旧接口。 |
| 其他 blur 搜索结果 | 源码中有 ThorVG 场景 GaussianBlur、VG Lite filter 等特定后端接口；不代表当前软件绘制的普通图片控件可直接使用。矩形阴影模糊也不会模糊封面像素。 |
| PPA | IDF 5.5.4 的 PPA 提供 SRM（缩放/旋转/镜像）、BLEND、FILL；公开操作枚举没有高斯模糊。可评估加速缩放，模糊仍需另行处理。 |
| PPA 缩放精度 | IDF 5.5.4 文档说明 scale_x/scale_y 按 1/16 截断，SRM 使用双线性缩放，且输入输出必须是不同缓冲。不能假定任意比例都恰好得到 460×460。 |

## 建议评估的处理路径

1. 后台解码当前 240×240 封面，保留清晰封面的独立显示缓冲。
2. 另外降采样到例如 60×60；先以该分辨率做按颜色通道分离的横向/纵向盒式模糊，多轮可近似高斯模糊。分辨率和半径仅为待实测起点，不是已确认的视觉方案。
3. 重采样为精确 460×460 RGB565，按用户后续选择决定是否暗化；换歌时生成并缓存，不在 LVGL 每帧做全屏卷积。
4. 在 LVGL 锁内发布已完成的背景；后台不能改写正在显示的像素。背景放在页面后层，禁用点击/滚动，不覆盖原封面、歌词和控制台的输入区域。
5. 如果后续需要交叉淡入或背景跟随封面滑动，再单独确认动画行为及额外缓冲预算。

这是建议的实现方向，尚未在设备上测量处理耗时、刷新率和内存带宽。PPA 是否值得引入需结合当前显示链路测试；本次不修改配置。

## 增量像素内存预算

| 缓冲 | 原始像素字节（不含对齐/临时区） |
| --- | ---: |
| 单个 460×460 RGB565 背景 | 423,200 字节，约 413.3 KiB |
| 两个背景缓冲（异步交接或淡入） | 846,400 字节，约 826.6 KiB |
| 单个 60×60 RGB565 中间图 | 7,200 字节，约 7.0 KiB |

横纵模糊通常还需中间缓冲或行缓冲。以上不包含现有 JPEG 双缓冲、LCD/LVGL 帧缓冲、压缩 JPEG、音频解码/PCM、任务栈或 DMA 对齐开销；不能把 32 MB PSRAM 全部视为可供背景使用的空闲内存。可以仅在切歌期间持有第二个背景缓冲，最终策略需与后续音频资源预算一起确定。

## 核实来源

已在线读取以下官方固定标签原文，并与工程安装的对应头文件/IDF 文档核对，避免混用 latest 文档：

- [LVGL v9.4.0 image API](https://github.com/lvgl/lvgl/blob/v9.4.0/src/widgets/image/lv_image.h)：`lv_image_set_scale`、`lv_image_set_antialias`。
- [LVGL v9.4.0 canvas API](https://github.com/lvgl/lvgl/blob/v9.4.0/src/widgets/canvas/lv_canvas.h)：当前 Canvas 接口清单。
- [ESP-IDF v5.5.4 PPA 文档源码](https://github.com/espressif/esp-idf/blob/v5.5.4/docs/en/api-reference/peripherals/ppa.rst)：Perform PPA Operations → SRM、精度、缓冲约束。
- [ESP-IDF v5.5.4 PPA API](https://github.com/espressif/esp-idf/blob/v5.5.4/components/esp_driver_ppa/include/driver/ppa.h)：`ppa_operation_t` 操作枚举及配置。

另通过乐鑫 Documentation MCP 检索 PPA 官方文档/TRM交叉确认功能范围；涉及当前工程行为时以 5.5.4 固定标签和本地源码为准。

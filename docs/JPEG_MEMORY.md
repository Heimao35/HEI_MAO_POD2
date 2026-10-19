# 大曲库与锁屏 JPEG 内存问题（2026-10-19）

## 现象与证据

用户反馈进入锁屏准备播放 MJPEG 时重启；签出旧提交也能复现。
进一步测试表明 Music 中放入较多歌曲文件夹会触发，仅保留四首时恢复。
串口首先报告 `jpeg_new_decoder_engine: no memory for jpeg decode rxlink`，
随后在 `jpeg_release_codec_handle -> jpeg_del_decoder_engine ->
jpeg_new_decoder_engine -> play_mjpeg` 出现 Load access fault。
这说明本次记录中的重启发生于 JPEG 构造失败清理；不能单凭该日志断言
SD 文件系统损坏、整个 PSRAM 耗尽或存在内存泄漏。

## 随歌曲数量增长的内存

`music_library.c` 的完整歌曲索引保存在 SD 上，并非把所有音频文件载入 RAM。
但 `ui_music_player_set_library()` 会为每首歌创建实际条目、图标、标题和样式，
还有条目指针数组。旧配置使用 LVGL CLIB 分配器；工程的普通 malloc
以 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 优先把小块分配放入内部 SRAM。
这些对象与 JPEG 的内部 DMA 描述符竞争内存，符合用户报告的歌曲数量相关性。
尚未取得板上修复前后的内部堆/最大连续块测量，不能声称已经量化全部压力来源。

当前改为 `CONFIG_LV_USE_CUSTOM_MALLOC=y`，由 `main/ui/ui_memory.c` 实现
LVGL 自定义内存接口。普通 LVGL malloc/realloc 固定请求
`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`，不回退内部 SRAM；条目指针数组也改用
`lv_calloc/lv_free`。显示、I2S、JPEG 的专用 DMA 缓冲继续使用各自原有分配接口。
LVGL 的内存监视器报告共享 PSRAM 堆，包含字体/封面等其他使用者，并非 LVGL 独占统计。

根 CMake 将该文件加入 LVGL 的构建目标，使自定义接口和调用方处于同一个静态库，
避免把实现仅放在 main 库时因静态库链接顺序导致接口未被抽取。
没有改动 managed_components 中的源文件。

列表仍为完整实际条目，布局、图标、标题、滚动、选择、手势与动画保持原实现。
**本次不是列表虚拟化：列表总内存仍随歌曲数量增长，只是转移到 PSRAM。**
32 MB PSRAM 同时承担字体、封面、显示等用途，不能认为曲库容量无限。

## ESP-IDF 5.5.4 清理缺陷与构建补丁

本地固定版本 `components/esp_driver_jpeg/jpeg_decode.c` 的构造顺序为：
分配 engine、rxlink、txlink、header，再取得共享 `codec_base`。
前几步失败会进入 `jpeg_del_decoder_engine()`，但原析构函数无条件调用
`jpeg_release_codec_handle(decoder_engine->codec_base)`。
`jpeg_common.c` 在共享 codec 已存在时会先减少引用；若封面解码器是唯一所有者，
此时释放一个 NULL 的未取得引用会错误影响现有所有者，并可能解引用 NULL。
这与用户日志的调用栈吻合，已用固定版本源代码核对。

`scripts/jpeg_cleanup.cmake` 调用 `scripts/patch_jpeg_cleanup.py`，生成
`build/player_jpeg/jpeg_decode.c` 替换构建目标中的原始源文件。
仅在 `codec_base` 非 NULL 时释放引用；其他清理和正常解码行为不变。
SDK 安装目录不作修改。补丁固定到 IDF 5.5.4，原始锚点变化或 SDK 升级时
构建会停止，必须人工核对，不可默默沿用。

分配失败后，既有锁屏代码释放文件缓冲并返回；下一次进入锁屏可以重新尝试。
这条安全返回路径不保证内存不足时还能播放动画，但必须避免因此重启。

## 验证与后续板测

- ESP-IDF 5.5.4 固件构建通过，应用镜像 `0x136920` 字节。
- 主机 CTest 17/17 通过。`ui_memory` 使用实际 LVGL 和播放器代码，验证
  所有自定义分配请求 PSRAM，并依次创建 512/1024/1536 条实际歌曲行，检查
  首尾条目、最后一首点击、原有滚动与播放器手势回归；主机堆不能证明 P4 性能。
- `jpeg_cleanup` 编译从生成驱动中提取的实际析构函数，模拟取得 codec 前
  rxlink/txlink/header 分配失败，确认已有封面所有者引用不变；取得引用后的
  清理仍正确释放全部资源。同一测试替换为未修复的原始析构函数时，会在
  释放引用的所有权断言处失败，确认测试能识别原缺陷。该测试不运行完整硬件构造过程。
- `lockscreen` 增加引擎创建失败后 SD/UI 锁释放、工作状态空闲、下次进入可重试检查。
- 主机 JPEG 用例要求先配置固件，以生成固定版本析构函数测试 include；当前测试
  CMake 仅在该 include 存在时注册该用例。请核对 CTest 总数，不要把缺项当作通过。

板上仍需用原先能触发问题的歌曲数量，交替播放 MP3/FLAC、打开歌曲列表、
多次进入锁屏/熄屏/唤醒，确认不再出现 rxlink 分配失败和重启，并检查 UI/音频流畅度。
如还有分配失败，需记录内部 DMA 的总空闲量和最大连续块，继续区分耗尽与碎片。
不要仅根据主机用例声称大曲库、所有文件或长期运行已经通过实机验证。

用户后续实机反馈（2026-10-19）：问题已修复，授权提交。本反馈不包含内部堆测量或曲库容量上限测定。

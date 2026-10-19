# 锁屏/开机动画的 Flash 持久保存

适用：ESP32-P4，32 MiB Flash / 32 MiB PSRAM，ESP-IDF v5.5.4。
实现：`main/lock_animation.c/.h`；启动接入：`main/main.c`；显示接入：
`main/ui/ui_lockscreen.c`。原有页面布局、滑入/滑出、唤醒重播和24fps逻辑保留。

## 用户如何更换动画

1. 正常关机，把 SD 卡接到电脑。
2. 将新动画替换到 SD 卡根目录下的：
   `HEI_MAO_POD_SYS/MEDIA/lock_scr/000.mjpeg`。
3. 插回 SD 卡，重新开机。系统在音频任务启动前检查源文件，内容与当前
   Flash 动画不同且通过校验时自动导入；首次导入/更换可能增加开机等待。
4. 导入成功后所有锁屏、熄屏唤醒重播直接使用只读缓存；之后拔卡、换成
   没有此文件的卡，或下次无卡开机，也可使用保存在 Flash 中的动画。

不需要为每次更换动画重新烧录固件。运行中修改源文件或重新插卡不导入，
必须重新开机；这样不会在音乐播放过程中做大文件读取或 Flash 擦写。
换卡后，如果该路径有另一份有效动画，下一次开机会将其视为更新源。
移除源文件不会清除已经保存的动画。

要求：MJPEG 裸串流，每帧460×460，8bit三分量baseline JPEG，支持444/422/420
采样，按现有24fps播放，整个文件不超过4 MiB（4,194,304字节）。
不接受progressive JPEG、其他分辨率、截断帧等。帧结构校验不会替代真正的
熵编码解码，故不能保证所有结构合法的输入都能被硬件正确解码。

串口关键日志（tag=`lock_animation`）：

- `Imported animation: slot=..., sequence=..., bytes=..., frames=...`：写入并读回校验成功。
- `Animation unchanged; no Flash write`：源文件长度/CRC与当前副本一致，不擦写。
- `Loaded Flash animation: slot=...`：无有效更新源时从Flash恢复。
- `Animation import unavailable ...`：更新未采用，尝试沿用已验证的Flash副本。
- `Missing lockanim partition; flash the new partition table`：仍使用旧分区表。

## 首次烧录与分区兼容

本次固件增加了分区，需要使用 `idf.py flash` 同时更新应用和分区表；
仅烧录应用无法建立新分区。标准flash清单不包含lockanim镜像，后续普通固件
更新不会主动擦除这个分区；全片擦除或显式擦除该区域则会删除持久动画。

新增分区：`lockanim,data,0x40,0x1110000,0x802000`，末尾`0x1912000`，
位于32 MiB范围内。NVS、PHY、factory、storage和fontfs的地址及容量均不变。
无需迁移这些既有分区；本模块不执行格式化/全片擦除，擦写范围限定在新增动画分区内。

双槽各`0x401000`字节：首4 KiB为头部扇区，其后最大4 MiB为MJPEG压缩数据。
总占用8 MiB+8 KiB。头部实际64字节，以明确的小端整数序列化，包含格式版本、
64bit序号、长度、CRC32、帧数、尺寸/帧率和头部CRC，不直接持久化C结构体。
此实现仅启用当前未加密、可写、4 KiB擦除粒度的数据分区；没有擅自引入加密策略。

## 启动、更新和错误处理

`lock_animation_init()`只由app_main调用一次，在SD初始挂载之后、音频任务和
屏幕电源任务之前、LVGL锁外执行。步骤为：

1. 读取两槽头部并用固定16 KiB PSRAM工作缓冲校验payload CRC。
2. 从有效槽中选择序号最大的副本。Flash读取报错时禁止更新，避免把暂时
   无法读取的有效副本误当作空槽覆盖。
3. 启动时SD已挂载则尝试读取源文件；整个FILE生命周期由已有SD锁保护，
   关闭并释放SD锁后才做结构检查和Flash更新。
4. 新文件先完成结构校验；若长度/CRC与当前副本相同，则不擦写Flash。
5. 更新只写非活动槽：先擦头部扇区，再分批擦写payload，读回核对CRC，
   最后写入带CRC的头部并读回核对。旧活动槽始终保留。
6. 无卡、缺文件、格式错误、读写失败等情况下，尝试从有效Flash槽加载。
   两槽均有效时，新副本加载失败还可尝试上一副本；没有可用数据则仅显示
   原锁屏时间/日期与手势，动画缺失不阻止系统和音频继续初始化。
7. 发布只读PSRAM缓存，生命周期直到关机。锁屏worker每次position=0重播，
   `lock_animation_next_frame()`不分配内存，不读SD/Flash，且只读已发布缓存。

本模块显式分配的单份压缩文件上限4 MiB，导入时只有一份完整文件加16 KiB工作缓冲；不同时
把新旧两份文件加载到PSRAM，不保存全部解码画面。发布后释放工作缓冲，
常驻压缩数据占用等于实际动画文件大小；既有画布和JPEG staging buffer保留。

开机动画不再由曲库扫描成功触发，而由资源就绪触发。因此空曲库、SD扫描失败
或无SD不再影响已经持久保存的动画，扫描/换卡也不会再次重播启动动画。
如果SD启动挂载失败但后来才恢复，本轮不重新检查源文件，下一次正常开机再导入。

A/B和CRC是应用恢复协议：在受测模型中，未完整提交的新槽会被忽略，上一槽
保留。它不构成对任意掉电、电源越限、Flash硬件故障的无条件可靠性承诺。

## 验证

- ESP-IDF v5.5.4固件构建通过，应用镜像0x137410字节；分区生成工具验证新旧范围无重叠、均在32 MiB内。
- 主机CTest18/18通过。实际生产模块配合NOR Flash模型，检查首次导入、
  相同文件零擦写、同大小不同内容替换、无SD重启、最新槽损坏回退、每个
  擦写调用中断、读回损坏、Flash读取/分区/分配错误、4 MiB边界及超限文件。
- 用真实FFmpeg生成的两帧460×460 MJPEG完成生产模块导入/逐帧遍历；FFmpeg
  独立解码验证该夹具。其他合成JPEG覆盖APP内嵌SOI/EOI、转义、重启标记和
  截断/progressive/尺寸错误，这些不代替ESP32硬件解码测试。
- 真实LVGL锁屏回归检查无卡缓存重播、快速取消、JPEG创建失败、下一次重试，
  重播路径零SD锁调用；既有播放器、音频、音量、关机/唤醒测试继续通过。

待上板：首次烧录后的原始动画导入；音乐播放时连续锁屏/熄屏/唤醒不再短暂停音；
替换同大小动画并重启；无SD冷启动播放已保存动画；模拟更新中断恢复；最大
文件和大曲库下PSRAM余量/体验。主机故障模型不等于真实物理断电试验。

## 版本依据

实际核对本机Espressif SDK源码，`git describe --tags --always --dirty`返回
`v5.5.4`，并核对以下同版本原始声明/实现：

- `components/esp_partition/include/esp_partition.h`：
  `esp_partition_find_first/read/write/erase_range`，目标分区范围限制、
  写前擦除及擦除偏移/长度按`partition->erase_size`对齐。
- `components/esp_hw_support/include/esp_crc.h`与
  `components/esp_rom/include/esp_rom_crc.h`：CRC32初值0、后续块传入上次结果。
- `components/partition_table/gen_esp32part.py`：用于生成并反解析实际分区表。

官方检索返回了master版示例，其API用途仅作为检索线索；本实现以本机固定
v5.5.4声明为准，没有采用新版mmap参数或假设Flash可直接作为JPEG DMA源。

用户后续实机反馈（2026-10-19）：测试没有问题，效果良好，授权提交。未提供逐项测试记录，不能据此视为所有断电/最大文件场景已验证。

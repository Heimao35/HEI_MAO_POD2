/**
 * @file ebook.h
 * @brief 电子书后台：扫描 BOOKS、解码封面、读取/分页 UTF-8 TXT。
 *
 * 目录约定（每本书一个文件夹）：
 *   HEI_MAO_POD_SYS/BOOKS/<书名>/<任意名字>.txt
 *   HEI_MAO_POD_SYS/BOOKS/<书名>/<任意名字>.jpg|.jpeg   （可选）
 * 文件夹名就是书名；书名和正文必须用 18 号全字库显示。
 *
 * 设计边界：
 * - 所有 SD/JPEG/分页工作都在一个 worker 中完成；UI 回调由 worker 在
 *   LVGL 锁内调用，回调里只更新控件。
 * - 封面缓冲、JPEG 输入/输出和页文本缓冲固定复用，不按书本数量增长。
 * - 打开一本书时把 TXT 一次性读入 PSRAM（有硬上限和可用内存检查），
 *   在 PSRAM 中完成换行归一化和分页；之后翻页不再访问 SD，避免拖动/
 *   翻页时和音频抢 SD 锁。超大文件明确报错，不静默截断。
 * - 只支持 UTF-8 TXT（可带 BOM、CRLF/CR）。非 UTF-8 明确报错；本项目
 *   没有内置 GBK/GB18030 转换表，不能把乱码当正文显示。
 */
#ifndef EBOOK_H
#define EBOOK_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EBOOK_MAX_BOOKS      128U   /**< 最多索引的书本数；超出标记 truncated */
#define EBOOK_TITLE_MAX      96U    /**< 文件夹名（书名）字节上限 */
#define EBOOK_COVER_W        140U   /**< 书库封面显示宽度 */
#define EBOOK_COVER_H        200U   /**< 书库封面显示高度 */
#define EBOOK_HOME_W         104U   /**< 首页卡片封面宽度 */
#define EBOOK_HOME_H         148U   /**< 首页卡片封面高度 */
#define EBOOK_COVER_SLOTS    8U     /**< 书库同时保留的封面缓冲槽数 */
#define EBOOK_MAX_BYTES      (8U * 1024U * 1024U) /**< 单本 TXT 硬上限 */
#define EBOOK_MAX_PAGES      32768U /**< 单本最多页数，限制页索引内存 */

/* 正文排版参数：后台分页和 UI 标签必须使用同一组值，否则页尾会重排。 */
#define EBOOK_TEXT_W         420    /**< 正文可用宽度（460 - 2*20） */
#define EBOOK_TEXT_H         412    /**< 正文可用高度（460 - 2*24） */
#define EBOOK_LINE_SPACE     4      /**< 行间距 */

typedef enum {
    EBOOK_SCAN_OK = 0,
    EBOOK_SCAN_NO_CARD,
    EBOOK_SCAN_BUSY,
    EBOOK_SCAN_ERROR,
} ebook_scan_result_t;

typedef enum {
    EBOOK_OPEN_OK = 0,
    EBOOK_OPEN_NO_CARD,
    EBOOK_OPEN_BUSY,
    EBOOK_OPEN_UTF8,       /**< 不是合法 UTF-8 */
    EBOOK_OPEN_TOO_LARGE,  /**< 超过 EBOOK_MAX_BYTES 或内存不足 */
    EBOOK_OPEN_READ,       /**< 打开/读取/空文件 */
    EBOOK_OPEN_TOO_MANY_PAGES,
} ebook_open_result_t;

/**
 * @brief UI 回调集合；全部由 worker 在 LVGL 锁内调用。
 *
 * 像素/文本指针只在回调返回前有效；UI 必须用 token/generation 丢弃过期
 * 结果，并在回调内把需要保留的内容复制/挂到控件上。
 */
typedef struct {
    /** 扫描结果（按书名排序；失败时 count=0 以清空旧列表）。 */
    void (*library)(uint32_t count, bool truncated, uint32_t generation, void *ctx);
    /** 两个目录都处理完后调用一次，用于结束加载提示。 */
    void (*scan_done)(ebook_scan_result_t result, uint32_t generation, void *ctx);
    /** 首页卡片封面：EBOOK_HOME_W x EBOOK_HOME_H RGB565。 */
    void (*home_cover)(uint32_t book, const uint8_t *pixels,
                       uint16_t w, uint16_t h, uint32_t generation,
                       bool ok, void *ctx);
    /** 书库封面：EBOOK_COVER_W x EBOOK_COVER_H RGB565；slot 0..7。 */
    void (*cover)(uint32_t slot, uint32_t book, const uint8_t *pixels,
                  uint16_t w, uint16_t h, uint32_t token, bool ok, void *ctx);
    /**
     * @brief 打开一本书并完成分页。
     *
     * progress_key 由书名、TXT 大小、修改时间和排版版本共同哈希；页码
     * 只在同一 key 下有效。UI 用它读取/更新 NVS 中的阅读进度。
     */
    void (*opened)(uint32_t book, uint32_t pages, uint64_t progress_key,
                   uint32_t token, ebook_open_result_t result, void *ctx);
    /** 一页文本，NUL 结尾；UI 在回调内复制给 label。 */
    void (*page)(uint32_t page, const char *text, uint32_t token,
                 bool ok, void *ctx);
} ebook_ops_t;

/** @brief 创建请求锁；不创建任务、不访问 SD、不预分配书本/封面缓冲。 */
esp_err_t ebook_init(void);

/*
 * 方案 A+（2026-11-14）：worker 首次进入电子书模式时创建，之后常驻。
 * 它的 12 KiB 栈由 worker_task.h 放在 PSRAM，内部堆不再参与任务栈分配，
 * 因此不会因为 12/16 KiB 栈反复创建/销毁而产生碎片。
 *
 * worker 若将来需要真正退出，必须用 worker_task_exit()
 * （vTaskDeleteWithCaps）；不能直接用 vTaskDelete()，否则 PSRAM 栈泄漏。
 * request_worker_exit() 保留为 UI 兼容入口，当前是有意空实现；离开模式
 * 时的 close_reader() 仍会释放阅读正文。
 */
void ebook_request_worker_exit(void);
esp_err_t ebook_ensure_started(void);

/** @brief 注册 UI 回调；可在 init 之前调用，只保存指针。 */
void ebook_set_ops(const ebook_ops_t *ops, void *ctx);

/** @brief 请求重新扫描 BOOKS；重复请求合并。 */
void ebook_request_scan(void);

uint32_t ebook_get_count(void);
bool ebook_has_cover(uint32_t index);
/** @brief 复制书名；只在 library 回调之后调用。 */
bool ebook_get_title(uint32_t index, char *out, size_t cap);

/** @brief 请求首页卡片封面（第一本书）。 */
void ebook_request_home_cover(uint32_t book, uint32_t generation, uint32_t token);
/** @brief 请求一个书库封面槽。 */
void ebook_request_cover(uint32_t book, uint32_t slot, uint32_t token);

/** @brief 打开并分页一本书；会释放上一本。 */
void ebook_open(uint32_t book, uint32_t token);
/** @brief 请求当前打开书的某一页（0 基）。 */
void ebook_request_page(uint32_t page, uint32_t token);
/** @brief 关闭当前书并释放正文/页索引；迟到回调由 token 自动丢弃。 */
void ebook_close(void);

#ifdef __cplusplus
}
#endif

#endif /* EBOOK_H */

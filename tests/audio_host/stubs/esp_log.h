#pragma once
#include <stdarg.h>
static inline void audio_host_log(const char *tag,const char *fmt,...) {(void)tag;(void)fmt;}
#define ESP_LOGI audio_host_log
#define ESP_LOGW audio_host_log
#define ESP_LOGE audio_host_log

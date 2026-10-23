// SPDX-License-Identifier: GPL-2.0-or-later
// Android logging API boundary for common runtime sources on Apple hosts.
#pragma once
#include <stdarg.h>
enum android_LogPriority {
    ANDROID_LOG_UNKNOWN=0, ANDROID_LOG_DEFAULT=1, ANDROID_LOG_VERBOSE=2,
    ANDROID_LOG_DEBUG=3, ANDROID_LOG_INFO=4, ANDROID_LOG_WARN=5,
    ANDROID_LOG_ERROR=6, ANDROID_LOG_FATAL=7, ANDROID_LOG_SILENT=8
};
#ifdef __cplusplus
extern "C" {
#endif
int __android_log_print(int priority,const char* tag,const char* format,...);
int __android_log_vprint(int priority,const char* tag,const char* format,va_list args);
int __android_log_write(int priority,const char* tag,const char* text);
#ifdef __cplusplus
}
#endif

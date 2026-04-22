/************************************************************************************
  If not stated otherwise in this file or this component's LICENSE file the
  following copyright and licenses apply:

  Copyright 2018 RDK Management

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 **************************************************************************/

#ifndef LQ_LOG_H
#define LQ_LOG_H

/*
 * Local logging shim for linkquality_stats.
 * Same format as OneWifi's wifi_util_error_print / wifi_util_info_print
 * but standalone — writes to /rdklogs/logs/wifiLinkQuality.txt.
 * No dependency on onewifi's wifi_util.c.
 */

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>

#define LQ_LOG_FILE "/rdklogs/logs/wifiLinkQuality.txt"

/* Module tags — re-use onewifi enum names so call sites look familiar */
typedef enum {
    WIFI_APPS,
    WIFI_CTRL,
    WIFI_LQ
} lq_dbg_type_t;

static inline const char *lq_module_str(lq_dbg_type_t m)
{
    switch (m) {
    case WIFI_APPS: return "wifiApps";
    case WIFI_CTRL: return "wifiCtrl";
    case WIFI_LQ:   return "wifiLinkQuality";
    default:        return "wifiLQ";
    }
}

static inline void lq_log_print(const char *level_tag, lq_dbg_type_t module,
                                  const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static inline void lq_log_print(const char *level_tag, lq_dbg_type_t module,
                                  const char *fmt, ...)
{
    FILE *fp = fopen(LQ_LOG_FILE, "a+");
    if (!fp) {
        fp = stderr;
    }

    struct timeval tv;
    struct tm tm_info;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm_info);

    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%y%m%d-%H:%M:%S", &tm_info);

    fprintf(fp, "[linkquality_stats] %s.%06ld %s [%s] ",
            timebuf, (long)tv.tv_usec, level_tag, lq_module_str(module));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fflush(fp);
    if (fp != stderr) {
        fclose(fp);
    }
}

#define wifi_util_error_print(module, fmt, ...) \
    lq_log_print("<E>", module, fmt, ##__VA_ARGS__)

#define wifi_util_info_print(module, fmt, ...) \
    lq_log_print("<I>", module, fmt, ##__VA_ARGS__)

#define wifi_util_dbg_print(module, fmt, ...) \
    lq_log_print("<D>", module, fmt, ##__VA_ARGS__)

#endif /* LQ_LOG_H */

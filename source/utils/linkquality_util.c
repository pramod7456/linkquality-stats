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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <fcntl.h>
#include "linkquality_util.h"
#include <netinet/in.h>
#include <time.h>
#include <openssl/sha.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/if_packet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <ifaddrs.h>

char *get_formatted_time(char *time)
{
    struct tm *tm_info;
    struct timeval tv_now;
    char tmp[128];

    gettimeofday(&tv_now, NULL);
    tm_info = (struct tm *)localtime(&tv_now.tv_sec);

    strftime(tmp, 128, "%y%m%d-%T", tm_info);

    snprintf(time, 128, "%s.%06lld", tmp, (long long)tv_now.tv_usec);
    return time;
}

void lq_util_print(linkq_log_level_t level, linkq_dbg_type_t module, char *format, ...)
{       
    char buff[256] = { 0 };
    va_list list;
    FILE *fpg = NULL;
    char filename_dbg_enable[64];
    char module_filename[32];
    char filename[100];

    switch (module) {
    case LQ_LQTY: {
        snprintf(filename_dbg_enable, sizeof(filename_dbg_enable), LOG_PATH_PREFIX "LINKQTYDbg");
        snprintf(module_filename, sizeof(module_filename), "linkquality");
        break;
    }
    case LQ_CAFF: {
        snprintf(filename_dbg_enable, sizeof(filename_dbg_enable), LOG_PATH_PREFIX "CAFFDbg");
        snprintf(module_filename, sizeof(module_filename), "affinity");
        break;
    }
      default:
        return;
    }
  if ((access(filename_dbg_enable, R_OK)) == 0) {
        snprintf(filename, sizeof(filename), "/tmp/%s", module_filename);
        fpg = fopen(filename, "a+");
        if (fpg == NULL) {
            return;
        }
    } else {
        switch (level) {
        case LQ_LOG_LVL_INFO:
        case LQ_LOG_LVL_ERROR:
            snprintf(filename, sizeof(filename), "/rdklogs/logs/%s.txt", module_filename);
            fpg = fopen(filename, "a+");
            if (fpg == NULL) {
                return;
            }
            break;
        case LQ_LOG_LVL_DEBUG:
        default:
            return;
        }
    }
      static const char *level_marker[LQ_LOG_LVL_MAX] = {
            [LQ_LOG_LVL_DEBUG] = "<D>",
            [LQ_LOG_LVL_INFO] = "<I>",
            [LQ_LOG_LVL_ERROR] = "<E>",
        };
        if (level < LQ_LOG_LVL_MAX)
            snprintf(&buff[strlen(buff)], 256 - strlen(buff), "%s ", level_marker[level]);

        fprintf(fpg, "%s ", buff);

    va_start(list, format);
    vfprintf(fpg, format, list);
    va_end(list);

    fflush(fpg);
    fclose(fpg);

}

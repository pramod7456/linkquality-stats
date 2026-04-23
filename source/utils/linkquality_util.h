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
 
 #ifndef _LINKQUALITY_UTIL_H_
#define _LINKQUALITY_UTIL_H_
  
#include "wifi_base.h"
#include "wifi_hal.h"
#include "wifi_webconfig.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include "bus.h"
#include "ccsp.h"
 
#define MAX_NAME_LEN 32
#define LOG_PATH_PREFIX "/nvram/"

 #ifdef __cplusplus
 extern "C" {
 #endif
typedef enum {
    LQ_LQTY,
    LQ_CAFF,
} linkq_dbg_type_t;

typedef enum {
    LQ_LOG_LVL_DEBUG,
    LQ_LOG_LVL_INFO,
    LQ_LOG_LVL_ERROR,
    LQ_LOG_LVL_MAX
} linkq_log_level_t;

void lq_util_print(linkq_log_level_t level, linkq_dbg_type_t module, char *format, ...);
char *get_formatted_time(char *time);

#define lq_util_dbg_print(module, format, ...) \
    lq_util_print(LQ_LOG_LVL_DEBUG, module, format, ##__VA_ARGS__)
#define lq_util_info_print(module, format, ...) \
    lq_util_print(LQ_LOG_LVL_INFO, module, format, ##__VA_ARGS__)
#define lq_util_error_print(module, format, ...) \
    lq_util_print(LQ_LOG_LVL_ERROR, module, format, ##__VA_ARGS__)


 #ifdef __cplusplus
 }
 #endif
 
 #endif

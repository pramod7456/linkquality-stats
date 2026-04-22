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

/*
 * linkquality_stats – standalone process that receives link-quality events
 * from OneWifi via rbus and drives the quality-manager (qmgr) library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include "linkquality_stats_rbus.h"
#include "lq_log.h"
#include "run_qmgr.h"

#define COMPONENT_NAME "linkquality_stats"

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    wifi_util_info_print(WIFI_LQ, "%s:%d starting linkquality_stats process\n", __func__, __LINE__);

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    if (lq_stats_rbus_init() != 0) {
        wifi_util_error_print(WIFI_LQ, "%s:%d rbus init failed\n", __func__, __LINE__);
        return EXIT_FAILURE;
    }

    wifi_util_info_print(WIFI_LQ, "%s:%d rbus subscriptions active, entering main loop\n", __func__, __LINE__);

    wifi_util_info_print(WIFI_LQ, "%s:%d === linkquality_stats subscribed events ===\n", __func__, __LINE__);
    wifi_util_info_print(WIFI_LQ, "%s:%d   [rbus] Device.WiFi.LinkQuality.PeriodicStats   -> on_periodic_stats  -> add_stats_metrics()\n", __func__, __LINE__);
    wifi_util_info_print(WIFI_LQ, "%s:%d   [rbus] Device.WiFi.LinkQuality.RapidDisconnect -> on_rapid_disconnect -> disconnect_link_stats()\n", __func__, __LINE__);
    wifi_util_info_print(WIFI_LQ, "%s:%d   [rbus] Device.WiFi.LinkQuality.Remove          -> on_remove           -> remove_link_stats()\n", __func__, __LINE__);
    wifi_util_info_print(WIFI_LQ, "%s:%d   [rbus] Device.WiFi.LinkQuality.HalIndication   -> on_hal_indication   -> periodic_caffinity_stats_update()\n", __func__, __LINE__);
    wifi_util_info_print(WIFI_LQ, "%s:%d         HAL sub_types: auth_req, auth_rsp, assoc_req, assoc_rsp, reassoc_req, reassoc_rsp, disassoc, deauth\n", __func__, __LINE__);
    wifi_util_info_print(WIFI_LQ, "%s:%d   [rbus] Device.WiFi.LinkQuality.Start           -> on_start            -> start_link_metrics()\n", __func__, __LINE__);
    wifi_util_info_print(WIFI_LQ, "%s:%d   [rbus] Device.WiFi.LinkQuality.Stop            -> on_stop             -> stop_link_metrics()\n", __func__, __LINE__);
    wifi_util_info_print(WIFI_LQ, "%s:%d   [rbus] Device.WiFi.LinkQuality.GwDiscovery     -> on_gw_discovery     -> (TODO)\n", __func__, __LINE__);
    wifi_util_info_print(WIFI_LQ, "%s:%d === onewifi sources that publish to us ===\n", __func__, __LINE__);
    wifi_util_info_print(WIFI_LQ, "%s:%d   [onewifi] wifi_stats_assoc_client.c  -> PeriodicStats, RapidDisconnect, Remove\n", __func__, __LINE__);
    wifi_util_info_print(WIFI_LQ, "%s:%d   [onewifi] wifi_linkquality.c         -> HalIndication (auth/assoc/disassoc), Start, Stop, GwDiscovery\n", __func__, __LINE__);
    wifi_util_info_print(WIFI_LQ, "%s:%d ==========================================\n", __func__, __LINE__);
    
    /* Event-driven: rbus dispatches callbacks on its own threads.
     * We just keep the process alive until signalled. */
    while (g_running) {
        pause();   /* sleep until signal */
    }

    wifi_util_info_print(WIFI_LQ, "%s:%d shutting down linkquality_stats\n", __func__, __LINE__);
    lq_stats_rbus_deinit();

    return EXIT_SUCCESS;
}

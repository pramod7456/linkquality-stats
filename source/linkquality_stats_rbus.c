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
 * linkquality_stats rbus subscriber – receives raw-byte events from OneWifi
 * and dispatches them to the quality-manager library (libwifi_quality_manager).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rbus/rbus.h>
#include "linkquality_stats_rbus.h"
#include "lq_rbus_events.h"
#include "lq_log.h"
#include "run_qmgr.h"

#define LQ_STATS_COMPONENT  "linkquality_stats"
#define NUM_LQ_EVENTS       7

static rbusHandle_t g_rbus_handle = NULL;
static rbusEventSubscription_t g_subs[NUM_LQ_EVENTS];

/* ------------------------------------------------------------------ */
/*  Event handler helpers                                              */
/* ------------------------------------------------------------------ */

static const lq_rbus_event_msg_t *validate_msg(const rbusEventRawData_t *event,
                                                 const char *handler_name)
{
    if (!event || !event->rawData || event->rawDataLen < (int)sizeof(lq_rbus_event_msg_t)) {
        wifi_util_error_print(WIFI_LQ, "%s:%d %s: invalid event data from onewifi\n",
                              __func__, __LINE__, handler_name);
        return NULL;
    }
    const lq_rbus_event_msg_t *msg = (const lq_rbus_event_msg_t *)event->rawData;
    size_t expected = LQ_RBUS_MSG_SIZE(msg->num_entries);
    if ((size_t)event->rawDataLen < expected) {
        wifi_util_error_print(WIFI_LQ, "%s:%d %s: truncated message (got %d, need %zu for %u entries)\n",
                              __func__, __LINE__, handler_name, event->rawDataLen, expected, msg->num_entries);
        return NULL;
    }
    return msg;
}

/* ------------------------------------------------------------------ */
/*  Individual event handlers                                          */
/* ------------------------------------------------------------------ */

static const char *hal_sub_type_to_str(uint32_t sub_type)
{
    switch (sub_type) {
    case 260: return "auth_frame";
    case 261: return "deauth_frame";
    case 262: return "assoc_req_frame";
    case 263: return "assoc_rsp_frame";
    case 264: return "reassoc_req_frame";
    case 265: return "reassoc_rsp_frame";
    case 271: return "disassoc_device";
    case 270: return "assoc_device";
    default:  return "unknown_hal_event";
    }
}

static void on_periodic_stats(rbusHandle_t handle,
                               rbusEventRawData_t const *event,
                               rbusEventSubscription_t *subscription)
{
    (void)handle; (void)subscription;
    const lq_rbus_event_msg_t *msg = validate_msg(event, __func__);
    if (!msg || msg->num_entries == 0) return;

    wifi_util_error_print(WIFI_LQ, "%s:%d received event from onewifi: event_type=PeriodicStats num_devs=%u\n",
                          __func__, __LINE__, msg->num_entries);
    for (unsigned int i = 0; i < msg->num_entries; i++) {
        wifi_util_error_print(WIFI_LQ, "%s:%d   [%u] mac=%s vap_index=%d snr=%d phy_rate=%d\n",
                              __func__, __LINE__, i,
                              msg->entries[i].mac_str,
                              msg->entries[i].vap_index,
                              msg->entries[i].dev.cli_SNR,
                              msg->entries[i].dev.cli_LastDataDownlinkRate);
    }
    add_stats_metrics((stats_arg_t *)msg->entries, (int)msg->num_entries);
}

static void on_rapid_disconnect(rbusHandle_t handle,
                                 rbusEventRawData_t const *event,
                                 rbusEventSubscription_t *subscription)
{
    (void)handle; (void)subscription;
    const lq_rbus_event_msg_t *msg = validate_msg(event, __func__);
    if (!msg || msg->num_entries < 1) return;

    wifi_util_error_print(WIFI_LQ, "%s:%d received event from onewifi: event_type=RapidDisconnect mac=%s vap_index=%d snr=%d\n",
                          __func__, __LINE__,
                          msg->entries[0].mac_str,
                          msg->entries[0].vap_index,
                          msg->entries[0].dev.cli_SNR);
    disconnect_link_stats((stats_arg_t *)&msg->entries[0]);
}

static void on_remove(rbusHandle_t handle,
                       rbusEventRawData_t const *event,
                       rbusEventSubscription_t *subscription)
{
    (void)handle; (void)subscription;
    const lq_rbus_event_msg_t *msg = validate_msg(event, __func__);
    if (!msg || msg->num_entries < 1) return;

    wifi_util_error_print(WIFI_LQ, "%s:%d received event from onewifi: event_type=Remove mac=%s vap_index=%d\n",
                          __func__, __LINE__,
                          msg->entries[0].mac_str,
                          msg->entries[0].vap_index);
    remove_link_stats((stats_arg_t *)&msg->entries[0]);
}

static void on_hal_indication(rbusHandle_t handle,
                               rbusEventRawData_t const *event,
                               rbusEventSubscription_t *subscription)
{
    (void)handle; (void)subscription;
    const lq_rbus_event_msg_t *msg = validate_msg(event, __func__);
    if (!msg || msg->num_entries < 1) return;

    wifi_util_error_print(WIFI_LQ, "%s:%d received event from onewifi: event_type=HalIndication sub_type=%u(%s) mac=%s vap_index=%d radio_index=%d status_code=%d channel_util=%d\n",
                          __func__, __LINE__,
                          msg->sub_type,
                          hal_sub_type_to_str(msg->sub_type),
                          msg->entries[0].mac_str,
                          msg->entries[0].vap_index,
                          msg->entries[0].radio_index,
                          msg->entries[0].status_code,
                          msg->entries[0].channel_utilization);
    periodic_caffinity_stats_update((stats_arg_t *)&msg->entries[0], (int)msg->num_entries);
}

static void on_start(rbusHandle_t handle,
                      rbusEventRawData_t const *event,
                      rbusEventSubscription_t *subscription)
{
    (void)handle; (void)event; (void)subscription;
    wifi_util_error_print(WIFI_LQ, "%s:%d received event from onewifi: event_type=Start\n",
                          __func__, __LINE__);
    start_link_metrics();
}

static void on_stop(rbusHandle_t handle,
                     rbusEventRawData_t const *event,
                     rbusEventSubscription_t *subscription)
{
    (void)handle; (void)event; (void)subscription;
    wifi_util_error_print(WIFI_LQ, "%s:%d received event from onewifi: event_type=Stop\n",
                          __func__, __LINE__);
    stop_link_metrics();
}

static void on_gw_discovery(rbusHandle_t handle,
                              rbusEventRawData_t const *event,
                              rbusEventSubscription_t *subscription)
{
    (void)handle; (void)event; (void)subscription;
    wifi_util_error_print(WIFI_LQ, "%s:%d received event from onewifi: event_type=GwDiscovery\n",
                          __func__, __LINE__);
    /* TODO: implement autoconf search in standalone process */
}

/* ------------------------------------------------------------------ */
/*  Subscription table                                                 */
/* ------------------------------------------------------------------ */

typedef void (*raw_event_handler_t)(rbusHandle_t, rbusEventRawData_t const *,
                                     rbusEventSubscription_t *);

static const struct {
    const char         *event_name;
    raw_event_handler_t handler;
} lq_event_table[NUM_LQ_EVENTS] = {
    { LQ_RBUS_EVENT_PERIODIC_STATS,   on_periodic_stats   },
    { LQ_RBUS_EVENT_RAPID_DISCONNECT, on_rapid_disconnect },
    { LQ_RBUS_EVENT_REMOVE,           on_remove           },
    { LQ_RBUS_EVENT_HAL_INDICATION,   on_hal_indication   },
    { LQ_RBUS_EVENT_START,            on_start            },
    { LQ_RBUS_EVENT_STOP,             on_stop             },
    { LQ_RBUS_EVENT_GW_DISCOVERY,     on_gw_discovery     },
};

/* ------------------------------------------------------------------ */
/*  Init / Deinit                                                      */
/* ------------------------------------------------------------------ */

int lq_stats_rbus_init(void)
{
    rbusError_t rc;

    rc = rbus_open(&g_rbus_handle, LQ_STATS_COMPONENT);
    if (rc != RBUS_ERROR_SUCCESS) {
        wifi_util_error_print(WIFI_LQ, "%s:%d rbus_open failed rc=%d\n", __func__, __LINE__, rc);
        return -1;
    }

    memset(g_subs, 0, sizeof(g_subs));
    for (int i = 0; i < NUM_LQ_EVENTS; i++) {
        g_subs[i].eventName    = lq_event_table[i].event_name;
        g_subs[i].handler      = (rbusEventHandler_t)lq_event_table[i].handler;
        g_subs[i].userData     = NULL;
        g_subs[i].filter       = NULL;
        g_subs[i].duration     = 0;
        g_subs[i].asyncHandler = NULL;
    }

    rc = rbusEvent_SubscribeExRawData(g_rbus_handle, g_subs, NUM_LQ_EVENTS, 0);
    if (rc != RBUS_ERROR_SUCCESS) {
        wifi_util_error_print(WIFI_LQ, "%s:%d rbusEvent_SubscribeExRawData failed rc=%d\n",
                              __func__, __LINE__, rc);
        rbus_close(g_rbus_handle);
        g_rbus_handle = NULL;
        return -1;
    }

    wifi_util_info_print(WIFI_LQ, "%s:%d subscribed to %d linkquality events from onewifi\n",
                         __func__, __LINE__, NUM_LQ_EVENTS);
    return 0;
}

void lq_stats_rbus_deinit(void)
{
    if (g_rbus_handle) {
        rbusEvent_UnsubscribeExRawData(g_rbus_handle, g_subs, NUM_LQ_EVENTS);
        stop_link_metrics();
        rbus_close(g_rbus_handle);
        g_rbus_handle = NULL;
    }
}

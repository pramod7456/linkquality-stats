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

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "lq_ipc_receiver.h"
#include "linkquality_util.h"
#include "qmgr.h"

/* ---- IPC protocol (must match ccsp-one-wifi lq_ipc_sender.h) ---- */

#define LQ_STATS_SOCKET_PATH      "/tmp/linkquality_stats.sock"

#define LQ_IPC_MSG_PERIODIC_STATS   1
#define LQ_IPC_MSG_DISCONNECT       2
#define LQ_IPC_MSG_RAPID_DISCONNECT 3
#define LQ_IPC_MSG_CAFFINITY_EVENT  4
#define LQ_IPC_MSG_START_METRICS    5
#define LQ_IPC_MSG_STOP_METRICS     6
#define LQ_IPC_MSG_REGISTER_STA     7
#define LQ_IPC_MSG_UNREGISTER_STA   8
#define LQ_IPC_MSG_REINIT_METRICS   9
#define LQ_IPC_MSG_SET_MAX_SNR     10

typedef struct {
    uint32_t msg_type;
    uint32_t num_entries;
} lq_ipc_header_t;

/* ---- internal state ---- */

static int              g_sock      = -1;
static pthread_t        g_thread;
static volatile sig_atomic_t g_exit = 0;

#define LQ_IPC_BUF_SZ  65536

static const char *msg_type_to_str(uint32_t type)
{
    switch (type) {
    case LQ_IPC_MSG_PERIODIC_STATS:   return "PERIODIC_STATS";
    case LQ_IPC_MSG_DISCONNECT:       return "DISCONNECT";
    case LQ_IPC_MSG_RAPID_DISCONNECT: return "RAPID_DISCONNECT";
    case LQ_IPC_MSG_CAFFINITY_EVENT:  return "CAFFINITY_EVENT";
    case LQ_IPC_MSG_START_METRICS:    return "START_METRICS";
    case LQ_IPC_MSG_STOP_METRICS:     return "STOP_METRICS";
    case LQ_IPC_MSG_REGISTER_STA:     return "REGISTER_STA";
    case LQ_IPC_MSG_UNREGISTER_STA:   return "UNREGISTER_STA";
    case LQ_IPC_MSG_REINIT_METRICS:   return "REINIT_METRICS";
    case LQ_IPC_MSG_SET_MAX_SNR:      return "SET_MAX_SNR";
    default:                          return "UNKNOWN";
    }
}

static void *receiver_thread(void *arg)
{
    (void)arg;
    uint8_t buf[LQ_IPC_BUF_SZ];

    lq_util_info_print(LQ_LQTY,
        "%s:%d IPC receiver thread started, listening on %s\n",
        __func__, __LINE__, LQ_STATS_SOCKET_PATH);

    while (!g_exit) {
        ssize_t n = recvfrom(g_sock, buf, sizeof(buf), 0, NULL, NULL);

        if (n < 0) {
            if (errno == EINTR) continue;
            if (g_exit) break;
            lq_util_error_print(LQ_LQTY,
                "%s:%d recvfrom failed: %s\n", __func__, __LINE__, strerror(errno));
            continue;
        }

        if (n < (ssize_t)sizeof(lq_ipc_header_t)) {
            lq_util_error_print(LQ_LQTY,
                "%s:%d short datagram (%zd bytes), ignoring\n", __func__, __LINE__, n);
            continue;
        }

        lq_ipc_header_t *hdr = (lq_ipc_header_t *)buf;
        uint32_t count = hdr->num_entries;
        size_t payload_sz = (size_t)n - sizeof(lq_ipc_header_t);

        /*
         * For stats_arg_t-bearing messages, validate the payload size.
         * Other messages (START/STOP/REGISTER/UNREGISTER/REINIT/SET_MAX_SNR)
         * carry different-sized or no payloads and are validated per-case.
         */
        if (hdr->msg_type <= LQ_IPC_MSG_CAFFINITY_EVENT && count > 0) {
            size_t expected_data = count * sizeof(stats_arg_t);
            if (payload_sz < expected_data) {
                lq_util_error_print(LQ_LQTY,
                    "%s:%d truncated datagram (got %zd, need %zu), ignoring\n",
                    __func__, __LINE__, n,
                    sizeof(lq_ipc_header_t) + expected_data);
                continue;
            }
        }

        stats_arg_t *entries = (stats_arg_t *)(buf + sizeof(lq_ipc_header_t));

        lq_util_info_print(LQ_LQTY,
            "%s:%d IPC event: type=%s(%u) count=%u\n",
            __func__, __LINE__,
            msg_type_to_str(hdr->msg_type), hdr->msg_type, count);

        switch (hdr->msg_type) {
        case LQ_IPC_MSG_PERIODIC_STATS:
            for (uint32_t i = 0; i < count; i++) {
                lq_util_info_print(LQ_LQTY,
                    "%s:%d PERIODIC_STATS [%u/%u] MAC=%s snr=%d vap=%u radio=%u "
                    "status_code=%u conn_time=%llds disconn_time=%llds\n",
                    __func__, __LINE__, i + 1, count, entries[i].mac_str,
                    entries[i].dev.cli_SNR, entries[i].vap_index,
                    entries[i].radio_index, entries[i].status_code,
                    (long long)entries[i].total_connected_time.tv_sec,
                    (long long)entries[i].total_disconnected_time.tv_sec);
            }
            {
                qmgr_t *qmgr = qmgr_t::get_instance();
                for (uint32_t i = 0; i < count; i++) {
                    qmgr->init(&entries[i], true);
                }
                for (uint32_t i = 0; i < count; i++) {
                    qmgr->caffinity_periodic_stats_update(&entries[i]);
                }
            }
            break;

        case LQ_IPC_MSG_DISCONNECT:
            for (uint32_t i = 0; i < count; i++) {
                lq_util_info_print(LQ_LQTY,
                    "%s:%d DISCONNECT MAC=%s status_code=%u "
                    "conn_time=%llds disconn_time=%llds\n",
                    __func__, __LINE__, entries[i].mac_str,
                    entries[i].status_code,
                    (long long)entries[i].total_connected_time.tv_sec,
                    (long long)entries[i].total_disconnected_time.tv_sec);
            }
            {
                qmgr_t *qmgr = qmgr_t::get_instance();
                for (uint32_t i = 0; i < count; i++) {
                    qmgr->init(&entries[i], false);
                }
            }
            break;

        case LQ_IPC_MSG_RAPID_DISCONNECT:
            for (uint32_t i = 0; i < count; i++) {
                lq_util_info_print(LQ_LQTY,
                    "%s:%d RAPID_DISCONNECT MAC=%s status_code=%u "
                    "conn_time=%llds disconn_time=%llds\n",
                    __func__, __LINE__, entries[i].mac_str,
                    entries[i].status_code,
                    (long long)entries[i].total_connected_time.tv_sec,
                    (long long)entries[i].total_disconnected_time.tv_sec);
            }
            {
                qmgr_t *qmgr = qmgr_t::get_instance();
                for (uint32_t i = 0; i < count; i++) {
                    qmgr->rapid_disconnect(&entries[i]);
                }
            }
            break;

        case LQ_IPC_MSG_CAFFINITY_EVENT:
            /* Single HAL/DHCP event for caffinity */
            for (uint32_t i = 0; i < count; i++) {
                lq_util_info_print(LQ_LQTY,
                    "[linkstatus] %s:%d CAFFINITY_EVENT MAC=%s event=%d "
                    "status_code=%u conn_time=%llds disconn_time=%llds "
                    "dhcp_event=%d dhcp_msg_type=%d\n",
                    __func__, __LINE__, entries[i].mac_str,
                    entries[i].event, entries[i].status_code,
                    (long long)entries[i].total_connected_time.tv_sec,
                    (long long)entries[i].total_disconnected_time.tv_sec,
                    entries[i].dhcp_event, entries[i].dhcp_msg_type);
            }
            {
                qmgr_t *qmgr = qmgr_t::get_instance();
                for (uint32_t i = 0; i < count; i++) {
                    qmgr->caffinity_periodic_stats_update(&entries[i]);
                }
            }
            break;

        case LQ_IPC_MSG_START_METRICS:
            lq_util_info_print(LQ_LQTY,
                "%s:%d START_METRICS\n", __func__, __LINE__);
            qmgr_t::get_instance()->start_background_run();
            break;

        case LQ_IPC_MSG_STOP_METRICS:
            lq_util_info_print(LQ_LQTY,
                "%s:%d STOP_METRICS\n", __func__, __LINE__);
            qmgr_t::destroy_instance();
            break;

        case LQ_IPC_MSG_REGISTER_STA:
        {
            /* Payload is a null-terminated MAC string */
            const char *mac_str = (const char *)(buf + sizeof(lq_ipc_header_t));
            lq_util_info_print(LQ_LQTY,
                "%s:%d REGISTER_STA mac=%s\n", __func__, __LINE__, mac_str);
            qmgr_t::get_instance()->register_station_mac(mac_str);
            break;
        }

        case LQ_IPC_MSG_UNREGISTER_STA:
        {
            const char *mac_str = (const char *)(buf + sizeof(lq_ipc_header_t));
            lq_util_info_print(LQ_LQTY,
                "%s:%d UNREGISTER_STA mac=%s\n", __func__, __LINE__, mac_str);
            qmgr_t::get_instance()->unregister_station_mac(mac_str);
            break;
        }

        case LQ_IPC_MSG_REINIT_METRICS:
        {
            server_arg_t *sarg = (server_arg_t *)(buf + sizeof(lq_ipc_header_t));
            lq_util_info_print(LQ_LQTY,
                "%s:%d REINIT_METRICS reporting=%u threshold=%f\n",
                __func__, __LINE__, sarg->reporting, sarg->threshold);
            qmgr_t::get_instance()->reinit(sarg);
            break;
        }

        case LQ_IPC_MSG_SET_MAX_SNR:
        {
            radio_max_snr_t *snr = (radio_max_snr_t *)(buf + sizeof(lq_ipc_header_t));
            lq_util_info_print(LQ_LQTY,
                "%s:%d SET_MAX_SNR 2g=%d 5g=%d 6g=%d\n",
                __func__, __LINE__, snr->radio_2g_max_snr,
                snr->radio_5g_max_snr, snr->radio_6g_max_snr);
            qmgr_t::get_instance()->set_max_snr_radios(snr);
            break;
        }

        default:
            lq_util_error_print(LQ_LQTY,
                "%s:%d unknown IPC msg_type=%u, ignoring\n",
                __func__, __LINE__, hdr->msg_type);
            break;
        }
    }

    lq_util_info_print(LQ_LQTY,
        "%s:%d IPC receiver thread exiting\n", __func__, __LINE__);
    return NULL;
}

int lq_ipc_receiver_start(void)
{
    struct sockaddr_un addr;

    g_exit = 0;

    g_sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (g_sock < 0) {
        lq_util_error_print(LQ_LQTY,
            "%s:%d socket() failed: %s\n", __func__, __LINE__, strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, LQ_STATS_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    /* Remove stale socket file if present */
    unlink(LQ_STATS_SOCKET_PATH);

    if (bind(g_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        lq_util_error_print(LQ_LQTY,
            "%s:%d bind(%s) failed: %s\n",
            __func__, __LINE__, LQ_STATS_SOCKET_PATH, strerror(errno));
        close(g_sock);
        g_sock = -1;
        return -1;
    }

    if (pthread_create(&g_thread, NULL, receiver_thread, NULL) != 0) {
        lq_util_error_print(LQ_LQTY,
            "%s:%d pthread_create failed: %s\n", __func__, __LINE__, strerror(errno));
        close(g_sock);
        g_sock = -1;
        unlink(LQ_STATS_SOCKET_PATH);
        return -1;
    }

    lq_util_info_print(LQ_LQTY,
        "%s:%d IPC receiver started on %s\n", __func__, __LINE__, LQ_STATS_SOCKET_PATH);
    return 0;
}

void lq_ipc_receiver_stop(void)
{
    g_exit = 1;

    if (g_sock >= 0) {
        shutdown(g_sock, SHUT_RDWR);
        close(g_sock);
        g_sock = -1;
    }

    pthread_join(g_thread, NULL);
    unlink(LQ_STATS_SOCKET_PATH);

    lq_util_info_print(LQ_LQTY,
        "%s:%d IPC receiver stopped\n", __func__, __LINE__);
}
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

/* ---- TLV wire format (must mirror lq_ipc_sender.h) ---- */

/* LQ TLV — the entire datagram is a single TLV, no outer header. */
typedef struct {
    uint8_t  type;   /* LQ_IPC_MSG_* (1-10) */
    uint16_t len;    /* payload byte count */
    uint8_t  value[0];
} __attribute__((packed)) lq_tlv_t;

/* ---- internal state ---- */

static int              g_sock      = -1;
static pthread_t        g_thread;
static volatile sig_atomic_t g_exit = 0;

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

/*
 * parse_tlv - validate and extract type/payload from a raw TLV datagram.
 *
 * The entire datagram IS the TLV (no outer header). The receiver derives
 * element count from payload_sz / sizeof(element_type) as needed.
 *
 * On success: sets *msg_type_out, *payload, and *payload_sz, returns 0.
 * On error:   returns -1.
 */
static int parse_tlv(const uint8_t *buf, size_t buf_sz,
                     uint32_t *msg_type_out,
                     const uint8_t **payload, size_t *payload_sz)
{
    if (buf_sz < sizeof(lq_tlv_t)) {
        lq_util_error_print(LQ_LQTY,
            "[IPC-RECV][TLV] datagram too short: %zu < %zu\n",
            buf_sz, sizeof(lq_tlv_t));
        return -1;
    }

    const lq_tlv_t *tlv = (const lq_tlv_t *)buf;
    size_t val_sz = (size_t)tlv->len;

    if (sizeof(lq_tlv_t) + val_sz > buf_sz) {
        lq_util_error_print(LQ_LQTY,
            "[IPC-RECV][TLV] payload overruns datagram: "
            "tlv->len=%zu + header=%zu > buf_sz=%zu\n",
            val_sz, sizeof(lq_tlv_t), buf_sz);
        return -1;
    }

    *msg_type_out = tlv->type;
    *payload      = tlv->value;
    *payload_sz   = val_sz;
    return 0;
}

static void *receiver_thread(void *arg)
{
    (void)arg;

    lq_util_info_print(LQ_LQTY,
        "%s:%d IPC receiver thread started, listening on %s\n",
        __func__, __LINE__, LQ_STATS_SOCKET_PATH);

    while (!g_exit) {
        /* Step 1: peek at the 3-byte TLV header to learn the payload size */
        lq_tlv_t hdr_peek;
        ssize_t peeked = recvfrom(g_sock, &hdr_peek, sizeof(hdr_peek), MSG_PEEK, NULL, NULL);

        if (peeked < 0) {
            if (errno == EINTR) continue;
            if (g_exit) break;
            lq_util_error_print(LQ_LQTY,
                "%s:%d recvfrom(peek) failed: %s\n", __func__, __LINE__, strerror(errno));
            continue;
        }
        if (peeked < (ssize_t)sizeof(lq_tlv_t)) {
            char drain[1];
            recvfrom(g_sock, drain, sizeof(drain), 0, NULL, NULL);
            lq_util_error_print(LQ_LQTY,
                "%s:%d short datagram (%zd bytes), dropping\n", __func__, __LINE__, peeked);
            continue;
        }

        /* Step 2: allocate exactly what this datagram needs */
        size_t alloc_sz = sizeof(lq_tlv_t) + (size_t)hdr_peek.len;
        uint8_t *buf = (uint8_t *)malloc(alloc_sz);
        if (!buf) {
            char drain[1];
            recvfrom(g_sock, drain, sizeof(drain), 0, NULL, NULL);
            lq_util_error_print(LQ_LQTY,
                "%s:%d malloc(%zu) failed, dropping\n", __func__, __LINE__, alloc_sz);
            continue;
        }

        /* Step 3: consume the full datagram into the exact-sized buffer */
        ssize_t n = recvfrom(g_sock, buf, alloc_sz, 0, NULL, NULL);
        if (n < 0) {
            free(buf);
            if (errno == EINTR) continue;
            if (g_exit) break;
            lq_util_error_print(LQ_LQTY,
                "%s:%d recvfrom failed: %s\n", __func__, __LINE__, strerror(errno));
            continue;
        }

        const uint8_t *payload  = NULL;
        size_t         data_sz  = 0;
        uint32_t       msg_type = 0;

        if (parse_tlv(buf, (size_t)n, &msg_type, &payload, &data_sz) < 0) {
            lq_util_error_print(LQ_LQTY,
                "%s:%d [IPC-RECV] TLV parse failed, dropping\n", __func__, __LINE__);
            free(buf);
            continue;
        }

        lq_util_info_print(LQ_LQTY,
            "%s:%d [IPC-RECV] type=%s(%u) data_sz=%zu datagram_bytes=%zd\n",
            __func__, __LINE__, msg_type_to_str(msg_type), msg_type, data_sz, n);

        switch (msg_type) {
        case LQ_IPC_MSG_PERIODIC_STATS:
        {
            size_t count = data_sz / sizeof(stats_arg_t);
            stats_arg_t *entries = (stats_arg_t *)payload;
            for (size_t i = 0; i < count; i++) {
                lq_util_info_print(LQ_LQTY,
                    "%s:%d PERIODIC_STATS [%zu/%zu] MAC=%s snr=%d vap=%u radio=%u "
                    "status_code=%u conn_time=%llds disconn_time=%llds\n",
                    __func__, __LINE__, i + 1, count, entries[i].mac_str,
                    entries[i].dev.cli_SNR, entries[i].vap_index,
                    entries[i].radio_index, entries[i].status_code,
                    (long long)entries[i].total_connected_time.tv_sec,
                    (long long)entries[i].total_disconnected_time.tv_sec);
            }
            {
                qmgr_t *qmgr = qmgr_t::get_instance();
                for (size_t i = 0; i < count; i++) {
                    qmgr->init(&entries[i], true);
                }
                for (size_t i = 0; i < count; i++) {
                    qmgr->caffinity_periodic_stats_update(&entries[i]);
                }
            }
            break;
        }

        case LQ_IPC_MSG_DISCONNECT:
        {
            size_t count = data_sz / sizeof(stats_arg_t);
            stats_arg_t *entries = (stats_arg_t *)payload;
            for (size_t i = 0; i < count; i++) {
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
                for (size_t i = 0; i < count; i++) {
                    qmgr->init(&entries[i], false);
                }
            }
            break;
        }

        case LQ_IPC_MSG_RAPID_DISCONNECT:
        {
            size_t count = data_sz / sizeof(stats_arg_t);
            stats_arg_t *entries = (stats_arg_t *)payload;
            for (size_t i = 0; i < count; i++) {
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
                for (size_t i = 0; i < count; i++) {
                    qmgr->rapid_disconnect(&entries[i]);
                }
            }
            break;
        }

        case LQ_IPC_MSG_CAFFINITY_EVENT:
        {
            size_t count = data_sz / sizeof(stats_arg_t);
            stats_arg_t *entries = (stats_arg_t *)payload;
            for (size_t i = 0; i < count; i++) {
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
                for (size_t i = 0; i < count; i++) {
                    qmgr->caffinity_periodic_stats_update(&entries[i]);
                }
            }
            break;
        }

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
            const char *mac_str = (const char *)payload;
            lq_util_info_print(LQ_LQTY,
                "%s:%d REGISTER_STA mac=%s\n", __func__, __LINE__, mac_str);
            qmgr_t::get_instance()->register_station_mac(mac_str);
            break;
        }

        case LQ_IPC_MSG_UNREGISTER_STA:
        {
            const char *mac_str = (const char *)payload;
            lq_util_info_print(LQ_LQTY,
                "%s:%d UNREGISTER_STA mac=%s\n", __func__, __LINE__, mac_str);
            qmgr_t::get_instance()->unregister_station_mac(mac_str);
            break;
        }

        case LQ_IPC_MSG_REINIT_METRICS:
        {
            if (data_sz < sizeof(server_arg_t)) {
                lq_util_error_print(LQ_LQTY,
                    "%s:%d REINIT_METRICS: payload too small (%zu < %zu), dropping\n",
                    __func__, __LINE__, data_sz, sizeof(server_arg_t));
                break;
            }
            server_arg_t *sarg = (server_arg_t *)payload;
            lq_util_info_print(LQ_LQTY,
                "%s:%d REINIT_METRICS reporting=%u threshold=%f\n",
                __func__, __LINE__, sarg->reporting, sarg->threshold);
            qmgr_t::get_instance()->reinit(sarg);
            break;
        }

        case LQ_IPC_MSG_SET_MAX_SNR:
        {
            if (data_sz < sizeof(radio_max_snr_t)) {
                lq_util_error_print(LQ_LQTY,
                    "%s:%d SET_MAX_SNR: payload too small (%zu < %zu), dropping\n",
                    __func__, __LINE__, data_sz, sizeof(radio_max_snr_t));
                break;
            }
            radio_max_snr_t *snr = (radio_max_snr_t *)payload;
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
                __func__, __LINE__, msg_type);
            break;
        }

        free(buf);
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
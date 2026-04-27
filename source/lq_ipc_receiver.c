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

/* ---- IPC protocol (must match ccsp-one-wifi lq_ipc_sender.h) ---- */

#define LQ_STATS_SOCKET_PATH      "/tmp/linkquality_stats.sock"

#define LQ_IPC_MSG_PERIODIC_STATS  1
#define LQ_IPC_MSG_DISCONNECT      2
#define LQ_IPC_MSG_RAPID_DISCONNECT 3

typedef struct {
    uint32_t msg_type;
    uint32_t num_entries;
} lq_ipc_header_t;

typedef struct {
    char mac_str[18];
} lq_ipc_sta_entry_t;

/* ---- internal state ---- */

static int              g_sock      = -1;
static pthread_t        g_thread;
static volatile sig_atomic_t g_exit = 0;

/* Maximum datagram size: header + generous room for entries */
#define LQ_IPC_BUF_SZ  65536

static const char *msg_type_to_str(uint32_t type)
{
    switch (type) {
    case LQ_IPC_MSG_PERIODIC_STATS:  return "PERIODIC_STATS";
    case LQ_IPC_MSG_DISCONNECT:      return "DISCONNECT";
    case LQ_IPC_MSG_RAPID_DISCONNECT: return "RAPID_DISCONNECT";
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
            if (errno == EINTR) {
                continue;
            }
            if (g_exit) {
                break;
            }
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
        uint32_t expected_sz = sizeof(lq_ipc_header_t) +
                               hdr->num_entries * sizeof(lq_ipc_sta_entry_t);

        if ((uint32_t)n < expected_sz) {
            lq_util_error_print(LQ_LQTY,
                "%s:%d truncated datagram (got %zd, need %u), ignoring\n",
                __func__, __LINE__, n, expected_sz);
            continue;
        }

        lq_ipc_sta_entry_t *entries =
            (lq_ipc_sta_entry_t *)(buf + sizeof(lq_ipc_header_t));

        lq_util_info_print(LQ_LQTY,
            "%s:%d received IPC event: type=%s(%u) num_entries=%u\n",
            __func__, __LINE__,
            msg_type_to_str(hdr->msg_type), hdr->msg_type, hdr->num_entries);

        for (uint32_t i = 0; i < hdr->num_entries; i++) {
            lq_util_info_print(LQ_LQTY,
                "%s:%d   [%u] MAC=%s event=%s\n",
                __func__, __LINE__, i,
                entries[i].mac_str,
                msg_type_to_str(hdr->msg_type));
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

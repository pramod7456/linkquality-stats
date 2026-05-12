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

#ifndef LQ_IPC_RECEIVER_H
#define LQ_IPC_RECEIVER_H
#include "qmgr.h"

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

/* LQ TLV — the entire datagram is a single TLV, no outer header. */
typedef struct {
    uint8_t  type;   /* LQ_IPC_MSG_* (1-10) */
    uint16_t len;    /* payload byte count */
    uint8_t  value[0];
} __attribute__((packed)) lq_tlv_t;


class qmgr_t;
class ipc_recv_t {
private: 
    int  m_sock;
    pthread_t        m_thread;
    int m_exit;
    qmgr_t *m_qmgr;
public:
/*
 * Start the IPC receiver thread.
 * Creates an AF_UNIX SOCK_DGRAM socket on /tmp/linkquality_stats.sock,
 * spawns receiver_thread to dispatch incoming messages from OneWifi.
 * Returns 0 on success, -1 on error.
 */
int ipc_receiver_start(void);
const char *msg_type_to_str(uint32_t type);

int parse_tlv(const uint8_t *buf, size_t buf_sz,uint32_t *msg_type_out,
    const uint8_t **payload, size_t *payload_sz);
static void *receiver_thread(void *arg);
/*
 * Stop the IPC receiver thread.
 * Sets exit flag, shuts down socket, joins thread, unlinks socket file.
 */
void ipc_receiver_stop(void);
ipc_recv_t();
~ipc_recv_t();

};

#endif /* LQ_IPC_RECEIVER_H */

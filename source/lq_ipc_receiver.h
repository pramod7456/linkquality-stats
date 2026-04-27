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

/*
 * AF_UNIX datagram socket receiver for link-quality events sent by
 * OneWifi (wifi_stats_assoc_client.c) to the linkquality-stats daemon.
 *
 * All socket-related code lives in lq_ipc_receiver.c / .h to keep it
 * separate from the main daemon logic.
 */

/*
 * Start the IPC receiver thread.
 * Creates the AF_UNIX datagram socket, binds to LQ_STATS_SOCKET_PATH,
 * and spawns a background thread that reads datagrams and prints received
 * events (MAC + event type).
 *
 * Returns 0 on success, -1 on failure.
 */
int lq_ipc_receiver_start(void);

/*
 * Stop the IPC receiver thread.
 * Signals the thread to exit, closes the socket, unlinks the socket file,
 * and joins the thread.
 */
void lq_ipc_receiver_stop(void);

#endif /* LQ_IPC_RECEIVER_H */

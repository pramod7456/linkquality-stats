
/**
 * Copyright 2023 Comcast Cable Communications Management, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CAFFINITY_H
#define CAFFINITY_H

#include <pthread.h>
#include <cjson/cJSON.h>
#include "linkq.h"
#include "run_qmgr.h"
#include <algorithm>
#include <string>
#include <unordered_map>
#include "linkquality_util.h"

// Per-BSSID timer data
typedef struct {
    struct timespec connected_time;
    struct timespec disconnected_time;
    struct timespec sleep_time;
} bssid_timers_t;

// Result structure returned by run_algorithm_caffinity
typedef struct {
    double score;
    bool connected;
} caffinity_result_t;

#if 0
typedef enum {
    DHCP_DISCOVER = 1,
    DHCP_OFFER    = 2,
    DHCP_REQUEST  = 3,
    DHCP_DECLINE  = 4,
    DHCP_ACK      = 5,
    DHCP_NAK      = 6,
    DHCP_UNKNOWN
} dhcp_pkt_type_t;
#endif
class caffinity_t
{
    pthread_mutex_t m_lock;
    std::unordered_map<std::string, bssid_timers_t> m_bssid_map;
    std::string m_current_bssid;
    unsigned int m_auth_failures;
    unsigned int m_auth_attempts;
    unsigned int m_assoc_failures;
    unsigned int m_assoc_attempts;
    unsigned int m_discover;
    unsigned int m_offer;
    unsigned int m_request;
    unsigned int m_decline;
    unsigned int m_nak;
    unsigned int m_ack;
    unsigned int m_snr_assoc;
    int m_cli_snr;
    int m_channel_utilization;
    bool m_connected;
    bool m_power_save;
    struct timespec  m_total_time;
    bssid_timers_t aggregate_bssid_timers() const;
public:
    caffinity_t();
    ~caffinity_t();
    int init(stats_arg_t *stats);  // Returns 0 on success, -1 on error
    int periodic_stats_update(stats_arg_t *stats);  // Updates connected_time, disconnected_time, and SNR
    int score();
    caffinity_result_t run_algorithm_caffinity(const char *mac);
    bool get_connected() const { return m_connected; }
    struct timespec get_disconnected_time() const {
        if (!m_current_bssid.empty()) {
            auto it = m_bssid_map.find(m_current_bssid);
            if (it != m_bssid_map.end()) return it->second.disconnected_time;
        }
        struct timespec ts = {0, 0};
        return ts;
    }
    struct timespec get_connected_time() const {
        if (!m_current_bssid.empty()) {
            auto it = m_bssid_map.find(m_current_bssid);
            if (it != m_bssid_map.end()) return it->second.connected_time;
        }
        struct timespec ts = {0, 0};
        return ts;
    }

   
};

#endif

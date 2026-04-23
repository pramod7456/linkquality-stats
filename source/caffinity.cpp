
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "linkq.h"
#include <sys/time.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include "lq_events.h"
#include "caffinity.h"

int caffinity_t::periodic_stats_update(stats_arg_t *arg)
{
    pthread_mutex_lock(&m_lock);
    
    // Handle DHCP event - update individual DHCP counters based on message type
    if (arg->dhcp_event == DHCP_EVENT_UPDATE) {
        switch(arg->dhcp_msg_type) {
            case 1: // DHCPDISCOVER
                m_discover++;
                break;
            case 2: // DHCPOFFER
                m_offer++;
                break;
            case 3: // DHCPREQUEST
                m_request++;
                lq_util_info_print(LQ_CAFF, "caffinity CAFF %s:%d DHCP REQUEST, total=%u\n",
                    __func__, __LINE__, m_request);
                break;
            case 4: // DHCPDECLINE
                m_decline++;
                break;
            case 5: // DHCPACK
                m_ack++;
                break;
            case 6: // DHCPNAK
                m_nak++;
                break;
            default:
                break;
        }
        pthread_mutex_unlock(&m_lock);
        return 0;
    } else {
    
    // Handle WiFi events (auth, assoc, etc.)
    switch(arg->event)
    {
        case wifi_event_hal_auth_frame:
            m_auth_attempts++;
            break;

        case wifi_event_hal_deauth_frame:
            m_auth_failures++;
            m_disconnected_time = arg->total_disconnected_time;
            break;

        case wifi_event_hal_assoc_req_frame:
        case wifi_event_hal_reassoc_req_frame:
            m_assoc_attempts++;
            break;

        case wifi_event_hal_assoc_rsp_frame:
        case wifi_event_hal_reassoc_rsp_frame:
            // Only increment failure if status_code is non-zero
            if (arg->status_code != 0) {
                m_assoc_failures++;
                m_connected = false;
            } else {
                m_connected = true;
                m_connected_time = arg->total_connected_time;

                //lq_util_info_print(LQ_CAFF, "%s:%d check in vector %s\n", __func__, __LINE__, arg->ap_mac_str);
                if (std::find(m_ap_mac.begin(), m_ap_mac.end(), arg->ap_mac_str) == m_ap_mac.end()) {
                    lq_util_info_print(LQ_CAFF, "%s:%d push_back\n", __func__, __LINE__);
                    m_ap_mac.push_back(arg->ap_mac_str);
                }

                for (size_t i = 0; i < m_ap_mac.size(); ++i) {
                    lq_util_info_print(LQ_CAFF, "%s:%d: %d: AP MAC %s\n", __func__, __LINE__, i, m_ap_mac[i].c_str());
                }
            }
            break;

        case wifi_event_hal_sta_conn_status:
            {
                lq_util_info_print(LQ_CAFF, "caffinity CAFF %s:%d wifi_event_hal_sta_conn_status for MAC %s\n",
                    __func__, __LINE__, arg->mac_str);
            }
            break;

        case wifi_event_hal_disassoc_device:
            m_connected = false;
            m_disconnected_time = arg->total_disconnected_time;
            lq_util_info_print(LQ_CAFF, "caffinity CAFF %s:%d DISASSOC device, m_connected=false, disconnected_time=%ld.%09ld\n",
                __func__, __LINE__, (long)m_disconnected_time.tv_sec, m_disconnected_time.tv_nsec);
            break;

        default:
            break;
    }
    }

    // Update m_connected_time from total_connected_time
    m_connected_time = arg->total_connected_time;
    

    // Update m_disconnected_time from total_disconnected_time
    m_disconnected_time = arg->total_disconnected_time;

    // Update cli_SNR only if client is connected to avoid overwriting valid SNR with 0
    if (m_connected) {
        m_cli_snr = arg->dev.cli_SNR;
        lq_util_info_print(LQ_CAFF, "caffinity stats %s:%d Updated m_cli_snr=%d (client connected)\n",
            __func__, __LINE__, m_cli_snr);
    } else {
        lq_util_info_print(LQ_CAFF, "caffinity stats %s:%d Skipping m_cli_snr update (client disconnected, keeping existing value=%d)\n",
            __func__, __LINE__, m_cli_snr);
    }

    // Update channel_utilization
    m_channel_utilization = arg->channel_utilization;

    pthread_mutex_unlock(&m_lock);
    
    return 0;
}

caffinity_result_t caffinity_t::run_algorithm_caffinity()
{
    caffinity_result_t result;
    double score = 0.0;
    
    // Initialize result
    strncpy(result.mac, m_mac, sizeof(result.mac) - 1);
    result.mac[sizeof(result.mac) - 1] = '\0';
    result.score = 0.0;
    result.connected = false;


    pthread_mutex_lock(&m_lock);


    result.connected = m_connected;
    
    bool connected = m_connected;

    /* ------------------------------------------------------------------ */
    /* PATH A: Connected client                                             */
    /* score = connected_time / (connected_time + disconnected_time + sleep_time) */
    /* ------------------------------------------------------------------ */
    if (connected) {
        double connected_sec    = (double)m_connected_time.tv_sec
                                  + (double)m_connected_time.tv_nsec / 1e9;
        double disconnected_sec = (double)m_disconnected_time.tv_sec
                                  + (double)m_disconnected_time.tv_nsec / 1e9;
        static thread_local unsigned int seed = (unsigned int)time(nullptr);
        double sleep_sec = 0;

        pthread_mutex_unlock(&m_lock);
        
        double total = connected_sec + disconnected_sec + sleep_sec;

        if (total <= 0.0) {
            lq_util_info_print(LQ_CAFF,
                "caffinity %s:%d Connected client, total time is zero, returning score=0\n",
                __func__, __LINE__);
            return result;
        }

        score = connected_sec / total;

        result.score = score;
        return result;
    }

    /* ------------------------------------------------------------------ */
    /* PATH B: Unconnected client sigmoid logic                           */
    /* ------------------------------------------------------------------ */
    double failure_ratio    = 0.0;
    double auth_failure_rate  = 0.0;
    double assoc_failure_rate = 0.0;
    double dhcp_failure_rate  = 0.0;
    double snr_normalized   = 0.0;
    double snr_squared      = 0.0;
    double sigmoid_factor   = 0.0;
    double exponent         = 0.0;
    int    channel_utilization = 0;
    int    cli_snr = 0;

    // For unconnected clients, use last known SNR (m_cli_snr)
    cli_snr = m_cli_snr;
    channel_utilization = m_channel_utilization;

    // Calculate DHCP attempts and failures from raw counters
    // attempts = max(request, offer) - covers both initial lease and renewals
    uint32_t dhcp_attempts = (m_request > m_offer) ? m_request : m_offer;
    
    // Case 1: no server response at all (discover sent but no offer received)
    uint32_t no_offer_failure = (m_discover > 0 && m_offer == 0) ? 1 : 0;
    
    // Case 2: missing ACKs (persistent failures)
    uint32_t missing_acks = (m_request > m_ack) ? (m_request - m_ack) : 0;
    
    // Final failures = nak + decline + no_offer_failure + missing_acks
    uint32_t dhcp_failures = m_nak + m_decline + no_offer_failure + missing_acks;
    

    // Calculate failure rates with division-by-zero protection
    if (m_auth_attempts > 0) {
        auth_failure_rate = (double)m_auth_failures / (double)m_auth_attempts;
    }
    if (m_assoc_attempts > 0) {
        assoc_failure_rate = (double)m_assoc_failures / (double)m_assoc_attempts;
    }
    // DHCP failure rate calculated from computed attempts/failures
    if (dhcp_attempts > 0) {
        dhcp_failure_rate = (double)dhcp_failures / (double)dhcp_attempts;
    }

    pthread_mutex_unlock(&m_lock);

    // Sum failure rates
    failure_ratio = auth_failure_rate + assoc_failure_rate + dhcp_failure_rate;


    // Normalize SNR to [0, 1] range (max SNR assumed 70)
    if (cli_snr > 0) {
        snr_normalized = (double)cli_snr / 70.0;
    }


    // Square the normalized SNR
    snr_squared = snr_normalized * snr_normalized;


    // Compute sigmoid factor: 1 / (1 + exp(-(b0 + b1 * channel_utilization)))
    exponent = -(LINK_QTY_B0 + LINK_QTY_B1 * channel_utilization);

    // Clamp exponent to safe range for numerical stability
    if (exponent < -50.0) exponent = -50.0;
    if (exponent > 50.0) exponent = 50.0;

    sigmoid_factor = 1.0 / (1.0 + exp(exponent));


    // Calculate final score: (1 - failure_ratio) * snr_squared * sigmoid_factor
    score = (1.0 - failure_ratio) * snr_squared * sigmoid_factor;

    lq_util_dbg_print(LQ_CAFF, "caffinity %s:%d FINAL SCORE=%.4f for MAC %s connected %d\n",
        __func__, __LINE__, score, m_mac, m_connected);
    
    result.score = score ;
    return result;
}



caffinity_t::caffinity_t(mac_addr_str_t *mac)
{
    strncpy(m_mac, *mac, sizeof(m_mac) - 1);
    m_mac[sizeof(m_mac) - 1] = '\0';
    pthread_mutex_init(&m_lock, NULL);
    m_auth_failures = 0;
    m_auth_attempts = 0;
    m_assoc_failures = 0;
    m_assoc_attempts = 0;
    m_discover = 0;
    m_offer = 0;
    m_request = 0;
    m_decline = 0;
    m_nak = 0;
    m_ack = 0;
    m_snr_assoc = 0;
    m_cli_snr = 0;
    m_channel_utilization = 0;
    m_power_save = false;
    memset(&m_disconnected_time, 0, sizeof(m_disconnected_time));
    memset(&m_connected_time, 0, sizeof(m_connected_time));
    memset(&m_sleep_time, 0, sizeof(m_sleep_time));
    memset(&m_total_time, 0, sizeof(m_total_time));
    m_connected =  false;

}

caffinity_t::~caffinity_t()
{
   pthread_mutex_destroy(&m_lock);
}

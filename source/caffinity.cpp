
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
    lq_util_info_print(LQ_CAFF, "caffinity CAFF %s:%d event=%d dhcp_event=%d\n", __func__, __LINE__, arg->event, arg->dhcp_event);

    pthread_mutex_lock(&m_lock);
    
    // Handle DHCP event - update individual DHCP counters based on message type
    if (arg->dhcp_event == DHCP_EVENT_UPDATE) {
        switch (arg->dhcp_msg_type) {
        case DHCP_DISCOVER: m_discover++; break;
        case DHCP_OFFER:    m_offer++;    break;
        case DHCP_REQUEST:  m_request++;  break;
        case DHCP_DECLINE:  m_decline++;  break;
        case DHCP_ACK:      m_ack++;      break;
        case DHCP_NAK:      m_nak++;      break;
        default:
            lq_util_info_print(LQ_CAFF, "%s:%d Unknown DHCP msg_type=%d\n", __func__, __LINE__, arg->dhcp_msg_type);
            break;
        }
        lq_util_info_print(LQ_CAFF, "caffinity CAFF %s:%d DHCP stats: discover=%u offer=%u"
                                    " request=%u decline=%u ack=%u nak=%u\n", __func__, __LINE__,
                                    m_discover, m_offer, m_request, m_decline, m_ack, m_nak);
    } else {
        // Handle WiFi events (auth, assoc, etc.)
        switch(arg->event) {
        case wifi_event_hal_auth_frame:
            m_auth_attempts++;
            // Also handles PERIODIC_STATS (event=0 same as auth_frame):
            // infer m_connected from total_connected_time
            if (arg->total_connected_time.tv_sec > 0) {
                m_connected = true;
            }
            break;

        case wifi_event_hal_deauth_frame:
            m_connected = false;
            m_auth_failures++;
            if (!m_current_bssid.empty()) {
                m_bssid_map[m_current_bssid].disconnected_time = arg->total_disconnected_time;
            }
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
                lq_util_info_print(LQ_CAFF, "caffinity CAFF %s:%d ASSOC/REASSOC response FAILED (status=%u), failures=%u, m_connected=false\n",
                        __func__, __LINE__, arg->status_code, m_assoc_failures);
            } else {
                m_connected = true;
                m_current_bssid = arg->ap_mac_str;
                m_bssid_map[m_current_bssid].connected_time = arg->total_connected_time;
                lq_util_info_print(LQ_CAFF, "caffinity CAFF %s:%d ASSOC/REASSOC response SUCCESS (status=%u), "
                            "m_connected=true, bssid=%s, connected_time=%ld.%09ld\n", __func__, __LINE__, arg->status_code,
                            m_current_bssid.c_str(),
                            (long)m_bssid_map[m_current_bssid].connected_time.tv_sec,
                            m_bssid_map[m_current_bssid].connected_time.tv_nsec);

                // Log all tracked BSSIDs
                for (auto it = m_bssid_map.begin(); it != m_bssid_map.end(); ++it) {
                    lq_util_info_print(LQ_CAFF, "%s:%d: BSSID %s conn=%ld.%09ld disc=%ld.%09ld\n",
                        __func__, __LINE__, it->first.c_str(),
                        (long)it->second.connected_time.tv_sec, it->second.connected_time.tv_nsec,
                        (long)it->second.disconnected_time.tv_sec, it->second.disconnected_time.tv_nsec);
                }
            }
            break;

        case wifi_event_hal_sta_conn_status:
            lq_util_info_print(LQ_CAFF, "caffinity CAFF %s:%d wifi_event_hal_sta_conn_status for MAC %s\n",
                                        __func__, __LINE__, arg->mac_str);
            break;

        case wifi_event_hal_disassoc_device:
            m_connected = false;
            if (!m_current_bssid.empty()) {
                m_bssid_map[m_current_bssid].disconnected_time = arg->total_disconnected_time;
                lq_util_info_print(LQ_CAFF, "caffinity CAFF %s:%d DISASSOC device, m_connected=false, bssid=%s, disconnected_time=%ld.%09ld\n",
                                            __func__, __LINE__, m_current_bssid.c_str(),
                                            (long)arg->total_disconnected_time.tv_sec, arg->total_disconnected_time.tv_nsec);
            }
            break;

        default:
            lq_util_info_print(LQ_CAFF, "caffinity CAFF %s:%d Unhandled event=%d\n",
                                        __func__, __LINE__, arg->event);
            break;
        }

        // Update timers on the current BSSID entry
        if (!m_current_bssid.empty()) {
            m_bssid_map[m_current_bssid].connected_time = arg->total_connected_time;
            m_bssid_map[m_current_bssid].disconnected_time = arg->total_disconnected_time;
        }

        // Update cli_SNR only if client is connected to avoid overwriting valid SNR with 0
        if (m_connected) {
            m_cli_snr = arg->dev.cli_SNR;
        }

        // Update channel_utilization
        m_channel_utilization = arg->channel_utilization;
#if 0
        bool new_ps = (bool)arg->dev.cli_PowerSaveMode;
        lq_util_info_print(LQ_CAFF, "caffinity stats %s:%d [PS-DBG] MAC %s arg->dev.cli_PowerSaveMode=%d m_power_save=%d\n",
                              __func__, __LINE__, arg->mac_str, (int)new_ps, (int)m_power_save);
        if (new_ps != m_power_save) {
           lq_util_info_print(LQ_CAFF,  "caffinity stats %s:%d m_power_save changed: %d -> %d for MAC %s\n",
                                  __func__, __LINE__, (int)m_power_save, (int)new_ps, arg->mac_str);
            m_power_save = new_ps;
        }
#endif
    }

    pthread_mutex_unlock(&m_lock);
    
    // Change below to dbg prints after testing.
    lq_util_info_print(LQ_CAFF, "caffinity stats %s:%d Updated stats for event=%d: auth_attempts=%u auth_failures=%u"
                                "assoc_attempts=%u assoc_failures=%u\n", __func__, __LINE__, arg->event,
                                m_auth_attempts, m_auth_failures, m_assoc_attempts, m_assoc_failures);
    if (!m_current_bssid.empty()) {
        auto it = m_bssid_map.find(m_current_bssid);
        if (it != m_bssid_map.end()) {
            lq_util_info_print(LQ_CAFF, "caffinity stats %s:%d Updated periodic stats for MAC %s bssid=%s: "
                              "connected_time=%ld.%09ld disconnected_time=%ld.%09ld cli_SNR=%d channel_utilization=%d\n",
                              __func__, __LINE__, arg->mac_str, m_current_bssid.c_str(),
                              (long)it->second.connected_time.tv_sec, it->second.connected_time.tv_nsec,
                              (long)it->second.disconnected_time.tv_sec, it->second.disconnected_time.tv_nsec,
                              m_cli_snr, m_channel_utilization);
        }
    }
    return 0;
}

caffinity_result_t caffinity_t::run_algorithm_caffinity(const char *mac)
{
    caffinity_result_t result;
    double score = 0.0;
    
    // Initialize result
    result.score = 0.0;
    result.connected = false;

    lq_util_info_print(LQ_CAFF, "caffinity %s:%d Computing caffinity score for MAC %s\n",
                                __func__, __LINE__, mac);

    pthread_mutex_lock(&m_lock);

    // Skip scoring while client is in Power Save mode
    if (m_power_save) {
        lq_util_info_print(LQ_CAFF,
            "caffinity %s:%d Skipping score for MAC %s — client in power save mode\n",
            __func__, __LINE__, mac);
        result.connected = m_connected;
        pthread_mutex_unlock(&m_lock);
        return result;
    }

    result.connected = m_connected;
    
    // Debug dump of all stats for this MAC (one call per line to avoid logging truncation on embedded \n)
    lq_util_info_print(LQ_CAFF, "caffinity %s:%d [MAC=%s] Stats Dump:\n", __func__, __LINE__, mac);
    lq_util_info_print(LQ_CAFF,  "caffinity   auth_attempts=%u auth_failures=%u assoc_attempts=%u\n",
        m_auth_attempts, m_auth_failures, m_assoc_attempts);
    lq_util_info_print(LQ_CAFF, "caffinity   assoc_failures=%u dhcp: discover=%u offer=%u request=%u decline=%u ack=%u nak=%u\n",
        m_assoc_failures, m_discover, m_offer, m_request, m_decline, m_ack, m_nak);
    // Dump per-BSSID timers
    for (auto it = m_bssid_map.begin(); it != m_bssid_map.end(); ++it) {
        lq_util_info_print(LQ_CAFF, "caffinity   bssid=%s conn=%ld.%09ld disc=%ld.%09ld sleep=%ld.%09ld\n",
            it->first.c_str(),
            (long)it->second.connected_time.tv_sec, it->second.connected_time.tv_nsec,
            (long)it->second.disconnected_time.tv_sec, it->second.disconnected_time.tv_nsec,
            (long)it->second.sleep_time.tv_sec, it->second.sleep_time.tv_nsec);
    }
    lq_util_info_print(LQ_CAFF, "caffinity   total_time=%ld.%09ld connected=%d\n",
        (long)m_total_time.tv_sec, (long)m_total_time.tv_nsec, m_connected);

    /* ------------------------------------------------------------------ */
    /* PATH A: Connected client                                             */
    /* score = connected_time / (connected_time + disconnected_time + sleep_time) */
    /* ------------------------------------------------------------------ */
    if (m_connected) {
        bssid_timers_t avg = aggregate_bssid_timers();
        double connected_sec    = (double)avg.connected_time.tv_sec
                                  + (double)avg.connected_time.tv_nsec / 1e9;
        double disconnected_sec = (double)avg.disconnected_time.tv_sec
                                  + (double)avg.disconnected_time.tv_nsec / 1e9;
        double sleep_sec        = (double)avg.sleep_time.tv_sec
                                  + (double)avg.sleep_time.tv_nsec / 1e9;

        pthread_mutex_unlock(&m_lock);
        
        double total = connected_sec + disconnected_sec + sleep_sec;

        lq_util_info_print(LQ_CAFF,
            "stats_dump CAFF_CONNECTED_RAW MAC=%s bssid_count=%zu connected_sec=%.4f disconnected_sec=%.4f sleep_sec=%.4f total=%.4f\n",
            mac, m_bssid_map.size(), connected_sec, disconnected_sec, sleep_sec, total);

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
    
    lq_util_info_print(LQ_CAFF, "caffinity %s:%d DHCP computed: attempts=%u failures=%u (no_offer=%u missing_acks=%u)\n",
                                __func__, __LINE__, dhcp_attempts, dhcp_failures, no_offer_failure, missing_acks);

    // Calculate failure rates with division-by-zero protection
    if (m_auth_attempts > 0) {
        auth_failure_rate = (double)m_auth_failures / (double)m_auth_attempts;
    }
    if (m_assoc_attempts > 0) {
        assoc_failure_rate = (double)m_assoc_failures / (double)m_assoc_attempts;
    }
    pthread_mutex_unlock(&m_lock);

    // DHCP failure rate calculated from computed attempts/failures
    if (dhcp_attempts > 0) {
        dhcp_failure_rate = (double)dhcp_failures / (double)dhcp_attempts;
    }

     lq_util_info_print(LQ_CAFF, "caffinity %s:%d cli_SNR=%d, channel_utilization=%d\n",
                                 __func__, __LINE__, cli_snr, channel_utilization);

    // Sum failure rates
    failure_ratio = auth_failure_rate + assoc_failure_rate + dhcp_failure_rate;

    lq_util_info_print(LQ_CAFF,
        "SCORE caffinity %s:%d failure_ratio=%.4f (auth=%.4f, assoc=%.4f, dhcp=%.4f)\n",
        __func__, __LINE__, failure_ratio, auth_failure_rate, assoc_failure_rate, dhcp_failure_rate);

    // Normalize SNR to [0, 1] range (max SNR assumed 70)
    if (cli_snr > 0) {
        snr_normalized = (double)cli_snr / 70.0;
    }

    // Square the normalized SNR
    snr_squared = snr_normalized * snr_normalized;

    lq_util_info_print(LQ_CAFF, "caffinity %s:%d snr_normalized=%.4f, snr_squared=%.4f\n",
                                __func__, __LINE__, snr_normalized, snr_squared);

    // Compute sigmoid factor: 1 / (1 + exp(-(b0 + b1 * channel_utilization)))
    exponent = -(LINK_QTY_B0 + LINK_QTY_B1 * channel_utilization);

    // Clamp exponent to safe range for numerical stability
    if (exponent < -50.0) {
        exponent = -50.0;
    }

    if (exponent > 50.0) {
        exponent = 50.0;
    }

    sigmoid_factor = 1.0 / (1.0 + exp(exponent));

    lq_util_info_print(LQ_CAFF,
        "stats_dump CAFF_UNCONNECTED_RAW MAC=%s "
        "cli_snr=%d channel_util=%d "
        "auth_att=%u auth_fail=%u auth_fail_rate=%.4f "
        "assoc_att=%u assoc_fail=%u assoc_fail_rate=%.4f "
        "dhcp_disc=%u dhcp_offer=%u dhcp_req=%u dhcp_ack=%u dhcp_nak=%u dhcp_decline=%u "
        "dhcp_att=%u dhcp_fail=%u dhcp_fail_rate=%.4f "
        "failure_ratio=%.4f snr_norm=%.4f snr_sq=%.4f exponent=%.4f sigmoid=%.4f\n",
        mac,
        cli_snr, channel_utilization,
        m_auth_attempts, m_auth_failures, auth_failure_rate,
        m_assoc_attempts, m_assoc_failures, assoc_failure_rate,
        m_discover, m_offer, m_request, m_ack, m_nak, m_decline,
        dhcp_attempts, dhcp_failures, dhcp_failure_rate,
        failure_ratio, snr_normalized, snr_squared, exponent, sigmoid_factor);

    // Calculate final score: (1 - failure_ratio) * snr_squared * sigmoid_factor
    score = (1.0 - failure_ratio) * snr_squared * sigmoid_factor;

    lq_util_dbg_print(LQ_CAFF, "caffinity %s:%d FINAL SCORE=%.4f for MAC %s connected %d\n",
                               __func__, __LINE__, score, mac, m_connected);
    
    result.score = score ;
    return result;
}

caffinity_t::caffinity_t()
{
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
    memset(&m_total_time, 0, sizeof(m_total_time));
    m_connected =  false;

}

caffinity_t::~caffinity_t()
{
   pthread_mutex_destroy(&m_lock);
}

bssid_timers_t caffinity_t::aggregate_bssid_timers() const
{
    bssid_timers_t avg = {};
    memset(&avg, 0, sizeof(avg));

    size_t count = m_bssid_map.size();
    if (count == 0) {
        return avg;
    }

    long long sum_conn_sec = 0, sum_conn_nsec = 0;
    long long sum_disc_sec = 0, sum_disc_nsec = 0;
    long long sum_sleep_sec = 0, sum_sleep_nsec = 0;

    for (auto it = m_bssid_map.begin(); it != m_bssid_map.end(); ++it) {
        sum_conn_sec   += it->second.connected_time.tv_sec;
        sum_conn_nsec  += it->second.connected_time.tv_nsec;
        sum_disc_sec   += it->second.disconnected_time.tv_sec;
        sum_disc_nsec  += it->second.disconnected_time.tv_nsec;
        sum_sleep_sec  += it->second.sleep_time.tv_sec;
        sum_sleep_nsec += it->second.sleep_time.tv_nsec;
    }

    // Normalize nanosecond overflow into seconds before averaging
    sum_conn_sec  += sum_conn_nsec / 1000000000LL;
    sum_conn_nsec  = sum_conn_nsec % 1000000000LL;
    sum_disc_sec  += sum_disc_nsec / 1000000000LL;
    sum_disc_nsec  = sum_disc_nsec % 1000000000LL;
    sum_sleep_sec += sum_sleep_nsec / 1000000000LL;
    sum_sleep_nsec = sum_sleep_nsec % 1000000000LL;

    long long n = (long long)count;
    avg.connected_time.tv_sec     = (time_t)(sum_conn_sec / n);
    avg.connected_time.tv_nsec    = (long)((sum_conn_nsec + (sum_conn_sec % n) * 1000000000LL) / n);
    avg.disconnected_time.tv_sec  = (time_t)(sum_disc_sec / n);
    avg.disconnected_time.tv_nsec = (long)((sum_disc_nsec + (sum_disc_sec % n) * 1000000000LL) / n);
    avg.sleep_time.tv_sec         = (time_t)(sum_sleep_sec / n);
    avg.sleep_time.tv_nsec        = (long)((sum_sleep_nsec + (sum_sleep_sec % n) * 1000000000LL) / n);

    lq_util_info_print(LQ_CAFF, "caffinity %s:%d aggregate_bssid_timers: count=%zu "
        "avg_conn=%ld.%09ld avg_disc=%ld.%09ld avg_sleep=%ld.%09ld\n",
        __func__, __LINE__, count,
        (long)avg.connected_time.tv_sec, avg.connected_time.tv_nsec,
        (long)avg.disconnected_time.tv_sec, avg.disconnected_time.tv_nsec,
        (long)avg.sleep_time.tv_sec, avg.sleep_time.tv_nsec);

    return avg;
}

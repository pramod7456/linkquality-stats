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
#include <dirent.h>
#include "qmgr.h"
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <vector>
#include <map>
#include <cjson/cJSON.h>
#include "wifi_util.h"

qmgr_t* qmgr_t::instance = NULL;
uint8_t qmgr_t::m_gw_mac[6] = {0};
extern "C" void qmgr_invoke_batch(const report_batch_t *batch);
extern "C" void qmgr_invoke_t2_callback(char **str,int count,double avg_lq_score,double avg_caff_score,double avg_ucaff_score);

qmgr_t* qmgr_t::get_instance()
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);

    if (instance == NULL) {
        instance = new qmgr_t();
    }

    pthread_mutex_unlock(&lock);

    return instance;
}

int qmgr_t::push_reporting_subdoc()
{
    linkq_t *lq;
    lq = (linkq_t *)hash_map_get_first(m_link_map);
    size_t total_links = hash_map_count(m_link_map);  // or precompute
    report_batch_t *report = (report_batch_t *)calloc(1, sizeof(report_batch_t));
    if (!report) return -1;
    report->links = (link_report_t *)calloc(total_links, sizeof(link_report_t));
    if (!report->links) {
        free(report);
        return -1;
    }

    size_t link_index = 0;
    sample_t *samples = NULL;
    size_t sample_count = 0;

    while (lq != NULL) {
        sample_count = lq->get_window_samples(&samples);
        if (sample_count > 0) {
            link_report_t *lr = &report->links[link_index];
            memset(lr, 0, sizeof(link_report_t));

            strncpy(lr->mac, lq->get_mac_addr(), sizeof(lr->mac) - 1);
            lr->mac[sizeof(lr->mac) - 1] = '\0';
            lr->vap_index = lq->get_vap_index();
            lr->threshold = m_args.threshold;
            lr->alarm = lq->get_alarm();
            get_local_time(lr->reporting_time,sizeof(lr->reporting_time),false);
            lr->sample_count = sample_count;
            lr->samples = (sample_t *)calloc(sample_count, sizeof(sample_t));
            for (size_t i = 0; i < sample_count; i++) {
                lr->samples[i] = samples[i];   // only safe if no pointers
            }

            free(samples);
            samples = NULL;

            link_index++;
        }
        lq->clear_window_samples();
        lq = (linkq_t *)hash_map_get_next(m_link_map, lq);
    }
    report->link_count = link_index;
    // Call the callback
    qmgr_invoke_batch(report);
    wifi_util_dbg_print(WIFI_APPS,"%s:%d Executed callback\n",__func__,__LINE__);

    // Free everything after callback
    for (size_t i = 0; i < report->link_count; i++) {
        free(report->links[i].samples);
    }
    free(report->links);
    free(report);
    return 0;
}
int qmgr_t::run()
{
    int rc,count = 0;
    struct timespec time_to_wait;
    struct timeval tm;
    struct timeval start_time;
    linkq_t *lq;
    vector_t v;
    mac_addr_str_t mac_str;
    unsigned char *sta_mac;
    std::vector<std::string> payload_list;
    bool alarm = false;
    bool rapid_disconnect = false;
    long elapsed_sec  = 0;
    bool update_alarm = false;
   std::string device_json;
   double rms_lq_score,rms_caffinity_score,rms_ucaffinity_score;
    gettimeofday(&start_time, NULL);
    pthread_mutex_lock(&m_lock);
    while (m_exit == false) {
        rc = 0;

        gettimeofday(&tm, NULL);
        time_to_wait.tv_sec = tm.tv_sec + m_args.sampling;
        time_to_wait.tv_nsec = tm.tv_usec * 1000;
        
        rc = pthread_cond_timedwait(&m_cond, &m_lock, &time_to_wait);
        gettimeofday(&tm, NULL);
        if (rc == 0) {
            ;
        } else if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&m_lock);
            elapsed_sec = tm.tv_sec - start_time.tv_sec;
            if (elapsed_sec >= m_args.reporting) {
                update_alarm = true;  
            } else {
                update_alarm = false;  
            }
            lq = (linkq_t *)hash_map_get_first(m_link_map);
            wifi_util_dbg_print(WIFI_APPS,"%s:%d Processing %d devices in m_link_map\n",
                __func__,__LINE__, hash_map_count(m_link_map));
            double lq_sum_sq_iter = 0.0;
            int lq_count_iter = 0;
            while (lq != NULL) {
                v = lq->run_test(alarm,update_alarm, rapid_disconnect);
                // Skip if run_test returned invalid/no data
                if (v.m_num == 0 && !rapid_disconnect) {
                    wifi_util_dbg_print(WIFI_APPS,
                        "%s:%d: Skipping device %s as no valid data available (v.m_num=%d)\n",
                        __func__, __LINE__, lq->get_mac_addr(), v.m_num);
                    lq = (linkq_t *)hash_map_get_next(m_link_map, lq);
                    continue;
                }
                wifi_util_dbg_print(WIFI_APPS,"%s:%d Device %s has valid data (v.m_num=%d), updating JSON\n",
                    __func__,__LINE__, lq->get_mac_addr(), v.m_num);
                strncpy(mac_str, lq->get_mac_addr(), sizeof(mac_str) - 1);
                mac_str[sizeof(mac_str) - 1] = '\0';
                
                // Accumulate RMS for Link Quality Score (per-iteration)
                double lq_score = v.m_val[SCORE_INDEX].m_re;
                lq_sum_sq_iter += lq_score * lq_score;

                device_json += "{";
                device_json += "\"mac\":\"" + std::string(lq->get_mac_addr()) + "\",";
                device_json += "\"lq_score\":" + std::to_string(lq_score) + ",";
                device_json += "\"values\":[";

                for (int i = 0; i < v.m_num; i++) {
                    device_json += std::to_string(v.m_val[i].m_re);
                    if (i != v.m_num - 1) device_json += ",";
                }

               device_json += "]";
                device_json += "\"caff_score\":" +  std::to_string(0.888) + ",\"values\":[";
	       device_json +=  std::to_string(29.4455) +"]}";
               wifi_util_info_print(WIFI_CTRL,"Pramod %s:%d device_json = %s\n",__func__,__LINE__,device_json.c_str());
               payload_list.push_back(device_json);
	       device_json = "";
		lq_sum_sq_iter += lq_score * lq_score;
                lq_count_iter++;

                lq = (linkq_t *)hash_map_get_next(m_link_map, lq);
            }
            
            // Calculate and update Link Quality RMS (per-iteration snapshot)
            if (lq_count_iter > 0) {
                double rms_lq = sqrt(lq_sum_sq_iter / lq_count_iter);
                rms_lq_score = rms_lq;
            }
            
            // --- Process caffinity in single pass: classify and populate JSON ---
            if (!m_caffinity_map.empty()) {
                pthread_mutex_lock(&m_json_lock);
                char tmp[MAX_LINE_SIZE];
                
                // Reset counts for this iteration
                int connected_count = 0;
                int unconnected_count = 0;
                double conn_sum_sq_iter = 0.0;
                double unconn_sum_sq_iter = 0.0;
                
                // Process each client: compute score, classify, populate JSON, accumulate RMS
                std::unordered_map<std::string, caffinity_t*>::iterator caff_it;
                for (caff_it = m_caffinity_map.begin(); caff_it != m_caffinity_map.end(); ++caff_it) {
                    caffinity_t *caff = caff_it->second;
                    if (!caff) continue;
                    
                    // Compute score and get connection status
                    caffinity_result_t result = caff->run_algorithm_caffinity();
                    const char *mac_cstr = result.mac;
                    double score = result.score;
                    
                    if (result.connected) {
                        // Process connected client
                        conn_sum_sq_iter += score * score;
                        connected_count++;
                    } else {
                        // Process unconnected client
                        unconn_sum_sq_iter += score * score;
                        unconnected_count++;
                    }
                }
                
                // Calculate per-iteration RMS values (snapshot, no historical accumulation)
                double rms_connected = (connected_count > 0) ? sqrt(conn_sum_sq_iter / connected_count) : 0.0;
                double rms_unconnected = (unconnected_count > 0) ? sqrt(unconn_sum_sq_iter / unconnected_count) : 0.0;
                rms_caffinity_score = rms_connected;
                rms_ucaffinity_score = rms_unconnected;
                wifi_util_info_print(WIFI_CTRL, "%s:%d RMS connected %lf samples, RMS unconnected %lf samples\n",
                        __func__, __LINE__, rms_connected, rms_unconnected);
                // Update RMS aggregate JSON

                pthread_mutex_unlock(&m_json_lock);
            }
            count = hash_map_count(m_link_map);
            if (count == 0 ) {
                remove(m_args.output_file);
            }
            if (update_alarm) {
                start_time = tm;
                update_alarm = false;
                if (qmgr_is_batch_registered()) {
                    push_reporting_subdoc();   // batch mode
                }
		int count1 = payload_list.size();

                char** payload_array = new char*[count1];

                for (int i = 0; i < count1; i++) {
                    payload_array[i] = strdup(payload_list[i].c_str());
                }
		// Here send the t2 event for all linkquality and connectedaffinity scores
	        qmgr_invoke_t2_callback(payload_array, count1,rms_lq_score,rms_caffinity_score,rms_ucaffinity_score);
		for (int i = 0; i < count1; i++) {
                    free(payload_array[i]);
                }
                delete[] payload_array;
	    }
	    payload_list.clear();
            pthread_mutex_lock(&m_lock);
        } else {
            wifi_util_error_print(WIFI_APPS,"%s:%d em exited with rc - %d",__func__,__LINE__,rc);
            pthread_mutex_unlock(&m_lock);
            return -1;
        }
    }
    pthread_mutex_unlock(&m_lock);
    return 0;
}



void qmgr_t::deinit()
{
    m_exit = true;
    pthread_cond_signal(&m_cond);

    // Wait for thread to finish
    pthread_join(m_thread, NULL);
    pthread_cond_destroy(&m_cond);
    
    // Clean up caffinity map
    std::unordered_map<std::string, caffinity_t*>::iterator caff_it;
    for (caff_it = m_caffinity_map.begin(); caff_it != m_caffinity_map.end(); ++caff_it) {
        delete caff_it->second;
    }
    m_caffinity_map.clear();
    
    hash_map_destroy(m_link_map);
    wifi_util_info_print(WIFI_APPS," %s:%d\n",__func__,__LINE__);
    return;
}

void qmgr_t::deinit(mac_addr_str_t mac_str)
{
    wifi_util_info_print(WIFI_APPS," %s:%d\n",__func__,__LINE__);
    return;
}

int qmgr_t::reinit(server_arg_t *args)
{
    linkq_t *lq = NULL;
    if (args){
        wifi_util_info_print(WIFI_APPS," %s:%d sampling=%d args->reporting =%d args->threshold=%f\n"
	, __func__,__LINE__,args->sampling,args->reporting,args->threshold); 
    } else {
        wifi_util_info_print(WIFI_APPS," %s:%d err\n", __func__,__LINE__); 
        return -1;
    }
   
    memcpy(&m_args, args, sizeof(server_arg_t));
    int count = hash_map_count(m_link_map);
    wifi_util_info_print(WIFI_APPS," count=%d\n",count);
    lq = (linkq_t *)hash_map_get_first(m_link_map);
    while ((lq != NULL)) {
        if (count > 0){
            lq->reinit(args);
            lq = (linkq_t *)hash_map_get_next(m_link_map, lq);
            count--;
        }
    }
    return 0;
}
bool qmgr_t::is_client_connected(const char *mac_str)
{
    if (!mac_str) {
        return false;
    }
    
    std::string mac_key(mac_str);
    pthread_mutex_lock(&m_json_lock);
    
    std::unordered_map<std::string, caffinity_t*>::iterator it = m_caffinity_map.find(mac_key);
    bool is_connected = false;
    
    if (it != m_caffinity_map.end() && it->second) {
        is_connected = it->second->get_connected();
    }
    
    pthread_mutex_unlock(&m_json_lock);
    
    wifi_util_dbg_print(WIFI_CTRL, "CAFF %s:%d MAC %s is_connected=%d\n",
        __func__, __LINE__, mac_str, is_connected);
    
    return is_connected;
}



int qmgr_t::init(stats_arg_t *stats, bool create_flag)
{
    char tmp[MAX_FILE_NAME_SZ];
    cJSON *dev_arr;
    mac_addr_str_t mac_str;

    strncpy(mac_str, stats->mac_str, sizeof(mac_str) - 1);
    mac_str[sizeof(mac_str) - 1] = '\0';

    snprintf(tmp, sizeof(tmp), "Devices");
    pthread_mutex_lock(&m_json_lock);
    if (!create_flag) {
        // remove from hashmap
        wifi_util_info_print(WIFI_APPS," %s need to be removed \n", mac_str);
        linkq_t *lq = (linkq_t *)hash_map_get(m_link_map, mac_str);
        if (lq) {
                hash_map_remove(m_link_map, mac_str);
                delete lq;
            }  else {
            wifi_util_info_print(WIFI_APPS,"Device %s not found, nothing to delete\n", mac_str);
        }
        pthread_mutex_unlock(&m_json_lock);
        return 0;
    } else {
        // Create linkq_t object and add to hashmap
	linkq_t *lq = (linkq_t *)hash_map_get(m_link_map, mac_str);
	if (!lq) {
            lq = new linkq_t(mac_str, stats->vap_index);
            if (lq) {
                lq->init(m_args.threshold, m_args.reporting, stats);
                hash_map_put(m_link_map, strdup(mac_str), lq);
                wifi_util_info_print(WIFI_APPS,"Added linkq_t for %s to m_link_map\n", mac_str);
            }
        } else {
            lq->init(m_args.threshold, m_args.reporting, stats);
            wifi_util_dbg_print(WIFI_APPS,"Updated stats for existing device %s\n", mac_str);
        }
    }

    pthread_mutex_unlock(&m_json_lock);
    return 0;
}
int qmgr_t::rapid_disconnect(stats_arg_t *stats)
{
    wifi_util_info_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    if (!stats || stats->mac_str[0] == '\0') {
        wifi_util_error_print(WIFI_APPS, "%s:%d invalid stats or empty MAC\n", __func__, __LINE__);
        return -1;
    }
    mac_addr_str_t mac_str;

    strncpy(mac_str, stats->mac_str, sizeof(mac_str) - 1);
    mac_str[sizeof(mac_str) - 1] = '\0';
    wifi_util_info_print(WIFI_APPS,"%s:%d mac_str=%s\n",__func__,__LINE__,mac_str);

    pthread_mutex_lock(&m_json_lock);
    linkq_t *lq = (linkq_t *)hash_map_get(m_link_map, mac_str);
    if (lq) {
        lq->rapid_disconnect(stats);   
        wifi_util_dbg_print(WIFI_APPS,"%s:%d rapid_disconnect called for mac_str=%s\n",__func__,__LINE__,mac_str);
    }
    pthread_mutex_unlock(&m_json_lock);
    return 0;
}

int qmgr_t::caffinity_periodic_stats_update(stats_arg_t *stats)
{
    wifi_util_info_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    if (!stats || stats->mac_str[0] == '\0') {
        wifi_util_error_print(WIFI_APPS, "%s:%d invalid stats or empty MAC\n", __func__, __LINE__);
        return -1;
    }

    mac_addr_str_t mac_str;
    strncpy(mac_str, stats->mac_str, sizeof(mac_str) - 1);
    mac_str[sizeof(mac_str) - 1] = '\0';
#if 0
    wifi_util_info_print(WIFI_APPS,"%s:%d mac_str=%s connected_time=%ld.%09ld disconnected_time=%ld.%09ld cli_SNR=%d\n",
        __func__, __LINE__, mac_str,
        (long)stats->total_connected_time.tv_sec, stats->total_connected_time.tv_nsec,
        (long)stats->total_disconnected_time.tv_sec, stats->total_disconnected_time.tv_nsec,
        stats->dev.cli_SNR);
#endif
    pthread_mutex_lock(&m_json_lock);

    // Find or create caffinity_t object for this MAC
    std::string mac_key(mac_str);
    std::unordered_map<std::string, caffinity_t*>::iterator it = m_caffinity_map.find(mac_key);
    caffinity_t *caff = NULL;

    if (it == m_caffinity_map.end()) {
        // Create new caffinity object for this MAC
        wifi_util_info_print(WIFI_CTRL, "CAFF qmgr_t %s:%d Creating new caffinity_t for MAC %s\n",
            __func__, __LINE__, mac_str);
        mac_addr_str_t mac_str_array;
        strncpy(mac_str_array, mac_str, sizeof(mac_str_array) - 1);
        mac_str_array[sizeof(mac_str_array) - 1] = '\0';
        caff = new caffinity_t(&mac_str_array);
        if (caff) {
            m_caffinity_map[mac_key] = caff;
            wifi_util_dbg_print(WIFI_APPS, "CAFF qmgr_t %s:%d Successfully created caffinity_t for MAC %s\n",
                __func__, __LINE__, mac_str);
        }
    } else {
        caff = it->second;
    }

    if (caff) {
        caff->periodic_stats_update(stats);
    }

    pthread_mutex_unlock(&m_json_lock);
    return 0;
}


// static helper function for pthread
void* qmgr_t::run_helper(void* arg)
{
    wifi_util_info_print(WIFI_APPS," %s:%d\n",__func__,__LINE__);
    qmgr_t* mgr = static_cast<qmgr_t*>(arg);
    if (mgr) {
        wifi_util_info_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
        mgr->run();
    }
    return NULL;
}

void qmgr_t::start_background_run()
{
    wifi_util_info_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    if (m_bg_running) {
        return;   // already running
    }
    m_bg_running = true;
    int ret = pthread_create(&m_thread, NULL, run_helper, this);
    if (ret != 0) {
        wifi_util_info_print(WIFI_APPS,"Failed to create background run thread\n");
    } else {
        wifi_util_info_print(WIFI_APPS,"created background run thread\n");
    }
    return;
}

char *qmgr_t::get_local_time(char *str, unsigned int len, bool hourformat)
{
    struct timeval tv;
    struct tm *local_time;
    
    gettimeofday(&tv, NULL); // Get current time into tv
    local_time = localtime(&tv.tv_sec);
    if(hourformat)
        strftime(str, len, "%M:%S", local_time);
    else
        strftime(str, len, "%Y-%m-%d %H:%M:%S", local_time);

    return str;
}

void qmgr_t::register_station_mac(const char* str)
{
    wifi_util_info_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    linkq_t::register_station_mac(str);
    return;
}

void qmgr_t::unregister_station_mac(const char* str)
{
    wifi_util_info_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    linkq_t::unregister_station_mac(str);
    reset_qmgr_score_cb();
    return;
}

int qmgr_t::set_max_snr_radios(radio_max_snr_t *max_snr_val)
{
    linkq_t::set_max_snr_radios(max_snr_val);
    return 0;   
}

qmgr_t::qmgr_t()
{
    memset(&m_args, 0, sizeof(server_arg_t));
    m_args.threshold = THRESHOLD;
    m_args.sampling = SAMPLING_INTERVAL;
    m_args.reporting = REPORTING_INTERVAL;
    snprintf(m_args.output_file, sizeof(m_args.output_file), "%s", "/www/data/telemetry.json");
    snprintf(m_args.path, sizeof(m_args.path), "%s", "/www/data");
    m_link_map = hash_map_create();
    
    
    // Initialize RMS tracking variables
    m_rms_conn_sum_sq = 0.0;
    m_rms_conn_count = 0;
    m_rms_unconn_sum_sq = 0.0;
    m_rms_unconn_count = 0;
    m_rms_lq_sum_sq = 0.0;
    m_rms_lq_count = 0;
    
    m_bg_running = false;
    m_exit = false;
    pthread_mutex_init(&m_json_lock, NULL);
    pthread_mutex_init(&m_lock, NULL);
    pthread_cond_init(&m_cond, NULL);
}

qmgr_t::qmgr_t(server_arg_t *args,stats_arg_t *stats)
{
    memcpy(&m_args, args, sizeof(server_arg_t));
    memcpy(&m_stats, stats, sizeof(stats_arg_t));
    
    // Initialize caffinity telemetry JSON with future-proof structure
    
    // Initialize RMS tracking variables
    m_rms_conn_sum_sq = 0.0;
    m_rms_conn_count = 0;
    m_rms_unconn_sum_sq = 0.0;
    m_rms_unconn_count = 0;
    m_rms_lq_sum_sq = 0.0;
    m_rms_lq_count = 0;
    
    m_exit = false;
    m_bg_running = false;
    m_link_map = hash_map_create();
    pthread_mutex_init(&m_json_lock, NULL);
    pthread_mutex_init(&m_lock, NULL);
    pthread_cond_init(&m_cond, NULL);
}
void qmgr_t::destroy_instance()
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    wifi_util_info_print(WIFI_APPS," %s:%d\n",__func__,__LINE__);

    if (instance) {
        instance->deinit();    // cleanup internal resources
        delete instance;       // call destructor
        instance = NULL;
    }

    pthread_mutex_unlock(&lock);
    return;
}

int qmgr_t::set_quality_flags(quality_flags_t *flag)
{
    linkq_t::set_quality_flags(flag);
    return 0;
}

int qmgr_t::get_quality_flags(quality_flags_t *flag)
{
    linkq_t::get_quality_flags(flag);
    return 0;
}
qmgr_t::~qmgr_t()
{
}
int qmgr_t::store_gw_mac(uint8_t *mac) 
{
    wifi_util_info_print(WIFI_APPS," %s:%d\n",__func__,__LINE__);
    memcpy(m_gw_mac,mac,sizeof(m_gw_mac));
   return 0;
}
int qmgr_t::get_gw_mac(uint8_t *mac)
{
    wifi_util_info_print(WIFI_APPS," %s:%d\n",__func__,__LINE__);
    if (!mac) {
        return -1;
    }

    memcpy(mac, m_gw_mac, sizeof(m_gw_mac));
    return 0;
}

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
 * linkquality_stats – standalone daemon that receives link-quality events
 * from OneWifi via rbus and drives the quality-manager (qmgr) library.
 *
 * Daemon pattern follows rdkb_daemonize() in
 *   ccsp-one-wifi/source/platform/rdkb/misc.c:
 *   - Named POSIX semaphore synchronises parent exit with child init.
 *   - fork() → child calls setsid(), redirects stdio → /dev/null.
 *   - Child signals semaphore after successful init so the parent
 *     (waited on by systemd Type=forking) can exit cleanly.
 *   - PID file written by child for systemd PIDFile= tracking.
 */

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include "linkquality_stats_rbus.h"
#include "run_qmgr.h"
#include "linkquality_util.h"
#include "lq_ipc_receiver.h"

#define COMPONENT_NAME          "linkquality_stats"
#define LQ_PID_FILE             "/var/tmp/linkquality_stats.pid"
#define LQ_SEM_NAME             "pSemLQStats"
#define SSP_LOOP_TIMEOUT_SEC    30

static volatile sig_atomic_t g_running     = 1;
static pthread_mutex_t       g_loop_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t        g_loop_cond   = PTHREAD_COND_INITIALIZER;
static sem_t                *g_sem         = SEM_FAILED;

/* ------------------------------------------------------------------ */
/*  Signal handler                                                     */
/* ------------------------------------------------------------------ */

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
    pthread_cond_signal(&g_loop_cond);
}

/* ------------------------------------------------------------------ */
/*  PID file                                                           */
/* ------------------------------------------------------------------ */

static void lq_write_pid_file(void)
{
    FILE *fp = fopen(LQ_PID_FILE, "w");
    if (!fp) {
        lq_util_error_print(LQ_LQTY, "%s:%d failed to open pid file %s: %s\n",
                            __func__, __LINE__, LQ_PID_FILE, strerror(errno));
        return;
    }
    fprintf(fp, "%d\n", getpid());
    fclose(fp);
}

/* ------------------------------------------------------------------ */
/*  Daemonize (rdkb_daemonize pattern from misc.c)                    */
/* ------------------------------------------------------------------ */

static void lq_daemonize(void)
{
    int fd;

    /*
     * Named semaphore: parent blocks here until child signals that
     * init has completed.  Unlinked immediately so a crash does not
     * leave a stale named semaphore blocking the next start.
     */
    g_sem = sem_open(LQ_SEM_NAME, O_CREAT | O_EXCL, 0644, 0);
    if (g_sem == SEM_FAILED) {
        lq_util_error_print(LQ_LQTY, "%s:%d sem_open failed: %d - %s\n",
                            __func__, __LINE__, errno, strerror(errno));
        _exit(1);
    }
    sem_unlink(LQ_SEM_NAME);

    switch (fork()) {
    case 0:
        /* child — fall through and continue daemon execution */
        break;
    case -1:
        lq_util_error_print(LQ_LQTY, "%s:%d fork failed: %d - %s\n",
                            __func__, __LINE__, errno, strerror(errno));
        sem_close(g_sem);
        _exit(1);
    default:
        /* parent — wait for child to confirm init, then exit */
        sem_wait(g_sem);
        sem_close(g_sem);
        _exit(0);
    }

    /* Child: create new session, detach from controlling terminal */
    if (setsid() < 0) {
        lq_util_error_print(LQ_LQTY, "%s:%d setsid failed: %d - %s\n",
                            __func__, __LINE__, errno, strerror(errno));
        sem_post(g_sem);
        sem_close(g_sem);
        _exit(1);
    }

    /* Redirect stdio to /dev/null */
    fd = open("/dev/null", O_RDONLY);
    if (fd != -1 && fd != STDIN_FILENO)  { dup2(fd, STDIN_FILENO);  close(fd); }
    fd = open("/dev/null", O_WRONLY);
    if (fd != -1 && fd != STDOUT_FILENO) { dup2(fd, STDOUT_FILENO); close(fd); }
    fd = open("/dev/null", O_WRONLY);
    if (fd != -1 && fd != STDERR_FILENO) { dup2(fd, STDERR_FILENO); close(fd); }
}

/* ------------------------------------------------------------------ */
/*  SSP-style event loop                                               */
/*                                                                     */
/*  Uses a condition variable with a timed wait so the loop can:       */
/*    - wake immediately on SIGINT/SIGTERM (via sig_handler)           */
/*    - wake periodically every SSP_LOOP_TIMEOUT_SEC seconds for       */
/*      health-checks or future periodic work                          */
/*    - wake on-demand when an internal event is posted (future use)   */
/*                                                                     */
/*  rc from pthread_cond_timedwait:                                    */
/*    ETIMEDOUT — periodic tick, no condition posted                   */
/*    0         — condition signaled: shutdown or future event         */
/*    EINTR     — interrupted by a signal, re-evaluate g_running       */
/*    other     — unexpected error, log and continue                   */
/* ------------------------------------------------------------------ */

static void lq_loop(void)
{
    struct timespec ts;
    int rc;

    pthread_mutex_lock(&g_loop_mutex);

    while (g_running) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += SSP_LOOP_TIMEOUT_SEC;

        rc = pthread_cond_timedwait(&g_loop_cond, &g_loop_mutex, &ts);

        /* Check shutdown first — takes priority over everything else */
        if (!g_running) {
            break;
        }

        switch (rc) {
        case ETIMEDOUT:
            /*
             * Periodic wakeup — placeholder for future periodic tasks:
             * e.g. health-check, telemetry flush, rbus reconnect probe.
             */
            lq_util_dbg_print(LQ_LQTY,
                "%s:%d ssp_loop: periodic tick (every %ds)\n",
                __func__, __LINE__, SSP_LOOP_TIMEOUT_SEC);
            break;

        case 0:
            /*
             * Condition was signaled while g_running is still true.
             * Placeholder for future in-process event dispatch.
             */
            lq_util_dbg_print(LQ_LQTY,
                "%s:%d ssp_loop: event wakeup\n", __func__, __LINE__);
            break;

        case EINTR:
            /* Signal interrupted the wait — re-evaluate g_running */
            break;

        default:
            lq_util_error_print(LQ_LQTY,
                "%s:%d ssp_loop: pthread_cond_timedwait error rc=%d (%s)\n",
                __func__, __LINE__, rc, strerror(rc));
            break;
        }
    }

    pthread_mutex_unlock(&g_loop_mutex);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /*
     * Daemonize first: fork(), setsid(), stdio→/dev/null.
     * Parent blocks on semaphore until child signals after init.
     * From this point on we are running in the child process.
     */
    lq_daemonize();

    /* Write PID file before anything else so systemd can track us */
    lq_write_pid_file();

    lq_util_info_print(LQ_LQTY, "%s:%d starting linkquality_stats daemon (pid=%d)\n",
                       __func__, __LINE__, getpid());

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);
#if 0
    if (lq_stats_rbus_init() != 0) {
        lq_util_error_print(LQ_LQTY, "%s:%d rbus init failed\n", __func__, __LINE__);
        sem_post(g_sem);
        sem_close(g_sem);
        unlink(LQ_PID_FILE);
        return EXIT_FAILURE;
    }
#endif
    /* Start the embedded webserver (port 8082).
     * Flow: main -> run_web_server -> web_t::start -> accept loop thread. */
    run_web_server();

    /* Post the initial status message into the webserver.
     * Flow: main -> post_web_message -> web_t::set_message
     *    -> served at GET /api/status -> displayed in index.html. */
    post_web_message("from main");

    /* Start background link-quality metrics collection (single call) */
    start_link_metrics();

    /* Start IPC receiver for AF_UNIX events from OneWifi */
    if (lq_ipc_receiver_start() != 0) {
        lq_util_error_print(LQ_LQTY, "%s:%d IPC receiver start failed\n", __func__, __LINE__);
    }

    /* Signal parent: init done, parent (systemd) can exit cleanly */
    sem_post(g_sem);
    sem_close(g_sem);
    g_sem = SEM_FAILED;

    /* Event-driven: rbus dispatches callbacks on its own threads.
     * lq_loop() keeps the daemon alive with periodic + on-demand wakeups. */
    lq_loop();

    lq_util_info_print(LQ_LQTY, "%s:%d shutting down linkquality_stats\n",
                       __func__, __LINE__);
    lq_ipc_receiver_stop();
    unlink(LQ_PID_FILE);

    return EXIT_SUCCESS;
}

/***************************************************************************
 *      ____  _____________  __    __  __ _           _____ ___   _        *
 *     / __ \/ ____/ ___/\ \/ /   |  \/  (_)__ _ _ __|_   _/ __| /_\  (R)  *
 *    / / / / __/  \__ \  \  /    | |\/| | / _| '_/ _ \| || (__ / _ \      *
 *   / /_/ / /___ ___/ /  / /     |_|  |_|_\__|_| \___/|_| \___/_/ \_\     *
 *  /_____/_____//____/  /_/      T  E  C  H  N  O  L  O  G  Y   L A B     *
 *                                                                         *
 *          Copyright 2022 Deutsches Elektronen-Synchrotron DESY.          *
 *                          All rights reserved.                           *
 *                                                                         *
 ***************************************************************************/

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "mmcmb/mmcmb.h"

// Poll FPGA control register 4 times per second.
#define POLL_INTERVAL_MS 250

static bool terminate = false;

static void sigterm_handler(int signum)
{
    (void)signum;
    terminate = true;
}

static void daemonize()
{
    pid_t pid = fork();

    if (pid < 0) {
        exit(EXIT_FAILURE);
    } else if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }

    struct sigaction action = {
        .sa_handler = SIG_IGN,
    };
    sigaction(SIGCHLD, &action, NULL);
    sigaction(SIGHUP, &action, NULL);

    action.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &action, NULL);

    pid = fork();

    if (pid < 0) {
        exit(EXIT_FAILURE);
    } else if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    umask(0);

    if (chdir("/") < 0) {
        perror("chdir");
        exit(EXIT_FAILURE);
    }

    for (int x = sysconf(_SC_OPEN_MAX); x >= 0; x--) {
        close(x);
    }

    openlog("mmcctrld", LOG_PID, LOG_DAEMON);
}

void handle_fpga_ctrl(const mb_fpga_ctrl_t* ctrl)
{
    if (!ctrl->req_shutdown) {
        return;
    }
    syslog(LOG_NOTICE, "Shutdown requested by MMC");

    execl("/sbin/shutdown", "shutdown", "-h", "now", (char*)NULL);

    syslog(LOG_ERR, "Could not execute shutdown command: %s", strerror(errno));
    terminate = true;
}

int main()
{
    if (geteuid() != 0) {
        fprintf(stderr, "mmcctrld: needs to be launched with root privileges\r\n");
        return 1;
    }

    daemonize();

    const char* eeprom = mb_get_eeprom_path();
    if (eeprom != NULL) {
        syslog(LOG_NOTICE, "Opened mailbox at %s", eeprom);
    } else {
        syslog(LOG_ERR, "Could not open mailbox");
        goto finish;
    }
    if (!mb_check_magic()) {
        syslog(LOG_ERR, "Mailbox not available");
        goto finish;
    }

    const mb_fpga_status_t stat = {
        .app_startup_finished = true,
    };
    if (!mb_set_fpga_status(&stat)) {
        syslog(LOG_ERR, "Could not set FPGA status");
        goto finish;
    }

    const struct timespec ts_poll = {
        .tv_nsec = POLL_INTERVAL_MS * 1e6,
    };

    syslog(LOG_NOTICE, "Started");

    while (!terminate) {
        mb_fpga_ctrl_t ctrl;
        if (!mb_get_fpga_ctrl(&ctrl)) {
            syslog(LOG_ERR, "Could not read FPGA_CTRL");
            break;
        }
        handle_fpga_ctrl(&ctrl);
        nanosleep(&ts_poll, NULL);
    }

finish:
    syslog(LOG_NOTICE, "Terminated");
    closelog();

    return EXIT_SUCCESS;
}

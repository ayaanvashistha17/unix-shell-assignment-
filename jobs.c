
#include "jobs.h"
#include <sys/wait.h>
#include <string.h>
#include <sys/types.h>
#include <stdio.h>

job_t g_jobs[MAX_JOBS] = {0};

int add_job(pid_t pid, const char *cmdline) {
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (!g_jobs[i].active) {
            g_jobs[i].pid    = pid;
            g_jobs[i].active = 1;
            // keep it short; avoid overflow
            snprintf(g_jobs[i].cmdline, sizeof(g_jobs[i].cmdline), "%s", cmdline ? cmdline : "");
            return i;
        }
    }
    fprintf(stderr, "jobs: job table full\n");
    return -1;
}

void reap_finished_jobs(void) {
    int status;
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (!g_jobs[i].active) continue;
        pid_t r = waitpid(g_jobs[i].pid, &status, WNOHANG);
        if (r == g_jobs[i].pid) {
            // child changed state (exited or was signalled)
            g_jobs[i].active = 0;
        }
    }
}

void print_jobs(void) {
    // refresh first so we don't show dead processes
    reap_finished_jobs();

    int any = 0;
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (g_jobs[i].active) {
            any = 1;
            printf("[%d] Running  %s\n", g_jobs[i].pid, g_jobs[i].cmdline[0] ? g_jobs[i].cmdline : "(unknown)");
        }
    }
    if (!any) printf("(no background jobs)\n");
}

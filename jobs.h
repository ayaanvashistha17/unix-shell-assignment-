#pragma once
#include <sys/types.h>

#define MAX_JOBS 64

typedef struct {
    pid_t pid;
    int   active;              // 1 = running, 0 = finished/empty
    char  cmdline[256];        
} job_t;

extern job_t g_jobs[MAX_JOBS];

// Add a job; returns index or -1 if full
int add_job(pid_t pid, const char *cmdline);

// Reap any finished background jobs (non-blocking)
void reap_finished_jobs(void);


void print_jobs(void);

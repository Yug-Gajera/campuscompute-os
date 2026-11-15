/*
 * jobs.c - FR1: job representation, input validation and lifecycle status.
 *
 * Job file format (one job per line, '#' starts a comment):
 *     <job_id> <arrival_time> <cpu_burst> <priority>
 * Rules:
 *     job_id     : positive integer, unique within the file
 *     arrival    : 0 .. MAX_ARRIVAL
 *     cpu_burst  : 1 .. MAX_BURST
 *     priority   : 0 .. MAX_PRIORITY  (lower number = higher priority)
 * Every malformed line is reported with its line number; if any line is
 * invalid the whole file is rejected so scheduling never runs on bad data.
 */
#include "campus.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

const char *job_status_name(JobStatus s)
{
    switch (s) {
    case JOB_NEW:     return "NEW";
    case JOB_READY:   return "READY";
    case JOB_RUNNING: return "RUNNING";
    case JOB_DONE:    return "COMPLETED";
    }
    return "?";
}

/* Put every job back into the NEW state with no execution history. */
void jobs_reset(Job *jobs, int count)
{
    for (int i = 0; i < count; i++) {
        jobs[i].remaining  = jobs[i].burst;
        jobs[i].start      = -1;
        jobs[i].completion = 0;
        jobs[i].turnaround = 0;
        jobs[i].waiting    = 0;
        jobs[i].response   = 0;
        jobs[i].status     = JOB_NEW;
    }
}

static int check_field(const char *path, int lineno, const char *name,
                       const char *tok, int min, int max, int *out)
{
    if (parse_int(tok, min, max, out) == 0)
        return 0;
    fprintf(stderr, "  %s:%d: %s must be an integer in [%d, %d] (got '%s')\n",
            path, lineno, name, min, max, tok);
    return -1;
}

int jobs_load(const char *path, Job *jobs, int *count)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "error: cannot open job file '%s': %s\n", path, strerror(errno));
        return -1;
    }

    char line[512];
    int  lineno = 0, n = 0, errors = 0;

    while (fgets(line, sizeof line, fp) != NULL) {
        lineno++;
        if (strchr(line, '\n') == NULL && !feof(fp)) {
            fprintf(stderr, "  %s:%d: line too long\n", path, lineno);
            errors++;
            int c;
            while ((c = fgetc(fp)) != '\n' && c != EOF) { }
            continue;
        }
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char *tok[6];
        int   ntok = 0;
        char *save = NULL;
        for (char *t = strtok_r(line, " \t\r\n,", &save); t && ntok < 6;
             t = strtok_r(NULL, " \t\r\n,", &save))
            tok[ntok++] = t;

        if (ntok == 0)
            continue;                         /* blank or comment-only line */
        if (ntok != 4) {
            fprintf(stderr, "  %s:%d: expected 4 fields <id> <arrival> <burst> <priority>, found %d\n",
                    path, lineno, ntok);
            errors++;
            continue;
        }
        if (n >= MAX_JOBS) {
            fprintf(stderr, "  %s:%d: too many jobs (maximum %d)\n", path, lineno, MAX_JOBS);
            errors++;
            break;
        }

        Job j;
        memset(&j, 0, sizeof j);
        int bad = 0;
        bad |= check_field(path, lineno, "job ID",       tok[0], 1, 999999, &j.id);
        bad |= check_field(path, lineno, "arrival time", tok[1], 0, MAX_ARRIVAL, &j.arrival);
        bad |= check_field(path, lineno, "CPU burst",    tok[2], 1, MAX_BURST, &j.burst);
        bad |= check_field(path, lineno, "priority",     tok[3], 0, MAX_PRIORITY, &j.priority);
        if (bad) { errors++; continue; }

        for (int k = 0; k < n; k++) {
            if (jobs[k].id == j.id) {
                fprintf(stderr, "  %s:%d: duplicate job ID %d\n", path, lineno, j.id);
                bad = 1;
                break;
            }
        }
        if (bad) { errors++; continue; }

        jobs[n++] = j;
    }
    fclose(fp);

    if (errors > 0) {
        fprintf(stderr, "error: '%s' rejected - %d invalid line(s). Nothing was scheduled.\n",
                path, errors);
        return -1;
    }
    if (n == 0) {
        fprintf(stderr, "error: '%s' contains no jobs\n", path);
        return -1;
    }
    jobs_reset(jobs, n);
    *count = n;
    return 0;
}

void jobs_print(const Job *jobs, int count)
{
    printf("  %-6s %-8s %-6s %-9s %-10s\n", "Job", "Arrival", "Burst", "Priority", "Status");
    printf("  %-6s %-8s %-6s %-9s %-10s\n", "------", "--------", "------", "---------", "----------");
    for (int i = 0; i < count; i++)
        printf("  J%-5d %-8d %-6d %-9d %-10s\n", jobs[i].id, jobs[i].arrival,
               jobs[i].burst, jobs[i].priority, job_status_name(jobs[i].status));
    printf("  %d job(s) loaded and validated.\n", count);
}

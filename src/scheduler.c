/*
 * scheduler.c - FR2: FCFS, Round Robin and non-preemptive Priority scheduling.
 *
 * All three algorithms are discrete-time simulations over a copy of the job
 * set, so the caller's jobs are never modified and the same input can be
 * compared across policies.
 *
 * Tie-breaking rules (applied consistently everywhere):
 *   FCFS     : earlier arrival first; equal arrival -> smaller job ID first.
 *   Priority : smaller priority number first; equal priority -> earlier
 *              arrival; still equal -> smaller job ID. Non-preemptive: once a
 *              job starts it runs to completion.
 *   RR       : ready queue is FIFO. Jobs arriving at the same instant join in
 *              job-ID order. A job that arrives exactly when another job's
 *              quantum expires is queued BEFORE the preempted job.
 * When no job is ready the CPU is shown as IDLE until the next arrival.
 *
 * Metrics: CT = completion, TAT = CT - arrival, WT = TAT - burst,
 *          RT (response) = first start - arrival.
 */
#include "campus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ---------------------------------------------------------- */

/*
 * Start a fresh result: copy the caller's jobs so the original array is never
 * changed, then put every copied job back into the NEW state.
 */
static void result_init(ScheduleResult *r, const char *name, const Job *jobs, int n)
{
    memset(r, 0, sizeof *r);
    snprintf(r->name, sizeof r->name, "%s", name);
    memcpy(r->jobs, jobs, (size_t)n * sizeof *jobs);
    r->njobs = n;
    jobs_reset(r->jobs, n);
}

/* Append a slice to the timeline, merging with the previous slice if it is the
 * same job running back-to-back (e.g. RR with a single ready job). */
static int add_slice(ScheduleResult *r, int job_id, int start, int end)
{
    if (end <= start)
        return 0;
    if (r->nslices > 0) {
        Slice *last = &r->slices[r->nslices - 1];
        /* Same job continuing right after itself -> just extend the last bar. */
        if (last->job_id == job_id && last->end == start) {
            last->end = end;
            return 0;
        }
    }
    if (r->nslices >= MAX_SLICES) {
        fprintf(stderr, "error: timeline too long (more than %d slices)\n", MAX_SLICES);
        return -1;
    }
    r->slices[r->nslices++] = (Slice){ job_id, start, end };
    return 0;
}

/* Comparator: arrival time, then job ID. */
/*
 * qsort() only passes two pointers to the comparator, so the job array it
 * needs to look at is kept in this file-level pointer while sorting.
 */
static const Job *g_sort_jobs;
static int cmp_arrival_then_id(const void *a, const void *b)
{
    const Job *x = &g_sort_jobs[*(const int *)a];
    const Job *y = &g_sort_jobs[*(const int *)b];
    if (x->arrival != y->arrival) return x->arrival - y->arrival;
    return x->id - y->id;
}

/*
 * Build idx[] = job indices sorted by (arrival, ID). The jobs themselves are
 * not moved; every policy walks this list to know who arrives next.
 */
static void order_by_arrival(const Job *jobs, int n, int *idx)
{
    for (int i = 0; i < n; i++) idx[i] = i;
    g_sort_jobs = jobs;
    qsort(idx, (size_t)n, sizeof *idx, cmp_arrival_then_id);
}

/*
 * Lifecycle step NEW -> READY: every job whose arrival time is <= t has
 * "arrived", so mark it READY. *next remembers how far we got in the sorted
 * arrival list, so each job is admitted exactly once.
 */
static void mark_ready_until(Job *jobs, const int *order, int n, int *next, int t,
                             const char *policy)
{
    while (*next < n && jobs[order[*next]].arrival <= t) {
        Job *j = &jobs[order[*next]];
        j->status = JOB_READY;
        logger_log(policy, "t=%d J%d arrived -> READY", j->arrival, j->id);
        (*next)++;
    }
}

/*
 * Lifecycle step READY -> RUNNING. The first time a job gets the CPU we
 * record its start time; response time = first start - arrival.
 */
static void run_job(Job *j, int t, const char *policy)
{
    if (j->start < 0) {
        j->start    = t;
        j->response = t - j->arrival;
    }
    j->status = JOB_RUNNING;
    logger_log(policy, "t=%d J%d -> RUNNING", t, j->id);
}

/*
 * Lifecycle step RUNNING -> COMPLETED, and the per-job metrics:
 *   turnaround = completion - arrival   (total time in the system)
 *   waiting    = turnaround - burst     (time spent ready but not running)
 */
static void complete_job(Job *j, int t, const char *policy)
{
    j->completion = t;
    j->turnaround = t - j->arrival;
    j->waiting    = j->turnaround - j->burst;
    j->status     = JOB_DONE;
    logger_log(policy, "t=%d J%d -> COMPLETED (TAT=%d WT=%d)", t, j->id, j->turnaround, j->waiting);
}

/* Fill in averages, makespan, utilisation and context switches. */
static void finalize(ScheduleResult *r)
{
    double tat = 0, wt = 0, rt = 0;
    for (int i = 0; i < r->njobs; i++) {
        tat += r->jobs[i].turnaround;
        wt  += r->jobs[i].waiting;
        rt  += r->jobs[i].response;
    }
    r->avg_tat = tat / r->njobs;
    r->avg_wt  = wt  / r->njobs;
    r->avg_rt  = rt  / r->njobs;

    /*
     * Walk the timeline: add up busy (non-idle) time, and count a context
     * switch every time the CPU moves from one job to a different job.
     */
    int prev = -1;
    r->busy_time = 0;
    r->context_switches = 0;
    for (int i = 0; i < r->nslices; i++) {
        const Slice *s = &r->slices[i];
        if (s->job_id < 0) continue;
        r->busy_time += s->end - s->start;
        if (prev != -1 && prev != s->job_id) r->context_switches++;
        prev = s->job_id;
    }
    r->makespan = r->nslices ? r->slices[r->nslices - 1].end : 0;
    logger_log("SCHED", "%s finished: avgTAT=%.2f avgWT=%.2f avgRT=%.2f", r->name,
               r->avg_tat, r->avg_wt, r->avg_rt);
}

/* ---- FCFS --------------------------------------------------------------- */

int schedule_fcfs(const Job *jobs, int n, ScheduleResult *r)
{
    result_init(r, "FCFS", jobs, n);
    int order[MAX_JOBS];
    order_by_arrival(r->jobs, n, order);

    int t = 0, next = 0;
    /* FCFS: take jobs strictly in arrival order; each runs its whole burst. */
    for (int k = 0; k < n; k++) {
        /* next job in arrival order (ties already broken by smaller ID) */
        Job *j = &r->jobs[order[k]];
        if (t < j->arrival) {                       /* nothing ready: CPU idle */
            if (add_slice(r, -1, t, j->arrival)) return -1;
            t = j->arrival;
        }
        mark_ready_until(r->jobs, order, n, &next, t, "FCFS");
        run_job(j, t, "FCFS");
        /* the job holds the CPU for its entire burst - no preemption */
        if (add_slice(r, j->id, t, t + j->burst)) return -1;
        t += j->burst;
        j->remaining = 0;
        mark_ready_until(r->jobs, order, n, &next, t, "FCFS");
        complete_job(j, t, "FCFS");
    }
    finalize(r);
    return 0;
}

/* ---- Round Robin ---------------------------------------------------------- */

int schedule_rr(const Job *jobs, int n, int quantum, ScheduleResult *r)
{
    if (quantum < 1) {
        fprintf(stderr, "error: time quantum must be >= 1\n");
        return -1;
    }
    char name[40];
    snprintf(name, sizeof name, "Round Robin (q=%d)", quantum);
    result_init(r, name, jobs, n);
    r->quantum = quantum;

    int order[MAX_JOBS];
    order_by_arrival(r->jobs, n, order);

    /*
     * The ready queue is a circular array of indices into r->jobs: push at tail, pop at head.
     * One extra slot lets 'head == tail' mean empty. A job is in the queue at
     * most once, so MAX_JOBS + 1 slots can never overflow.
     */
    int queue[MAX_JOBS + 1], head = 0, tail = 0;
#define RQ_PUSH(x) do { queue[tail] = (x); tail = (tail + 1) % (MAX_JOBS + 1); } while (0)
#define RQ_EMPTY() (head == tail)

    int t = 0, next = 0, done = 0;
    /*
     * Each loop iteration = one dispatch: admit arrivals, pick the job at the
     * front of the queue, run it for min(quantum, remaining) time units.
     */
    while (done < n) {
        /* admit everything that has arrived by time t */
        while (next < n && r->jobs[order[next]].arrival <= t) {
            Job *a = &r->jobs[order[next]];
            a->status = JOB_READY;
            logger_log("RR", "t=%d J%d arrived -> READY", a->arrival, a->id);
            RQ_PUSH(order[next]);
            next++;
        }
        if (RQ_EMPTY()) {                           /* idle until next arrival */
            int na = r->jobs[order[next]].arrival;
            if (add_slice(r, -1, t, na)) return -1;
            t = na;
            continue;
        }

        int idx = queue[head];                      /* dequeue front of ready queue */
        head = (head + 1) % (MAX_JOBS + 1);
        Job *j = &r->jobs[idx];
        run_job(j, t, "RR");
        /* run a full quantum, or less if the job finishes sooner */
        int run = j->remaining < quantum ? j->remaining : quantum;
        if (add_slice(r, j->id, t, t + run)) return -1;
        t += run;
        j->remaining -= run;

        /* arrivals during/at the end of this slice enter BEFORE the preempted job */
        while (next < n && r->jobs[order[next]].arrival <= t) {
            Job *a = &r->jobs[order[next]];
            a->status = JOB_READY;
            logger_log("RR", "t=%d J%d arrived -> READY", a->arrival, a->id);
            RQ_PUSH(order[next]);
            next++;
        }

        /*
         * Unfinished job is PREEMPTED and goes to the BACK of the queue; a finished
         * job leaves the system.
         */
        if (j->remaining > 0) {
            j->status = JOB_READY;
            logger_log("RR", "t=%d J%d quantum expired, %d left -> back of queue",
                       t, j->id, j->remaining);
            RQ_PUSH(idx);
        } else {
            complete_job(j, t, "RR");
            done++;
        }
    }
#undef RQ_PUSH
#undef RQ_EMPTY
    finalize(r);
    return 0;
}

/* ---- Non-preemptive Priority --------------------------------------------- */

int schedule_priority(const Job *jobs, int n, ScheduleResult *r)
{
    result_init(r, "Priority (non-preemptive)", jobs, n);
    int order[MAX_JOBS];
    order_by_arrival(r->jobs, n, order);

    int t = 0, next = 0, done = 0;
    while (done < n) {
        mark_ready_until(r->jobs, order, n, &next, t, "PRIORITY");

        /*
         * Scan all READY jobs and keep the 'best' one:
         *   1) smaller priority number wins (lower number = higher priority)
         *   2) equal priority -> earlier arrival wins
         *   3) still equal    -> smaller job ID wins
         */
        int best = -1;
        for (int i = 0; i < n; i++) {
            Job *c = &r->jobs[i];
            if (c->status != JOB_READY) continue;
            if (best < 0) { best = i; continue; }
            Job *b = &r->jobs[best];
            if (c->priority != b->priority) { if (c->priority < b->priority) best = i; }
            else if (c->arrival != b->arrival) { if (c->arrival < b->arrival) best = i; }
            else if (c->id < b->id) best = i;
        }

        if (best < 0) {                             /* nobody ready: idle */
            int na = r->jobs[order[next]].arrival;
            if (add_slice(r, -1, t, na)) return -1;
            t = na;
            continue;
        }

        /*
         * Non-preemptive: the chosen job runs its whole burst. A higher-priority job
         * that arrives meanwhile has to wait until this one finishes.
         */
        Job *j = &r->jobs[best];
        run_job(j, t, "PRIORITY");
        if (add_slice(r, j->id, t, t + j->burst)) return -1;
        t += j->burst;
        j->remaining = 0;
        complete_job(j, t, "PRIORITY");
        done++;
    }
    finalize(r);
    return 0;
}

/* ---- output ---------------------------------------------------------------- */

/* Print a text Gantt chart, wrapping every few slices so it fits a terminal. */
static void print_gantt(const ScheduleResult *r)
{
    /*
     * Draw two text lines per row of the chart:
     *   |J1    |J2    |      <- one box per slice
     *   0      5      8      <- start time of each slice, end time at the far right
     * RR can produce many slices, so the chart wraps every 10 slices.
     */
    const int per_row = 10;
    printf("  Gantt chart:\n");
    for (int base = 0; base < r->nslices; base += per_row) {
        int lim = base + per_row < r->nslices ? base + per_row : r->nslices;
        char bar[512] = "  |", times[512] = "  ";
        size_t bl = strlen(bar), tl = strlen(times);
        for (int i = base; i < lim; i++) {
            char label[16];
            if (r->slices[i].job_id < 0) snprintf(label, sizeof label, "IDLE");
            else                         snprintf(label, sizeof label, "J%d", r->slices[i].job_id);
            int w = 6;
            bl += (size_t)snprintf(bar + bl, sizeof bar - bl, "%-*s|", w, label);
            tl += (size_t)snprintf(times + tl, sizeof times - tl, "%-*d", w + 1, r->slices[i].start);
        }
        snprintf(times + tl, sizeof times - tl, "%d", r->slices[lim - 1].end);
        printf("%s\n%s\n", bar, times);
    }
}

void schedule_print(const ScheduleResult *r)
{
    printf("\n  Policy: %s\n", r->name);

    printf("  Execution order: ");
    for (int i = 0; i < r->nslices; i++) {
        if (r->slices[i].job_id < 0) printf("IDLE[%d-%d]", r->slices[i].start, r->slices[i].end);
        else printf("J%d[%d-%d]", r->slices[i].job_id, r->slices[i].start, r->slices[i].end);
        if (i + 1 < r->nslices) printf(" -> ");
    }
    printf("\n");
    print_gantt(r);

    printf("\n  %-5s %-4s %-6s %-4s %-6s %-6s %-6s %-6s %-10s\n",
           "Job", "AT", "Burst", "Pri", "Start", "CT", "TAT", "WT", "Status");
    printf("  %-5s %-4s %-6s %-4s %-6s %-6s %-6s %-6s %-10s\n",
           "-----", "----", "------", "----", "------", "------", "------", "------", "----------");
    for (int i = 0; i < r->njobs; i++) {
        const Job *j = &r->jobs[i];
        printf("  J%-4d %-4d %-6d %-4d %-6d %-6d %-6d %-6d %-10s\n", j->id, j->arrival,
               j->burst, j->priority, j->start, j->completion, j->turnaround, j->waiting,
               job_status_name(j->status));
    }
    printf("\n  Average turnaround time : %.2f\n", r->avg_tat);
    printf("  Average waiting time    : %.2f\n", r->avg_wt);
    printf("  Average response time   : %.2f\n", r->avg_rt);
    printf("  Makespan / CPU busy     : %d / %d  (utilisation %.1f%%)\n", r->makespan,
           r->busy_time, r->makespan ? 100.0 * r->busy_time / r->makespan : 0.0);
    printf("  Context switches        : %d\n", r->context_switches);
}

void schedule_print_comparison(const ScheduleResult *rs, int count)
{
    printf("\n  %-28s %8s %8s %8s %9s %8s\n", "Policy", "AvgTAT", "AvgWT", "AvgRT", "CtxSw", "Makespan");
    printf("  %-28s %8s %8s %8s %9s %8s\n", "----------------------------", "--------",
           "--------", "--------", "---------", "--------");
    /*
     * Remember which policy has the lowest value in each column (strict '<', so
     * on a tie the policy listed first is kept).
     */
    int best_wt = 0, best_tat = 0, best_rt = 0;
    for (int i = 0; i < count; i++) {
        printf("  %-28s %8.2f %8.2f %8.2f %9d %8d\n", rs[i].name, rs[i].avg_tat,
               rs[i].avg_wt, rs[i].avg_rt, rs[i].context_switches, rs[i].makespan);
        if (rs[i].avg_wt  < rs[best_wt].avg_wt)   best_wt = i;
        if (rs[i].avg_tat < rs[best_tat].avg_tat) best_tat = i;
        if (rs[i].avg_rt  < rs[best_rt].avg_rt)   best_rt = i;
    }
    printf("\n  Lowest average waiting time    : %s (%.2f)\n", rs[best_wt].name, rs[best_wt].avg_wt);
    printf("  Lowest average turnaround time : %s (%.2f)\n", rs[best_tat].name, rs[best_tat].avg_tat);
    printf("  Lowest average response time   : %s (%.2f)\n", rs[best_rt].name, rs[best_rt].avg_rt);
    printf("  (Ties keep the first policy listed.)\n");
}

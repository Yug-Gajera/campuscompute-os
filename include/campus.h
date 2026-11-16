/*
 * campus.h - shared types and module interfaces for CampusCompute.
 *
 * CampusCompute is a command-line prototype that demonstrates core
 * operating-system concepts for a shared university computing lab:
 *   FR1 job management          (jobs.c)
 *   FR2 CPU scheduling          (scheduler.c)
 *   FR3 concurrent logger       (logger.c)
 *   FR4 shared-memory IPC       (ipc.c)
 *   FR5 Banker's algorithm      (banker.c)
 *   FR6 worker failure detection(workers.c)
 *   FR7 resource limits/cleanup (rlimit.c, cleanup.c)
 *   FR8 CLI + error handling    (main.c)
 */
#ifndef CAMPUS_H
#define CAMPUS_H

#include <stddef.h>
#include <sys/types.h>

/* ---------- global limits and paths ---------- */
#define MAX_JOBS        64
#define MAX_SLICES      2048
#define MAX_ARRIVAL     100000
#define MAX_BURST       10000
#define MAX_PRIORITY    99
#define DEFAULT_QUANTUM 2
#define LOG_DIR         "logs"
#define LOG_PATH        "logs/campuscompute.log"
#define IPC_PREFIX      "campuscompute_"   /* every IPC name we create starts with this */

/* Exit codes used by every command */
#define EXIT_OK         0
#define EXIT_RUNTIME    1
#define EXIT_USAGE      2

/* ---------- FR1: jobs ---------- */
typedef enum { JOB_NEW, JOB_READY, JOB_RUNNING, JOB_DONE } JobStatus;

typedef struct {
    int id;           /* unique positive job ID                    */
    int arrival;      /* arrival time (time units)                 */
    int burst;        /* total CPU burst required                  */
    int priority;     /* 0..99, LOWER number = HIGHER priority     */
    int remaining;    /* CPU time still needed (used by RR)        */
    int start;        /* first time the job got the CPU, -1 if not */
    int completion;   /* completion time                           */
    int turnaround;   /* completion - arrival                      */
    int waiting;      /* turnaround - burst                        */
    int response;     /* start - arrival                           */
    JobStatus status;
} Job;

const char *job_status_name(JobStatus s);
int  jobs_load(const char *path, Job *jobs, int *count);   /* 0 ok, -1 error (messages printed) */
void jobs_print(const Job *jobs, int count);
void jobs_reset(Job *jobs, int count);

/* ---------- FR2: scheduling ---------- */
typedef struct { int job_id; int start; int end; } Slice;   /* job_id == -1 means CPU idle */

typedef struct {
    char  name[40];
    int   quantum;                 /* only meaningful for RR */
    Job   jobs[MAX_JOBS];
    int   njobs;
    Slice slices[MAX_SLICES];
    int   nslices;
    int   makespan;
    int   busy_time;
    int   context_switches;
    double avg_tat, avg_wt, avg_rt;
} ScheduleResult;

int  schedule_fcfs(const Job *jobs, int n, ScheduleResult *r);
int  schedule_rr(const Job *jobs, int n, int quantum, ScheduleResult *r);
int  schedule_priority(const Job *jobs, int n, ScheduleResult *r);
void schedule_print(const ScheduleResult *r);
void schedule_print_comparison(const ScheduleResult *rs, int count);

/* ---------- FR3: logger ---------- */
typedef struct {
    long produced, consumed;
    long full_waits, empty_waits;
    int  max_occupancy;
    int  capacity;
} LoggerStats;

int  logger_start(const char *path, int capacity, int echo);
int  logger_log(const char *source, const char *fmt, ...)
         __attribute__((format(printf, 2, 3)));
int  logger_stop(LoggerStats *stats);
int  logger_running(void);
int  logger_demo(int producers, int events, int capacity);

/* ---------- FR4: IPC ---------- */
int  ipc_run(const Job *jobs, int n, int verbose);

/* ---------- FR5: Banker's ---------- */
#define BANK_MAX_P 16
#define BANK_MAX_R 8
typedef struct {
    int n, m;                                  /* processes, resource types */
    int available[BANK_MAX_R];
    int max[BANK_MAX_P][BANK_MAX_R];
    int alloc[BANK_MAX_P][BANK_MAX_R];
    int need[BANK_MAX_P][BANK_MAX_R];
} BankState;

int  banker_load(const char *path, BankState *s);
int  banker_check(const BankState *s, int *sequence, int verbose); /* 1 safe, 0 unsafe */
void banker_print(const BankState *s);
int  banker_command(const char *path, int verbose);   /* returns EXIT_* */

/* ---------- FR6: workers ---------- */
typedef enum { W_NORMAL, W_FAIL_EXIT, W_FAIL_SIGNAL, W_FAIL_HANG } WorkerMode;

typedef struct {
    char       label[24];
    WorkerMode mode;
    int        work_units;     /* size of the real computation */
} WorkerSpec;

typedef struct {
    pid_t pid;
    int   finished;
    int   completed_ok;       /* exited with status 0 */
    int   exit_code;          /* valid if exited      */
    int   signal_no;          /* valid if signaled    */
    int   timed_out;          /* killed by watchdog   */
    double seconds;
} WorkerReport;

int  workers_run(const WorkerSpec *specs, int n, double timeout_s, WorkerReport *out);
int  workers_report(const WorkerSpec *specs, const WorkerReport *rep, int n); /* returns #failures */
int  workers_command(int argc, char **argv);
const char *worker_mode_name(WorkerMode m);

/* ---------- FR7: resource limits and cleanup ---------- */
int  rlimit_command(const char *which);
int  cleanup_check(int remove_stale);
int  thread_count_self(void);
int  no_children_left(void);

/* ---------- FR8: interactive terminal menu ---------- */
int  menu_run(void);

/* ---------- integrated demo ---------- */
int  demo_run(const char *jobfile, const char *bankfile);

/* ---------- small helpers (util.c) ---------- */
int    parse_int(const char *text, int min, int max, int *out);
double now_seconds(void);
void   print_banner(const char *title);
int    ensure_log_dir(void);

#endif /* CAMPUS_H */

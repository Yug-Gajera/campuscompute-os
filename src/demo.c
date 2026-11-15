/*
 * demo.c - integrated end-to-end demonstration.
 *
 * One run exercises every feature on the same job set, with the concurrent
 * logger recording events from all stages, then verifies cleanup and prints
 * a PASS/FAIL summary per functional requirement.
 */
#include "campus.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int our_ipc_objects_in_devshm(void)
{
    DIR *d = opendir("/dev/shm");
    if (!d) return 0;
    char mine[64];
    snprintf(mine, sizeof mine, "_%d", (int)getpid());
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strstr(e->d_name, IPC_PREFIX) == NULL) continue;
        size_t L = strlen(e->d_name), M = strlen(mine);
        if (L >= M && strcmp(e->d_name + L - M, mine) == 0) n++;
    }
    closedir(d);
    return n;
}

int demo_run(const char *jobfile, const char *bankfile)
{
    const char *names[8] = { "FR1 Job management", "FR2 CPU scheduling", "FR3 Threaded logger",
                             "FR4 Shared-memory IPC", "FR5 Banker's safety check",
                             "FR6 Worker failure detection", "FR7 Resource limit + cleanup",
                             "FR8 CLI / error handling" };
    int pass[8] = {0};
    double t0 = now_seconds();

    print_banner("Integrated demonstration");
    printf("  Job file: %s   Banker state: %s   Log: %s\n", jobfile, bankfile, LOG_PATH);
    if (logger_start(LOG_PATH, 8, 0) != 0) {
        fprintf(stderr, "error: cannot start logger; aborting demo\n");
        return EXIT_RUNTIME;
    }
    printf("  Logger thread started (threads in process: %d)\n", thread_count_self());
    logger_log("DEMO", "integrated demo started (jobs=%s, state=%s)", jobfile, bankfile);

    /* ---- FR1 ---- */
    print_banner("Step 1/8  FR1 load and validate jobs");
    Job jobs[MAX_JOBS];
    int n = 0;
    if (jobs_load(jobfile, jobs, &n) == 0) {
        jobs_print(jobs, n);
        pass[0] = 1;
    }

    /* ---- FR2 ---- */
    if (pass[0]) {
        print_banner("Step 2/8  FR2 schedule the same jobs with three policies");
        static ScheduleResult rs[3];
        int ok = schedule_fcfs(jobs, n, &rs[0]) == 0 &&
                 schedule_rr(jobs, n, DEFAULT_QUANTUM, &rs[1]) == 0 &&
                 schedule_priority(jobs, n, &rs[2]) == 0;
        if (ok) {
            schedule_print(&rs[2]);
            schedule_print_comparison(rs, 3);
            int all_done = 1;
            for (int k = 0; k < 3; k++)
                for (int i = 0; i < rs[k].njobs; i++)
                    if (rs[k].jobs[i].status != JOB_DONE) all_done = 0;
            pass[1] = all_done;
        }
    }

    /* ---- FR4 ---- */
    if (pass[0]) {
        print_banner("Step 3/8  FR4 hand jobs to a dispatcher process via shared memory");
        pass[3] = ipc_run(jobs, n, 0) == 0;
    }

    /* ---- FR5 ---- */
    print_banner("Step 4/8  FR5 Banker's safety check before granting lab resources");
    {
        BankState s;
        int seq[BANK_MAX_P];
        if (banker_load(bankfile, &s) == 0) {
            int safe = banker_check(&s, seq, 0);
            printf("  %s: %s", bankfile, safe ? "SAFE, sequence < " : "UNSAFE - requests would be deferred\n");
            for (int k = 0; safe && k < s.n; k++) printf("P%d%s", seq[k], k + 1 < s.n ? ", " : " >\n");
            logger_log("BANKER", "demo state %s", safe ? "SAFE" : "UNSAFE");
            pass[4] = 1;
        }
    }

    /* ---- FR6 ---- */
    if (pass[0]) {
        print_banner("Step 5/8  FR6 run one worker process per job (last one: SIMULATED failure)");
        WorkerSpec   specs[MAX_JOBS];
        WorkerReport rep[MAX_JOBS];
        int w = n < 6 ? n : 6;
        for (int i = 0; i < w; i++) {
            snprintf(specs[i].label, sizeof specs[i].label, "job-J%d", jobs[i].id);
            specs[i].mode = i == w - 1 ? W_FAIL_EXIT : W_NORMAL;
            specs[i].work_units = jobs[i].burst > 5 ? 5 : jobs[i].burst;
        }
        workers_run(specs, w, 2.0, rep);
        int failed = workers_report(specs, rep, w);
        pass[5] = failed == 1 && !rep[w - 1].completed_ok;
    }

    /* ---- FR7 ---- */
    print_banner("Step 6/8  FR7 resource limit on a runaway job");
    int rl_ok = rlimit_command("cpu") == EXIT_OK;

    /* ---- FR8: malformed input must be rejected, not scheduled ---- */
    print_banner("Step 7/8  FR8 malformed input is rejected with useful errors");
    {
        Job bad[MAX_JOBS];
        int nb = 0, q = 0;
        fflush(stdout);
        int rejected = jobs_load("tests/jobs_invalid.txt", bad, &nb) != 0;
        fflush(stderr);
        int quantum_rejected = parse_int("abc", 1, 100, &q) != 0 && parse_int("0", 1, 100, &q) != 0;
        printf("  tests/jobs_invalid.txt rejected : %s\n", rejected ? "yes (errors listed above)" : "NO");
        printf("  quantum 'abc' and '0' rejected  : %s\n", quantum_rejected ? "yes" : "NO");
        logger_log("DEMO", "input validation check: %s", rejected && quantum_rejected ? "OK" : "FAILED");
        pass[7] = rejected && quantum_rejected;
    }

    /* ---- FR3 + cleanup ---- */
    print_banner("Step 8/8  FR3/FR7 stop logger and verify cleanup");
    logger_log("DEMO", "integrated demo finishing, stopping logger");
    LoggerStats st;
    int lg = logger_stop(&st);
    pass[2] = lg == 0 && st.produced == st.consumed;
    int threads = thread_count_self();
    int kids    = no_children_left();
    int ipc     = our_ipc_objects_in_devshm();
    printf("  Logger: %ld entries produced, %ld written, %ld producer waits, peak %d/%d\n",
           st.produced, st.consumed, st.full_waits, st.max_occupancy, st.capacity);
    printf("  Threads left in process : %d\n", threads);
    printf("  Un-reaped child processes: %s\n", kids ? "none" : "SOME");
    printf("  IPC objects left by this run in /dev/shm: %d\n", ipc);
    pass[6] = rl_ok && threads == 1 && kids && ipc == 0;

    /* ---- summary ---- */
    print_banner("Integrated demo summary");
    int total = 0;
    for (int i = 0; i < 8; i++) {
        printf("  %-32s %s\n", names[i], pass[i] ? "PASS" : "FAIL");
        total += pass[i];
    }
    printf("  ------------------------------------------\n");
    printf("  %d/8 requirements passed in %.2fs. Log entries appended to %s\n", total,
           now_seconds() - t0, LOG_PATH);
    return total == 8 ? EXIT_OK : EXIT_RUNTIME;
}

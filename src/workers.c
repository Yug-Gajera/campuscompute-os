/*
 * workers.c - FR6: worker processes and failure detection.
 *
 * The parent fork()s one child per worker. Every worker does REAL work
 * (counting primes) and then, depending on its mode:
 *   W_NORMAL      exits with status 0
 *   W_FAIL_EXIT   [SIMULATED] exits with status 3   (e.g. a bad input file)
 *   W_FAIL_SIGNAL [SIMULATED] calls abort() -> SIGABRT (core dumps disabled
 *                 with RLIMIT_CORE=0 so no core file is left behind)
 *   W_FAIL_HANG   [SIMULATED] stops responding (sleeps forever)
 *
 * The parent supervises all children with waitpid(pid, &status, WNOHANG)
 * in a polling loop and decodes the status with WIFEXITED / WEXITSTATUS /
 * WIFSIGNALED / WTERMSIG. A watchdog deadline catches hung workers: after
 * the timeout the parent sends SIGKILL and then reaps the child.
 *
 * Simulated failures are deliberate and are labelled [SIMULATED] in the
 * output - they are not real OS faults.
 */
#include "campus.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const char *worker_mode_name(WorkerMode m)
{
    switch (m) {
    case W_NORMAL:      return "normal";
    case W_FAIL_EXIT:   return "sim-exit(3)";
    case W_FAIL_SIGNAL: return "sim-abort";
    case W_FAIL_HANG:   return "sim-hang";
    }
    return "?";
}

/* Real CPU work: count primes below limit. */
static int count_primes(int limit)
{
    int count = 0;
    for (int x = 2; x < limit; x++) {
        int prime = 1;
        for (int d = 2; d * d <= x; d++)
            if (x % d == 0) { prime = 0; break; }
        count += prime;
    }
    return count;
}

static void worker_main(const WorkerSpec *w)
{
    int limit = 20000 * (w->work_units > 0 ? w->work_units : 1);
    int primes = count_primes(limit);
    printf("    [%-8s pid %d] computed %d primes below %d\n", w->label, (int)getpid(), primes, limit);
    fflush(stdout);

    switch (w->mode) {
    case W_NORMAL:
        _exit(0);
    case W_FAIL_EXIT:
        printf("    [%-8s pid %d] [SIMULATED FAILURE] exiting with status 3\n", w->label, (int)getpid());
        fflush(stdout);
        _exit(3);
    case W_FAIL_SIGNAL: {
        struct rlimit no_core = { 0, 0 };
        setrlimit(RLIMIT_CORE, &no_core);          /* never leave a core file behind */
        printf("    [%-8s pid %d] [SIMULATED FAILURE] calling abort()\n", w->label, (int)getpid());
        fflush(stdout);
        abort();
    }
    case W_FAIL_HANG:
        printf("    [%-8s pid %d] [SIMULATED FAILURE] hanging (no further progress)\n", w->label, (int)getpid());
        fflush(stdout);
        for (;;) pause();
    }
    _exit(0);
}

int workers_run(const WorkerSpec *specs, int n, double timeout_s, WorkerReport *out)
{
    double started[MAX_JOBS];
    int launched = 0;

    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof out[i]);
        fflush(stdout);
        fflush(stderr);
        pid_t pid = fork();
        if (pid == -1) {
            fprintf(stderr, "error: fork() for %s failed: %s\n", specs[i].label, strerror(errno));
            break;
        }
        if (pid == 0)
            worker_main(&specs[i]);                 /* never returns */
        out[i].pid = pid;
        started[i] = now_seconds();
        launched++;
        printf("    [parent   pid %d] fork() -> %s pid %d (mode: %s)\n", (int)getpid(),
               specs[i].label, (int)pid, worker_mode_name(specs[i].mode));
        logger_log("WORKERS", "forked %s pid %d mode %s", specs[i].label, (int)pid,
                   worker_mode_name(specs[i].mode));
    }

    /* supervision loop: reap finished children, kill hung ones */
    int remaining = launched;
    while (remaining > 0) {
        for (int i = 0; i < launched; i++) {
            if (out[i].finished) continue;
            int status = 0;
            pid_t r = waitpid(out[i].pid, &status, WNOHANG);
            if (r == -1) {
                if (errno == EINTR) continue;
                fprintf(stderr, "error: waitpid(%d): %s\n", (int)out[i].pid, strerror(errno));
                out[i].finished = 1;
                remaining--;
                continue;
            }
            if (r == 0) {                           /* still running */
                if (now_seconds() - started[i] > timeout_s && !out[i].timed_out) {
                    printf("    [parent   pid %d] watchdog: %s pid %d silent for %.1fs -> sending SIGKILL\n",
                           (int)getpid(), specs[i].label, (int)out[i].pid, timeout_s);
                    kill(out[i].pid, SIGKILL);
                    out[i].timed_out = 1;
                }
                continue;
            }
            out[i].finished = 1;
            out[i].seconds  = now_seconds() - started[i];
            if (WIFEXITED(status)) {
                out[i].exit_code    = WEXITSTATUS(status);
                out[i].completed_ok = out[i].exit_code == 0;
                out[i].signal_no    = 0;
            } else if (WIFSIGNALED(status)) {
                out[i].signal_no = WTERMSIG(status);
                out[i].exit_code = -1;
            }
            if (WIFEXITED(status))
                printf("    [parent   pid %d] waitpid(%d) -> WIFEXITED, WEXITSTATUS=%d\n",
                       (int)getpid(), (int)out[i].pid, out[i].exit_code);
            else
                printf("    [parent   pid %d] waitpid(%d) -> WIFSIGNALED, WTERMSIG=%d (%s)\n",
                       (int)getpid(), (int)out[i].pid, out[i].signal_no, strsignal(out[i].signal_no));
            remaining--;
        }
        if (remaining > 0) {
            struct timespec ts = { 0, 20 * 1000 * 1000 };   /* 20 ms poll interval */
            nanosleep(&ts, NULL);
        }
    }
    return launched;
}

static void describe(const WorkerSpec *s, const WorkerReport *r, char *buf, size_t len)
{
    const char *tag = s->mode == W_NORMAL ? "" : " [SIMULATED]";
    if (r->completed_ok)
        snprintf(buf, len, "COMPLETED - exited normally, status 0");
    else if (r->timed_out)
        snprintf(buf, len, "FAILED - unresponsive, killed by watchdog (signal %d %s)%s",
                 r->signal_no, strsignal(r->signal_no), tag);
    else if (r->signal_no)
        snprintf(buf, len, "FAILED - terminated by signal %d (%s)%s", r->signal_no,
                 strsignal(r->signal_no), tag);
    else
        snprintf(buf, len, "FAILED - exited with non-zero status %d%s", r->exit_code, tag);
}

/* Print the per-worker report table; returns number of failures detected. */
static int print_report(const WorkerSpec *specs, const WorkerReport *rep, int n)
{
    int ok = 0, failed = 0, simulated = 0;
    printf("\n  %-9s %-7s %-12s %-7s %s\n", "Worker", "PID", "Mode", "Time", "Status reported by waitpid()");
    printf("  %-9s %-7s %-12s %-7s %s\n", "---------", "-------", "------------", "-------",
           "------------------------------------------------------------");
    for (int i = 0; i < n; i++) {
        char d[160];
        describe(&specs[i], &rep[i], d, sizeof d);
        printf("  %-9s %-7d %-12s %5.2fs  %s\n", specs[i].label, (int)rep[i].pid,
               worker_mode_name(specs[i].mode), rep[i].seconds, d);
        logger_log("WORKERS", "%s pid %d: %s", specs[i].label, (int)rep[i].pid, d);
        if (rep[i].completed_ok) ok++;
        else { failed++; if (specs[i].mode != W_NORMAL) simulated++; }
    }
    printf("\n  Summary: %d completed, %d failed (%d of the failures were deliberately simulated)\n",
           ok, failed, simulated);
    printf("  Children remaining after supervision: %s\n",
           no_children_left() ? "none (waitpid(-1) -> ECHILD)" : "SOME - check!");
    return failed;
}

int workers_command(int argc, char **argv)
{
    /* argv[0] = "workers", argv[1] = normal|fail, argv[2] optional */
    WorkerSpec   specs[8];
    WorkerReport rep[8];
    int n = 0;
    int expect_failures = 0;

    if (argc < 2) goto usage;

    if (strcmp(argv[1], "normal") == 0) {
        int count = 3;
        if (argc > 3 || (argc == 3 && parse_int(argv[2], 1, 8, &count) != 0)) {
            fprintf(stderr, "error: worker count must be an integer 1..8\n");
            goto usage;
        }
        for (int i = 0; i < count; i++) {
            snprintf(specs[i].label, sizeof specs[i].label, "worker-%d", i + 1);
            specs[i].mode = W_NORMAL;
            specs[i].work_units = i + 1;
        }
        n = count;
    } else if (strcmp(argv[1], "fail") == 0) {
        const char *which = argc >= 3 ? argv[2] : "all";
        if (argc > 3) goto usage;
        snprintf(specs[0].label, sizeof specs[0].label, "worker-1");
        specs[0].mode = W_NORMAL;
        specs[0].work_units = 1;
        n = 1;
        int want_exit = !strcmp(which, "exit") || !strcmp(which, "all");
        int want_sig  = !strcmp(which, "signal") || !strcmp(which, "all");
        int want_hang = !strcmp(which, "hang") || !strcmp(which, "all");
        if (!want_exit && !want_sig && !want_hang) {
            fprintf(stderr, "error: unknown failure type '%s'\n", which);
            goto usage;
        }
        if (want_exit) { specs[n] = (WorkerSpec){ "", W_FAIL_EXIT, 1 };   n++; }
        if (want_sig)  { specs[n] = (WorkerSpec){ "", W_FAIL_SIGNAL, 1 }; n++; }
        if (want_hang) { specs[n] = (WorkerSpec){ "", W_FAIL_HANG, 1 };   n++; }
        for (int i = 1; i < n; i++)
            snprintf(specs[i].label, sizeof specs[i].label, "worker-%d", i + 1);
        expect_failures = n - 1;
    } else {
        fprintf(stderr, "error: unknown workers mode '%s'\n", argv[1]);
        goto usage;
    }

    print_banner(expect_failures ? "FR6 Worker failure detection (SIMULATED failures)"
                                 : "FR6 Worker processes - normal completion");
    printf("  Mechanism: fork() + waitpid(WNOHANG) supervision loop, watchdog timeout 2.0s\n");
    if (expect_failures)
        printf("  NOTE: failures below are deliberately SIMULATED by the program, not real OS faults.\n");
    printf("\n");
    workers_run(specs, n, 2.0, rep);
    int failed = print_report(specs, rep, n);

    int correct = failed == expect_failures;
    printf("  Result: %s\n", correct ? "PASS - every worker outcome was detected and reported correctly"
                                     : "FAIL - unexpected worker outcome");
    return correct ? EXIT_OK : EXIT_RUNTIME;

usage:
    fprintf(stderr, "usage: campuscompute workers normal [count 1..8]\n"
                    "       campuscompute workers fail [exit|signal|hang|all]\n");
    return EXIT_USAGE;
}

/* Exposed for the integrated demo. */
int workers_report(const WorkerSpec *specs, const WorkerReport *rep, int n)
{
    return print_report(specs, rep, n);
}

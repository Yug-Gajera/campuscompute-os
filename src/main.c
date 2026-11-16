/*
 * main.c - FR8: command-line interface for CampusCompute.
 *
 * Every feature is a separate sub-command so it can be tested on its own:
 *     ./campuscompute <command> [arguments]
 * Invalid commands or arguments print a specific error plus usage and exit
 * with status 2; runtime failures exit with status 1; success exits with 0.
 */
#include "campus.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out,
        "CampusCompute - intelligent job scheduling and resource management prototype\n"
        "\n"
        "Usage: ./campuscompute <command> [arguments]\n"
        "\n"
        "Jobs and CPU scheduling (FR1, FR2)\n"
        "  jobs     <jobfile>                    load, validate and list jobs\n"
        "  fcfs     <jobfile>                    First-Come First-Served\n"
        "  rr       <jobfile> [quantum]          Round Robin (default quantum %d)\n"
        "  priority <jobfile>                    non-preemptive Priority (lower number = higher)\n"
        "  compare  <jobfile> [quantum]          all three policies on the same job set\n"
        "\n"
        "Concurrency and IPC (FR3, FR4)\n"
        "  logger   [producers] [events] [cap]   pthread producer-consumer logger (default 3 5 4)\n"
        "  ipc      [jobfile]                    POSIX shared memory + named semaphores between processes\n"
        "\n"
        "Resource safety (FR5)\n"
        "  banker   <statefile>                  Banker's safety algorithm (safe sequence / unsafe)\n"
        "\n"
        "Processes, limits and cleanup (FR6, FR7)\n"
        "  workers  normal [count]               fork() workers that complete normally (default 3)\n"
        "  workers  fail [exit|signal|hang|all]  SIMULATED worker failures detected with waitpid()\n"
        "  rlimit   [cpu|fsize|mem|all]          apply setrlimit() in a child and show the effect\n"
        "  cleanup-check [--remove-stale]        verify no CampusCompute processes/IPC objects remain\n"
        "\n"
        "Other\n"
        "  menu                                  interactive terminal menu (arrow keys + Enter)\n"
        "  demo     [jobfile] [statefile]        integrated end-to-end demonstration\n"
        "  help                                  show this message\n"
        "\n"
        "Job file : one job per line '<id> <arrival> <burst> <priority>', '#' comments allowed\n"
        "Tie-break: FCFS arrival then ID | Priority: priority, then arrival, then ID |\n"
        "           RR: FIFO queue; new arrivals enter before the preempted job\n"
        "Log file : %s     Exit codes: 0 ok, 1 runtime failure, 2 invalid usage/input\n",
        DEFAULT_QUANTUM, LOG_PATH);
}

static int usage_error(const char *fmt, const char *arg)
{
    fprintf(stderr, "error: ");
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\nRun './campuscompute help' for the list of commands and arguments.\n");
    return EXIT_USAGE;
}

static int need_args(int argc, int min, int max, const char *cmd, const char *synopsis)
{
    if (argc - 1 < min || argc - 1 > max) {
        fprintf(stderr, "error: wrong number of arguments for '%s'\nusage: ./campuscompute %s\n",
                cmd, synopsis);
        return -1;
    }
    return 0;
}

/* load jobs and run one or all schedulers */
static int cmd_schedule(const char *which, const char *path, int quantum)
{
    static Job jobs[MAX_JOBS];
    static ScheduleResult rs[3];
    int n;
    if (jobs_load(path, jobs, &n) != 0)
        return EXIT_USAGE;

    if (!strcmp(which, "jobs")) {
        print_banner("FR1 Job management - loaded jobs");
        printf("  Source: %s   (priority: lower number = higher priority)\n\n", path);
        jobs_print(jobs, n);
        logger_log("JOBS", "loaded %d jobs from %s", n, path);
        return EXIT_OK;
    }
    if (!strcmp(which, "fcfs")) {
        print_banner("FR2 FCFS scheduling");
        printf("  Tie-break: earlier arrival first, then smaller job ID\n");
        if (schedule_fcfs(jobs, n, &rs[0])) return EXIT_RUNTIME;
        schedule_print(&rs[0]);
    } else if (!strcmp(which, "rr")) {
        print_banner("FR2 Round Robin scheduling");
        printf("  Time quantum: %d\n", quantum);
        printf("  Tie-break: FIFO ready queue; simultaneous arrivals by job ID;\n");
        printf("             a job arriving when a quantum expires is queued before the preempted job\n");
        if (schedule_rr(jobs, n, quantum, &rs[0])) return EXIT_RUNTIME;
        schedule_print(&rs[0]);
    } else if (!strcmp(which, "priority")) {
        print_banner("FR2 Non-preemptive Priority scheduling");
        printf("  Rule: lower priority number = higher priority; a running job is never preempted\n");
        printf("  Tie-break: equal priority -> earlier arrival -> smaller job ID\n");
        if (schedule_priority(jobs, n, &rs[0])) return EXIT_RUNTIME;
        schedule_print(&rs[0]);
    } else {                                           /* compare */
        print_banner("FR2 Scheduling policy comparison (same job set)");
        printf("  Job set: %s   RR quantum: %d\n\n", path, quantum);
        jobs_print(jobs, n);
        if (schedule_fcfs(jobs, n, &rs[0]) || schedule_rr(jobs, n, quantum, &rs[1]) ||
            schedule_priority(jobs, n, &rs[2]))
            return EXIT_RUNTIME;
        for (int k = 0; k < 3; k++) {
            printf("\n  %-26s order: ", rs[k].name);
            for (int i = 0; i < rs[k].nslices; i++) {
                if (rs[k].slices[i].job_id < 0) printf("IDLE ");
                else printf("J%d ", rs[k].slices[i].job_id);
            }
        }
        printf("\n");
        schedule_print_comparison(rs, 3);
    }
    return EXIT_OK;
}

static int dispatch(int argc, char **argv)
{
    const char *cmd = argv[0];
    int q = DEFAULT_QUANTUM;

    if (!strcmp(cmd, "jobs") || !strcmp(cmd, "fcfs") || !strcmp(cmd, "priority")) {
        char syn[64];
        snprintf(syn, sizeof syn, "%s <jobfile>", cmd);
        if (need_args(argc, 1, 1, cmd, syn)) return EXIT_USAGE;
        return cmd_schedule(cmd, argv[1], q);
    }
    if (!strcmp(cmd, "rr") || !strcmp(cmd, "compare")) {
        char syn[64];
        snprintf(syn, sizeof syn, "%s <jobfile> [quantum 1..100]", cmd);
        if (need_args(argc, 1, 2, cmd, syn)) return EXIT_USAGE;
        if (argc == 3 && parse_int(argv[2], 1, 100, &q) != 0)
            return usage_error("time quantum must be an integer 1..100 (got '%s')", argv[2]);
        return cmd_schedule(cmd, argv[1], q);
    }
    if (!strcmp(cmd, "ipc")) {
        if (need_args(argc, 0, 1, cmd, "ipc [jobfile]")) return EXIT_USAGE;
        static Job jobs[MAX_JOBS];
        int n;
        const char *path = argc == 2 ? argv[1] : "tests/jobs_sample.txt";
        if (jobs_load(path, jobs, &n) != 0) return EXIT_USAGE;
        print_banner("FR4 Shared-memory IPC between cooperating processes");
        printf("  Job source: %s\n", path);
        return ipc_run(jobs, n, 1) == 0 ? EXIT_OK : EXIT_RUNTIME;
    }
    if (!strcmp(cmd, "banker")) {
        if (need_args(argc, 1, 1, cmd, "banker <statefile>")) return EXIT_USAGE;
        print_banner("FR5 Banker's safety algorithm");
        return banker_command(argv[1], 1);
    }
    if (!strcmp(cmd, "workers"))
        return workers_command(argc, argv);
    if (!strcmp(cmd, "rlimit")) {
        if (need_args(argc, 0, 1, cmd, "rlimit [cpu|fsize|mem|all]")) return EXIT_USAGE;
        return rlimit_command(argc == 2 ? argv[1] : "all");
    }
    return usage_error("unknown command '%s'", cmd);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);      /* line-buffered: keeps parent/child output ordered */

    if (argc < 2) {
        fprintf(stderr, "error: no command given\n\n");
        usage(stderr);
        return EXIT_USAGE;
    }
    const char *cmd = argv[1];
    int sub_argc = argc - 1;
    char **sub_argv = argv + 1;

    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        usage(stdout);
        return EXIT_OK;
    }

    if (!strcmp(cmd, "menu")) {
        if (sub_argc != 1) {
            fprintf(stderr, "error: 'menu' takes no arguments\nusage: ./campuscompute menu\n");
            return EXIT_USAGE;
        }
        return menu_run();
    }

    if (!strcmp(cmd, "logger")) {
        int p = 3, e = 5, c = 4;
        if (sub_argc > 4) {
            fprintf(stderr, "error: too many arguments\nusage: ./campuscompute logger [producers 1..16] [events 1..100] [capacity 1..64]\n");
            return EXIT_USAGE;
        }
        if ((sub_argc > 1 && parse_int(sub_argv[1], 1, 16, &p)) ||
            (sub_argc > 2 && parse_int(sub_argv[2], 1, 100, &e)) ||
            (sub_argc > 3 && parse_int(sub_argv[3], 1, 64, &c))) {
            fprintf(stderr, "error: invalid logger argument\nusage: ./campuscompute logger [producers 1..16] [events 1..100] [capacity 1..64]\n");
            return EXIT_USAGE;
        }
        return logger_demo(p, e, c);
    }

    if (!strcmp(cmd, "cleanup-check")) {
        if (sub_argc > 2 || (sub_argc == 2 && strcmp(sub_argv[1], "--remove-stale"))) {
            fprintf(stderr, "error: unknown option\nusage: ./campuscompute cleanup-check [--remove-stale]\n");
            return EXIT_USAGE;
        }
        return cleanup_check(sub_argc == 2);
    }

    if (!strcmp(cmd, "demo")) {
        if (sub_argc > 3) {
            fprintf(stderr, "error: too many arguments\nusage: ./campuscompute demo [jobfile] [statefile]\n");
            return EXIT_USAGE;
        }
        return demo_run(sub_argc > 1 ? sub_argv[1] : "tests/jobs_sample.txt",
                        sub_argc > 2 ? sub_argv[2] : "tests/banker_safe.txt");
    }

    /* All other commands record their events through the threaded logger. */
    int logging = logger_start(LOG_PATH, 16, 0) == 0;
    if (!logging)
        fprintf(stderr, "warning: activity logging disabled for this run\n");
    logger_log("CLI", "command started: %s", cmd);
    int rc = dispatch(sub_argc, sub_argv);
    logger_log("CLI", "command finished: %s (exit %d)", cmd, rc);
    if (logging)
        logger_stop(NULL);
    return rc;
}

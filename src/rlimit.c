/*
 * rlimit.c - FR7: process resource limits with setrlimit().
 *
 * Each limit is applied inside a CHILD process so the main program (and the
 * user's shell) keep their normal limits. The parent reports what happened.
 *
 *   cpu   RLIMIT_CPU  soft 1s / hard 2s. The child spins on the CPU; when it
 *         has used 1s of CPU time the kernel sends SIGXCPU, which terminates it.
 *   fsize RLIMIT_FSIZE soft limit 4096 bytes. The child ignores SIGXFSZ and
 *         writes 1 KiB blocks to a temporary file; the write that would cross
 *         4096 bytes fails with EFBIG. The temporary file is deleted afterwards.
 *   mem   RLIMIT_AS = current address-space size + 64 MiB. A further 16 MiB
 *         mapping succeeds; a 256 MiB mapping is refused by the kernel (ENOMEM).
 */
#include "campus.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define FSIZE_TMP "logs/rlimit_fsize_test.tmp"

/*
 * Print a resource's current SOFT limit (what is enforced now) and HARD limit
 * (the ceiling the soft limit may be raised to).
 */
static void show_limit(const char *who, const char *name, int res)
{
    struct rlimit rl;
    if (getrlimit(res, &rl) == -1) { perror("getrlimit"); return; }
    char s[32], h[32];
    if (rl.rlim_cur == RLIM_INFINITY) snprintf(s, sizeof s, "unlimited");
    else snprintf(s, sizeof s, "%llu", (unsigned long long)rl.rlim_cur);
    if (rl.rlim_max == RLIM_INFINITY) snprintf(h, sizeof h, "unlimited");
    else snprintf(h, sizeof h, "%llu", (unsigned long long)rl.rlim_max);
    printf("    [%s pid %d] %s soft=%s hard=%s\n", who, (int)getpid(), name, s, h);
}

/* Apply new soft/hard limits to THIS process with setrlimit(). */
static int set_limit(int res, rlim_t soft, rlim_t hard)
{
    struct rlimit rl = { soft, hard };
    if (setrlimit(res, &rl) == -1) {
        fprintf(stderr, "    setrlimit failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* ---- child bodies ---------------------------------------------------------- */

/*
 * CPU-time limit. Soft 1 s, hard 2 s. The child just burns CPU in a loop;
 * once it has USED 1 s of CPU time (not wall time) the kernel sends SIGXCPU,
 * whose default action terminates the process. A runaway job is stopped.
 */
static void child_cpu(void)
{
    struct rlimit no_core = { 0, 0 };
    setrlimit(RLIMIT_CORE, &no_core);             /* SIGXCPU would otherwise dump core */
    show_limit("child", "RLIMIT_CPU before", RLIMIT_CPU);
    if (set_limit(RLIMIT_CPU, 1, 2) != 0) _exit(10);
    show_limit("child", "RLIMIT_CPU after ", RLIMIT_CPU);
    printf("    [child pid %d] spinning on the CPU until the kernel stops me...\n", (int)getpid());
    fflush(stdout);
    volatile unsigned long spin = 0;
    for (;;) spin++;                              /* SIGXCPU arrives after ~1s CPU time */
}

static void child_fsize(void)
{
    /*
     * RLIMIT_FSIZE applies to EVERY regular file this process writes - including
     * stdout when the output is redirected to a file. So while the limit is active
     * the child only records what happens in memory; it lowers the SOFT limit,
     * runs the test, restores the soft limit, and only then prints the report.
     */
    struct rlimit orig;
    if (getrlimit(RLIMIT_FSIZE, &orig) == -1) { perror("getrlimit"); _exit(10); }
    show_limit("child", "RLIMIT_FSIZE before", RLIMIT_FSIZE);
    signal(SIGXFSZ, SIG_IGN);                     /* get EFBIG instead of being killed */

    char report[1024];
    size_t len = 0;
    long total = 0;
    int fd = open(FSIZE_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) { perror("open"); _exit(11); }

    /*
     * Lower only the SOFT limit to 4096 bytes. Because the hard limit is unchanged,
     * the child may raise the soft limit back afterwards to print its report.
     */
    struct rlimit lim = { 4096, orig.rlim_max };  /* soft 4096 bytes, hard unchanged */
    if (setrlimit(RLIMIT_FSIZE, &lim) == -1) { perror("setrlimit"); close(fd); unlink(FSIZE_TMP); _exit(10); }
    struct rlimit during;
    getrlimit(RLIMIT_FSIZE, &during);

    char block[1024];
    memset(block, 'x', sizeof block);
    for (int i = 1; i <= 8; i++) {
        ssize_t w = write(fd, block, sizeof block);
        if (w == -1) {
            int e = errno;
            len += (size_t)snprintf(report + len, sizeof report - len,
                    "    [child pid %d] write #%d FAILED: %s (errno %d, EFBIG=%s)\n", (int)getpid(), i,
                    strerror(e), e, e == EFBIG ? "yes" : "no");
            break;
        }
        total += w;
        len += (size_t)snprintf(report + len, sizeof report - len,
                "    [child pid %d] write #%d ok, file size now %ld bytes\n", (int)getpid(), i, total);
    }
    setrlimit(RLIMIT_FSIZE, &orig);               /* restore so we can report safely */
    close(fd);
    unlink(FSIZE_TMP);                            /* remove the file we created */

    printf("    [child pid %d] RLIMIT_FSIZE during test soft=%llu (restored after the test)\n",
           (int)getpid(), (unsigned long long)during.rlim_cur);
    fputs(report, stdout);
    printf("    [child pid %d] stopped at %ld bytes; temporary file removed\n", (int)getpid(), total);
    fflush(stdout);
    _exit(total == 4096 ? 0 : 12);
}

/*
 * Address-space limit. RLIMIT_AS caps the total virtual memory a process may
 * map. Requests beyond the cap fail with ENOMEM instead of eating memory
 * that other lab users need.
 */
static void child_mem(void)
{
    show_limit("child", "RLIMIT_AS before", RLIMIT_AS);
    /*
     * RLIMIT_AS caps the TOTAL virtual address space, which already includes
     * the program, libraries and thread stacks/arenas inherited from the parent.
     * So the cap is set to (current size + 64 MiB): 64 MiB of headroom.
     */
    unsigned long cur_kb = 0;
    FILE *st = fopen("/proc/self/status", "r");
    if (st) {
        char line[128];
        while (fgets(line, sizeof line, st))
            if (sscanf(line, "VmSize: %lu kB", &cur_kb) == 1) break;
        fclose(st);
    }
    rlim_t cap = ((rlim_t)cur_kb << 10) + (64UL << 20);
    printf("    [child pid %d] current address space %lu KiB -> cap = current + 64 MiB\n",
           (int)getpid(), cur_kb);
    if (set_limit(RLIMIT_AS, cap, cap) != 0) _exit(10);
    show_limit("child", "RLIMIT_AS after ", RLIMIT_AS);

    /*
     * Memory is requested with mmap() directly rather than malloc(): it is a
     * real system call, so the compiler cannot optimise it away (newer GCC
     * versions may delete an unused malloc()/free() pair and assume success).
     */
    size_t small_len = 16UL << 20, big_len = 256UL << 20;
    void *small = mmap(NULL, small_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (small != MAP_FAILED) memset(small, 1, small_len);       /* really use it */
    printf("    [child pid %d] mmap(16 MiB)  -> %s\n", (int)getpid(),
           small != MAP_FAILED ? "succeeded" : strerror(errno));

    /* this request would push us past the cap -> the kernel must refuse it */
    void *big = mmap(NULL, big_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int big_errno = errno;
    if (big == MAP_FAILED)
        printf("    [child pid %d] mmap(256 MiB) -> refused: %s (errno %d = ENOMEM? %s)\n", (int)getpid(),
               strerror(big_errno), big_errno, big_errno == ENOMEM ? "yes" : "no");
    else
        printf("    [child pid %d] mmap(256 MiB) -> succeeded (limit not enforced here)\n", (int)getpid());
    fflush(stdout);

    int ok = small != MAP_FAILED && big == MAP_FAILED;
    if (small != MAP_FAILED) munmap(small, small_len);
    if (big != MAP_FAILED) munmap(big, big_len);
    _exit(ok ? 0 : 12);
}

/* ---- parent side ------------------------------------------------------------- */

/*
 * PARENT side of each demonstration: fork a child that applies the limit
 * itself (so the parent and the user's shell keep their normal limits),
 * wait for it, and check that it ended the way the limit predicts:
 *   CPU   -> killed by SIGXCPU
 *   FSIZE -> exited 0 after seeing EFBIG at 4096 bytes
 *   AS    -> exited 0 after the big request was refused
 */
static int run_one(const char *name, void (*body)(void), int expect_signal)
{
    printf("\n  --- %s ---\n", name);
    fflush(stdout);
    double t0 = now_seconds();
    pid_t pid = fork();
    if (pid == -1) { perror("fork"); return -1; }
    if (pid == 0) body();

    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) { perror("waitpid"); return -1; }
    }
    double dt = now_seconds() - t0;
    struct rusage ru;

    int ok;
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        printf("  [parent] child %d terminated by signal %d (%s) after %.2fs wall time\n",
               (int)pid, sig, strsignal(sig), dt);
        ok = sig == expect_signal;
    } else {
        printf("  [parent] child %d exited with status %d after %.2fs\n", (int)pid, WEXITSTATUS(status), dt);
        ok = expect_signal == 0 && WEXITSTATUS(status) == 0;
    }
    if (getrusage(RUSAGE_CHILDREN, &ru) == 0 && expect_signal == SIGXCPU)
        printf("  [parent] CPU time consumed by children so far: %ld.%02lds user\n",
               (long)ru.ru_utime.tv_sec, (long)ru.ru_utime.tv_usec / 10000);
    logger_log("RLIMIT", "%s: %s", name, ok ? "limit enforced as expected" : "unexpected result");
    return ok ? 0 : -1;
}

/*
 * 'rlimit [cpu|fsize|mem|all]' - run the chosen demonstrations and report
 * how many behaved as expected.
 */
int rlimit_command(const char *which)
{
    int cpu = !strcmp(which, "cpu") || !strcmp(which, "all");
    int fsz = !strcmp(which, "fsize") || !strcmp(which, "all");
    int mem = !strcmp(which, "mem") || !strcmp(which, "all");
    if (!cpu && !fsz && !mem) {
        fprintf(stderr, "error: unknown limit '%s'\nusage: campuscompute rlimit [cpu|fsize|mem|all]\n", which);
        return EXIT_USAGE;
    }
    if (ensure_log_dir() != 0) return EXIT_RUNTIME;

    print_banner("FR7 Resource limits with setrlimit()");
    printf("  Limits are applied in a child process; this parent keeps its own limits:\n");
    show_limit("parent", "RLIMIT_CPU", RLIMIT_CPU);

    int fails = 0, ran = 0;
    if (cpu) {
        ran++;
        int bad = run_one("RLIMIT_CPU = 1s soft / 2s hard", child_cpu, SIGXCPU) != 0;
        fails += bad;
        printf(bad ? "  Effect NOT observed as expected on this system.\n"
                   : "  Observed effect: the kernel sent SIGXCPU once the child used 1s of CPU, stopping a runaway job.\n");
    }
    if (fsz) {
        ran++;
        int bad = run_one("RLIMIT_FSIZE = 4096 bytes", child_fsize, 0) != 0;
        fails += bad;
        printf(bad ? "  Effect NOT observed as expected on this system.\n"
                   : "  Observed effect: writes beyond 4096 bytes fail with EFBIG, so a job cannot fill the lab disk.\n");
    }
    if (mem) {
        ran++;
        int bad = run_one("RLIMIT_AS = current size + 64 MiB", child_mem, 0) != 0;
        fails += bad;
        printf(bad ? "  Effect NOT observed as expected on this system.\n"
                   : "  Observed effect: memory requests beyond the address-space cap fail, protecting other users' memory.\n");
    }
    printf("\n  Parent limit unchanged afterwards:\n");
    show_limit("parent", "RLIMIT_CPU", RLIMIT_CPU);
    printf("  Temporary file %s present: %s\n", FSIZE_TMP, access(FSIZE_TMP, F_OK) == 0 ? "YES" : "no (cleaned up)");
    printf("  Result: %d/%d limit demonstrations behaved as expected -> %s\n", ran - fails, ran,
           fails ? "FAIL" : "PASS");
    return fails ? EXIT_RUNTIME : EXIT_OK;
}

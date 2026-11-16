/*
 * cleanup.c - FR7: cleanup verification.
 *
 * cleanup-check looks ONLY for things this program creates:
 *   - /dev/shm/campuscompute_*        (POSIX shared memory objects)
 *   - /dev/shm/sem.campuscompute_*    (POSIX named semaphores)
 *   - logs/<name>.tmp                   (temporary files from the rlimit demo)
 *   - other running 'campuscompute' processes
 * Each IPC name ends in the PID of the run that created it. An object whose
 * creator is no longer alive is "stale"; with --remove-stale those (and only
 * those) are unlinked. Nothing else on the system is touched.
 */
#include "campus.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Number of threads in this process, read from /proc/self/status. */
int thread_count_self(void)
{
    FILE *fp = fopen("/proc/self/status", "r");
    if (!fp) return -1;
    char line[256];
    int threads = -1;
    while (fgets(line, sizeof line, fp))
        if (sscanf(line, "Threads: %d", &threads) == 1) break;
    fclose(fp);
    return threads;
}

/* 1 if this process has no un-reaped children. */
int no_children_left(void)
{
    int st;
    return waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD;
}

/*
 * kill(pid, 0) sends no signal; it only checks whether the process exists.
 * EPERM means it exists but belongs to another user.
 */
static int pid_alive(int pid)
{
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

/* extract the trailing _<pid> from an object name */
/* Our IPC names end in _<pid of the creating run>; extract that number. */
static int trailing_pid(const char *name)
{
    const char *u = strrchr(name, '_');
    int pid = 0;
    if (!u || parse_int(u + 1, 1, 1 << 30, &pid) != 0) return -1;
    return pid;
}

/*
 * Look ONLY for things CampusCompute itself creates (names starting with
 * 'campuscompute_'), never touching anyone else's files or IPC objects.
 */
int cleanup_check(int remove_stale)
{
    print_banner("FR7 Cleanup verification");
    int problems = 0;

    /* 1. IPC objects in /dev/shm */
    printf("  [1] POSIX IPC objects created by CampusCompute in /dev/shm:\n");
    DIR *d = opendir("/dev/shm");
    int found = 0;
    if (!d) {
        printf("      cannot open /dev/shm: %s\n", strerror(errno));
    } else {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *nm = e->d_name;
            int is_shm = strncmp(nm, IPC_PREFIX, strlen(IPC_PREFIX)) == 0;
            int is_sem = strncmp(nm, "sem." IPC_PREFIX, 4 + strlen(IPC_PREFIX)) == 0;
            if (!is_shm && !is_sem) continue;
            found++;
            int owner = trailing_pid(nm);
            int alive = pid_alive(owner);
            printf("      %-45s owner pid %d %s\n", nm, owner, alive ? "(still running)" : "(STALE)");
            /* creator process is gone but the object remains -> a leftover ('stale') */
            if (!alive) {
                if (remove_stale) {
                    char path[300];
                    snprintf(path, sizeof path, "/dev/shm/%s", nm);
                    if (unlink(path) == 0) printf("        -> removed stale object\n");
                    else { printf("        -> could not remove: %s\n", strerror(errno)); problems++; }
                } else {
                    problems++;
                }
            }
        }
        closedir(d);
        if (!found) printf("      none - all shared memory and semaphores were released\n");
    }

    /* 2. temporary files */
    printf("  [2] Temporary files in %s/:\n", LOG_DIR);
    int tmp = 0;
    d = opendir(LOG_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            size_t len = strlen(e->d_name);
            if (len > 4 && strcmp(e->d_name + len - 4, ".tmp") == 0) {
                printf("      %s/%s\n", LOG_DIR, e->d_name);
                tmp++;
            }
        }
        closedir(d);
    }
    if (!tmp) printf("      none\n");
    problems += tmp;

    /* 3. other campuscompute processes (scan /proc/<pid>/comm) */
    printf("  [3] Other running campuscompute processes:\n");
    int others = 0;
    /*
     * Every running process has a directory /proc/<pid>; its 'comm' file holds
     * the program name. Any other 'campuscompute' process would be a leftover.
     */
    d = opendir("/proc");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!isdigit((unsigned char)e->d_name[0])) continue;
            int pid = atoi(e->d_name);
            if (pid == (int)getpid()) continue;
            char path[300], comm[64] = "";
            snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
            FILE *fp = fopen(path, "r");
            if (!fp) continue;
            if (fgets(comm, sizeof comm, fp)) comm[strcspn(comm, "\n")] = '\0';
            fclose(fp);
            if (strcmp(comm, "campuscompute") == 0) {
                printf("      pid %d is still running\n", pid);
                others++;
            }
        }
        closedir(d);
    }
    if (!others) printf("      none - no worker/IPC child processes left behind\n");
    problems += others;

    /* 4. this process */
    printf("  [4] This process: threads=%d, un-reaped children=%s\n", thread_count_self(),
           no_children_left() ? "none" : "SOME");

    printf("\n  Result: %s\n", problems == 0 ? "CLEAN - nothing left behind by CampusCompute"
                                             : "ATTENTION - leftovers listed above (run with --remove-stale)");
    return problems == 0 ? EXIT_OK : EXIT_RUNTIME;
}

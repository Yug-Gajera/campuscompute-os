/*
 * ipc.c - FR4: inter-process communication with POSIX shared memory and
 * POSIX named semaphores.
 *
 * Two cooperating child processes share one memory region:
 *   SUBMITTER  - writes job records into a small ring buffer in shared memory
 *   DISPATCHER - reads the records, "dispatches" them and writes a summary
 *                (count, total burst, order received) back into shared memory
 * The parent creates the IPC objects, forks both children, waits for them
 * with waitpid(), checks the summary against its own calculation, and then
 * removes every object it created.
 *
 * Semaphores (classic bounded-buffer solution, but across PROCESSES):
 *   sem_empty  initial = SLOTS  counts free slots   (submitter waits on it)
 *   sem_full   initial = 0      counts filled slots (dispatcher waits on it)
 *   sem_mutex  initial = 1      mutual exclusion on head/tail indices
 *
 * Names contain the parent's PID, e.g. /campuscompute_shm_4242, so several
 * runs never collide and cleanup never touches objects we did not create.
 */
#include "campus.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Only 3 slots for 5 jobs on purpose: the submitter fills the buffer and must
 * wait on sem_empty, which shows the semaphores actually coordinating.
 */
#define SLOTS 3

/* One message passed through shared memory: a job's fields, or an END marker. */
typedef struct {
    int id, arrival, burst, priority;
    int last;                       /* 1 = end-of-stream marker */
} JobMsg;

/*
 * Layout of the shared-memory region. Both child processes mmap() the SAME
 * physical pages, so a write by one is immediately visible to the other.
 * It contains only plain values (no pointers), because each process may map
 * the region at a different virtual address.
 */
typedef struct {
    /* ring buffer written by submitter, read by dispatcher */
    JobMsg slot[SLOTS];
    int    head, tail;
    /* summary written by dispatcher, read by parent */
    int    received;
    long   burst_total;
    int    order[MAX_JOBS];
    pid_t  dispatcher_pid;
    int    summary_ready;
} SharedRegion;

typedef struct {
    char shm[64], empty[64], full[64], mutex[64];
} IpcNames;

/* sem_wait() can be interrupted by a signal (errno EINTR); just try again. */
static int sem_wait_retry(sem_t *s)
{
    while (sem_wait(s) == -1) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

/* wait, but announce when we are actually going to block */
/*
 * Try the semaphore without blocking first (sem_trywait). If that fails with
 * EAGAIN the process is about to block, so we print it - this is how the
 * output shows 'blocked on sem_empty' - and then really wait.
 */
static int sem_wait_verbose(sem_t *s, const char *who, const char *semname, int verbose)
{
    if (sem_trywait(s) == 0)
        return 0;
    if (errno != EAGAIN)
        return -1;
    if (verbose) {
        printf("    [%-10s pid %d] blocked on %s ...\n", who, (int)getpid(), semname);
        fflush(stdout);
    }
    return sem_wait_retry(s);
}

/*
 * SUBMITTER (child process 1) - the producer side of the bounded buffer:
 *   sem_wait(empty)  wait for a free slot
 *   sem_wait(mutex)  lock the head/tail indices
 *   write the job into slot[tail], advance tail
 *   sem_post(mutex)  unlock
 *   sem_post(full)   tell the dispatcher one more slot is filled
 * After all jobs it sends an END marker, then exits with status 0.
 */
static void submitter(SharedRegion *r, sem_t *empty, sem_t *full, sem_t *mutex,
                      const Job *jobs, int n, int verbose)
{
    for (int i = 0; i <= n; i++) {
        JobMsg m = {0};
        if (i < n) {
            m.id = jobs[i].id; m.arrival = jobs[i].arrival;
            m.burst = jobs[i].burst; m.priority = jobs[i].priority;
        } else {
            m.last = 1;
        }
        if (sem_wait_verbose(empty, "submitter", "sem_empty (all slots full)", verbose) == -1 ||
            sem_wait_retry(mutex) == -1) {
            perror("submitter: sem_wait");
            _exit(3);
        }
        int slot = r->tail;
        r->slot[slot] = m;
        r->tail = (r->tail + 1) % SLOTS;
        sem_post(mutex);
        if (verbose) {             /* report before sem_post(full) so output order is truthful */
            if (m.last) printf("    [submitter  pid %d] wrote END marker into slot %d\n", (int)getpid(), slot);
            else printf("    [submitter  pid %d] wrote J%d (burst %d, pri %d) into slot %d\n",
                        (int)getpid(), m.id, m.burst, m.priority, slot);
            fflush(stdout);
        }
        sem_post(full);
        usleep(1000);
    }
    _exit(0);
}

/*
 * DISPATCHER (child process 2) - the consumer side, the mirror image:
 *   sem_wait(full) -> lock -> read slot[head] -> unlock -> sem_post(empty)
 * It adds up what it receives and finally writes a summary into shared memory
 * so the parent can verify that nothing was lost or reordered.
 */
static void dispatcher(SharedRegion *r, sem_t *empty, sem_t *full, sem_t *mutex, int verbose)
{
    int  received = 0;
    long total = 0;
    int  order[MAX_JOBS];
    usleep(20000);                 /* start late so the submitter fills the buffer and blocks */
    for (;;) {
        if (sem_wait_verbose(full, "dispatcher", "sem_full (no slots filled)", verbose) == -1 ||
            sem_wait_retry(mutex) == -1) {
            perror("dispatcher: sem_wait");
            _exit(3);
        }
        int slot = r->head;
        JobMsg m = r->slot[slot];
        r->head = (r->head + 1) % SLOTS;
        sem_post(mutex);
        if (m.last) {
            if (verbose) printf("    [dispatcher pid %d] read END marker from slot %d\n", (int)getpid(), slot);
            fflush(stdout);
            sem_post(empty);
            break;
        }
        if (received < MAX_JOBS) order[received] = m.id;
        received++;
        total += m.burst;
        if (verbose) {
            printf("    [dispatcher pid %d] read  J%d from slot %d -> dispatched (running total burst %ld)\n",
                   (int)getpid(), m.id, slot, total);
            fflush(stdout);
        }
        sem_post(empty);           /* slot is free again */
        usleep(8000);              /* simulated dispatch work */
    }
    /* publish the summary for the parent (under the mutex) */
    if (sem_wait_retry(mutex) == -1) _exit(3);
    r->received = received;
    r->burst_total = total;
    memcpy(r->order, order, sizeof(int) * (size_t)(received < MAX_JOBS ? received : MAX_JOBS));
    r->dispatcher_pid = getpid();
    r->summary_ready = 1;
    sem_post(mutex);
    fflush(stdout);
    _exit(0);
}

/*
 * POSIX shm objects appear as /dev/shm/<name> and named semaphores as
 * /dev/shm/sem.<name> on Linux, so checking those files tells us whether
 * an IPC object still exists.
 */
static int object_exists(const char *devpath)
{
    return access(devpath, F_OK) == 0;
}

/* Decode a waitpid() status: normal exit (with its code) or killed by a signal. */
static int describe_child(const char *who, pid_t pid, int status)
{
    if (WIFEXITED(status)) {
        printf("  %-10s (pid %d) exited with status %d\n", who, (int)pid, WEXITSTATUS(status));
        return WEXITSTATUS(status) == 0 ? 0 : -1;
    }
    if (WIFSIGNALED(status))
        printf("  %-10s (pid %d) killed by signal %d (%s)\n", who, (int)pid, WTERMSIG(status),
               strsignal(WTERMSIG(status)));
    return -1;
}

/*
 * PARENT: create the IPC objects, fork the two children, wait for them,
 * check their results, and finally remove everything it created.
 * The 'goto cleanup' pattern makes sure objects are removed on ANY error.
 */
int ipc_run(const Job *jobs, int n, int verbose)
{
    IpcNames nm;
    pid_t me = getpid();
    /* Names include our PID (e.g. /campuscompute_shm_4242) so two runs never clash. */
    snprintf(nm.shm,   sizeof nm.shm,   "/" IPC_PREFIX "shm_%d", (int)me);
    snprintf(nm.empty, sizeof nm.empty, "/" IPC_PREFIX "sem_empty_%d", (int)me);
    snprintf(nm.full,  sizeof nm.full,  "/" IPC_PREFIX "sem_full_%d", (int)me);
    snprintf(nm.mutex, sizeof nm.mutex, "/" IPC_PREFIX "sem_mutex_%d", (int)me);

    int   fd = -1, ok = 0;
    SharedRegion *r = MAP_FAILED;
    sem_t *se = SEM_FAILED, *sf = SEM_FAILED, *sm = SEM_FAILED;
    pid_t sub = -1, dis = -1;

    if (verbose) {
        printf("  Shared memory object : %s (%zu bytes)\n", nm.shm, sizeof(SharedRegion));
        printf("  Named semaphores     : %s (init %d)\n", nm.empty, SLOTS);
        printf("                         %s (init 0)\n", nm.full);
        printf("                         %s (init 1)\n", nm.mutex);
        printf("  Ring buffer slots    : %d   Jobs to transfer: %d\n\n", SLOTS, n);
    }

    /* 1. create shared memory - O_EXCL guarantees we never reuse someone else's object */
    fd = shm_open(nm.shm, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) { fprintf(stderr, "error: shm_open(%s): %s\n", nm.shm, strerror(errno)); goto cleanup; }
    /* a new shm object has size 0 -> give it the size of our struct */
    if (ftruncate(fd, sizeof(SharedRegion)) == -1) { perror("error: ftruncate"); goto cleanup; }
    /* MAP_SHARED: changes are shared with every process that maps this object */
    r = mmap(NULL, sizeof(SharedRegion), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (r == MAP_FAILED) { perror("error: mmap"); goto cleanup; }
    memset(r, 0, sizeof *r);

    /* 2. create semaphores */
    /* empty = SLOTS (all slots free), full = 0 (nothing to read), mutex = 1 (unlocked) */
    se = sem_open(nm.empty, O_CREAT | O_EXCL, 0600, SLOTS);
    sf = sem_open(nm.full,  O_CREAT | O_EXCL, 0600, 0);
    sm = sem_open(nm.mutex, O_CREAT | O_EXCL, 0600, 1);
    if (se == SEM_FAILED || sf == SEM_FAILED || sm == SEM_FAILED) {
        fprintf(stderr, "error: sem_open: %s\n", strerror(errno));
        goto cleanup;
    }
    if (verbose) {
        char p[256];
        const char *made[4] = { nm.shm, nm.empty, nm.full, nm.mutex };
        for (int i = 0; i < 4; i++) {
            if (i == 0) snprintf(p, sizeof p, "/dev/shm%s", made[i]);
            else        snprintf(p, sizeof p, "/dev/shm/sem.%s", made[i] + 1);
            printf("  Created: %-44s present=%s\n", p, object_exists(p) ? "yes" : "no");
        }
        printf("\n");
    }
    logger_log("IPC", "created %s and 3 named semaphores", nm.shm);

    /* 3. fork the two cooperating processes */
    fflush(stdout);
    fflush(stderr);
    /*
     * Flush stdout before fork() so buffered text is not printed twice, then
     * create the two children. Each child inherits the mapping and semaphores.
     * The child functions end with _exit(), so they never return here.
     */
    sub = fork();
    if (sub == -1) { perror("error: fork(submitter)"); goto cleanup; }
    if (sub == 0) submitter(r, se, sf, sm, jobs, n, verbose);

    dis = fork();
    if (dis == -1) { perror("error: fork(dispatcher)"); goto cleanup; }
    if (dis == 0) dispatcher(r, se, sf, sm, verbose);

    /* 4. wait for both and check results */
    int st_sub = 0, st_dis = 0;
    pid_t sub_pid = sub, dis_pid = dis;
    /* wait for BOTH children (reaping them, so no zombie processes remain) */
    if (waitpid(sub, &st_sub, 0) == -1) perror("waitpid(submitter)");
    sub = -1;
    if (waitpid(dis, &st_dis, 0) == -1) perror("waitpid(dispatcher)");
    dis = -1;
    printf("\n");
    describe_child("submitter", sub_pid, st_sub);
    describe_child("dispatcher", dis_pid, st_dis);

    /*
     * The parent computes the expected result itself and compares it with what
     * the dispatcher wrote into shared memory: same count, same total burst,
     * same order (FIFO).
     */
    long expect_total = 0;
    for (int i = 0; i < n; i++) expect_total += jobs[i].burst;
    int order_ok = 1;
    for (int i = 0; i < n && i < MAX_JOBS; i++)
        if (r->order[i] != jobs[i].id) order_ok = 0;

    printf("\n  Parent reads summary from shared memory:\n");
    printf("    jobs received by dispatcher : %d (expected %d)\n", r->received, n);
    printf("    total CPU burst received    : %ld (expected %ld)\n", r->burst_total, expect_total);
    printf("    FIFO order preserved        : %s\n", order_ok ? "yes" : "NO");
    ok = WIFEXITED(st_sub) && WEXITSTATUS(st_sub) == 0 && WIFEXITED(st_dis) &&
         WEXITSTATUS(st_dis) == 0 && r->summary_ready && r->received == n &&
         r->burst_total == expect_total && order_ok;
    logger_log("IPC", "transfer %s: %d jobs, burst total %ld", ok ? "OK" : "FAILED",
               r->received, r->burst_total);

/*
 * CLEANUP (reached on success and on every error): kill/reap any child still
 * running, unmap and close the shared memory, close the semaphores, then
 * shm_unlink/sem_unlink to delete the objects from the system.
 */
cleanup:
    /* 5. reap any child still alive after an error, then remove only OUR objects */
    if (sub > 0) { kill(sub, SIGTERM); waitpid(sub, NULL, 0); }
    if (dis > 0) { kill(dis, SIGTERM); waitpid(dis, NULL, 0); }
    if (r != MAP_FAILED) munmap(r, sizeof(SharedRegion));
    if (fd != -1) close(fd);
    if (se != SEM_FAILED) sem_close(se);
    if (sf != SEM_FAILED) sem_close(sf);
    if (sm != SEM_FAILED) sem_close(sm);

    int removed = 0, leftovers = 0;
    const char *sems[3] = { nm.empty, nm.full, nm.mutex };
    if (shm_unlink(nm.shm) == 0) removed++;
    for (int i = 0; i < 3; i++)
        if (sem_unlink(sems[i]) == 0) removed++;

    char p[256];
    snprintf(p, sizeof p, "/dev/shm%s", nm.shm);
    /* verify: after unlinking, none of our objects may still exist in /dev/shm */
    leftovers += object_exists(p);
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof p, "/dev/shm/sem.%s", sems[i] + 1);
        leftovers += object_exists(p);
    }
    printf("\n  Cleanup: shm_unlink + sem_unlink removed %d object(s); leftovers in /dev/shm: %d\n",
           removed, leftovers);
    printf("  Children remaining: %s\n", no_children_left() ? "none (verified with waitpid -> ECHILD)" : "SOME");
    logger_log("IPC", "cleanup removed %d objects, %d leftovers", removed, leftovers);

    printf("  Result: %s\n", ok && leftovers == 0 ? "PASS" : "FAIL");
    return ok && leftovers == 0 ? 0 : -1;
}

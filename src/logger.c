/*
 * logger.c - FR3: concurrent activity logger.
 *
 * Design: a bounded circular buffer (capacity chosen at start) shared between
 * any number of PRODUCER threads (callers of logger_log) and ONE CONSUMER
 * thread that writes entries to logs/campuscompute.log.
 *
 *   mutex      - protects the buffer, counters and the 'stopping' flag
 *   not_full   - producers wait here while count == capacity
 *   not_empty  - consumer waits here while count == 0
 *
 * Clean shutdown (logger_stop): set stopping=1 under the mutex and broadcast
 * both conditions. The consumer keeps draining until the buffer is empty and
 * then exits; logger_stop joins it, so no entry is ever lost and no thread is
 * left running. New logger_log calls after stop has begun are rejected.
 *
 * Fork safety: only the process that started the logger may log. Child
 * processes created with fork() inherit a copy of the buffer but no consumer
 * thread, so logger_log() silently does nothing in them.
 */
#include "campus.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    long  seq;
    char  when[32];
    pid_t pid;
    char  source[24];
    char  text[200];
} LogEntry;

static struct {
    LogEntry       *buf;
    int             capacity, head, tail, count;
    int             running, stopping, echo;
    long            next_seq;
    pid_t           owner_pid;
    FILE           *fp;
    pthread_t       consumer;
    pthread_mutex_t mutex;
    pthread_cond_t  not_full, not_empty;
    LoggerStats     stats;
    int             consumer_delay_us;   /* only used by the demo to make the buffer fill up */
} L = { .mutex = PTHREAD_MUTEX_INITIALIZER,
        .not_full = PTHREAD_COND_INITIALIZER,
        .not_empty = PTHREAD_COND_INITIALIZER };

/* echo output is serialised so lines from different threads never interleave */
static pthread_mutex_t echo_mutex = PTHREAD_MUTEX_INITIALIZER;
static void echo_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void echo_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&echo_mutex);
    vprintf(fmt, ap);
    fflush(stdout);
    pthread_mutex_unlock(&echo_mutex);
    va_end(ap);
}

static void timestamp(char *out, size_t len)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    size_t n = strftime(out, len, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(out + n, len - n, ".%03ld", ts.tv_nsec / 1000000);
}

/* ---- consumer thread ---------------------------------------------------- */

static void *consumer_main(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&L.mutex);
        while (L.count == 0 && !L.stopping) {
            L.stats.empty_waits++;
            pthread_cond_wait(&L.not_empty, &L.mutex);
        }
        if (L.count == 0 && L.stopping) {           /* drained and asked to stop */
            pthread_mutex_unlock(&L.mutex);
            break;
        }
        LogEntry e = L.buf[L.head];                 /* remove one entry */
        L.head = (L.head + 1) % L.capacity;
        L.count--;
        int left = L.count;
        L.stats.consumed++;
        pthread_cond_signal(&L.not_full);
        pthread_mutex_unlock(&L.mutex);

        /* File I/O happens OUTSIDE the lock so producers are not blocked by disk. */
        fprintf(L.fp, "%s [pid %d] #%04ld %-12s %s\n", e.when, (int)e.pid, e.seq, e.source, e.text);
        fflush(L.fp);
        if (L.echo)
            echo_line("    [consumer] wrote #%04ld to log   (queue %d/%d)\n", e.seq, left, L.capacity);
        if (L.consumer_delay_us > 0)
            usleep((useconds_t)L.consumer_delay_us);
    }
    if (L.echo)
        echo_line("    [consumer] queue empty and stop requested -> consumer thread exiting\n");
    return NULL;
}

/* ---- public API ---------------------------------------------------------- */

int logger_running(void)
{
    return L.running && L.owner_pid == getpid();
}

int logger_start(const char *path, int capacity, int echo)
{
    if (L.running) {
        fprintf(stderr, "error: logger already running\n");
        return -1;
    }
    if (capacity < 1 || capacity > 1024) {
        fprintf(stderr, "error: logger capacity must be 1..1024\n");
        return -1;
    }
    if (ensure_log_dir() != 0)
        return -1;
    L.fp = fopen(path, "a");
    if (L.fp == NULL) {
        fprintf(stderr, "error: cannot open log file '%s': %s\n", path, strerror(errno));
        return -1;
    }
    L.buf = calloc((size_t)capacity, sizeof *L.buf);
    if (L.buf == NULL) {
        fprintf(stderr, "error: out of memory for log buffer\n");
        fclose(L.fp);
        return -1;
    }
    L.capacity = capacity;
    L.head = L.tail = L.count = 0;
    L.stopping = 0;
    L.echo = echo;
    L.next_seq = 1;
    L.owner_pid = getpid();
    memset(&L.stats, 0, sizeof L.stats);
    L.stats.capacity = capacity;

    int rc = pthread_create(&L.consumer, NULL, consumer_main, NULL);
    if (rc != 0) {
        fprintf(stderr, "error: pthread_create(consumer): %s\n", strerror(rc));
        free(L.buf);
        fclose(L.fp);
        return -1;
    }
    L.running = 1;
    return 0;
}

/* Producer side: blocks while the buffer is full. Returns 0, or -1 if the
 * logger is not running / is shutting down. */
int logger_log(const char *source, const char *fmt, ...)
{
    if (!L.running || L.owner_pid != getpid())
        return -1;

    LogEntry e;
    timestamp(e.when, sizeof e.when);
    e.pid = getpid();
    snprintf(e.source, sizeof e.source, "%s", source);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.text, sizeof e.text, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&L.mutex);
    if (L.stopping) {
        pthread_mutex_unlock(&L.mutex);
        return -1;
    }
    while (L.count == L.capacity && !L.stopping) {
        L.stats.full_waits++;
        if (L.echo)
            echo_line("    [%-10s] queue FULL (%d/%d) -> waiting on not_full\n",
                      source, L.count, L.capacity);
        pthread_cond_wait(&L.not_full, &L.mutex);
    }
    if (L.stopping) {
        pthread_mutex_unlock(&L.mutex);
        return -1;
    }
    e.seq = L.next_seq++;
    L.buf[L.tail] = e;                              /* insert one entry */
    L.tail = (L.tail + 1) % L.capacity;
    L.count++;
    if (L.count > L.stats.max_occupancy)
        L.stats.max_occupancy = L.count;
    L.stats.produced++;
    int now = L.count;
    pthread_cond_signal(&L.not_empty);
    pthread_mutex_unlock(&L.mutex);

    if (L.echo)
        echo_line("    [%-10s] queued #%04ld \"%s\"   (queue %d/%d)\n", source, e.seq, e.text,
                  now, L.capacity);
    return 0;
}

int logger_stop(LoggerStats *stats)
{
    if (!L.running || L.owner_pid != getpid())
        return -1;
    pthread_mutex_lock(&L.mutex);
    L.stopping = 1;
    pthread_cond_broadcast(&L.not_empty);
    pthread_cond_broadcast(&L.not_full);
    pthread_mutex_unlock(&L.mutex);

    int rc = pthread_join(L.consumer, NULL);
    if (rc != 0)
        fprintf(stderr, "error: pthread_join(consumer): %s\n", strerror(rc));

    fclose(L.fp);
    free(L.buf);
    L.fp = NULL;
    L.buf = NULL;
    L.running = 0;
    L.consumer_delay_us = 0;
    if (stats)
        *stats = L.stats;
    return rc == 0 ? 0 : -1;
}

/* ---- FR3 demonstration ---------------------------------------------------- */

typedef struct {
    int  index;
    int  events;
    char name[24];
} ProducerArg;

static const char *EVENT_KINDS[] = {
    "job submitted", "job admitted to ready queue", "job dispatched to CPU",
    "job completed", "lab node heartbeat"
};

static void *producer_main(void *arg)
{
    ProducerArg *p = arg;
    unsigned seed = (unsigned)(p->index * 7919 + 17);     /* deterministic per thread */
    for (int k = 1; k <= p->events; k++) {
        const char *kind = EVENT_KINDS[(p->index + k) % 5];
        logger_log(p->name, "%s: J%d%02d (event %d/%d)", kind, p->index, k, k, p->events);
        usleep((useconds_t)(200 + rand_r(&seed) % 800)); /* jitter so threads interleave */
    }
    echo_line("    [%-10s] finished producing %d events -> thread returning\n", p->name, p->events);
    return NULL;
}

int logger_demo(int producers, int events, int capacity)
{
    if (producers < 1 || producers > 16) {
        fprintf(stderr, "error: producers must be 1..16\n");
        return EXIT_USAGE;
    }
    print_banner("FR3 Producer-consumer activity logger (POSIX threads)");
    printf("  Producers: %d threads x %d events | Buffer capacity: %d | Consumer: 1 thread\n",
           producers, events, capacity);
    printf("  Sync: pthread mutex + condition variables not_full / not_empty\n");
    printf("  Log file: %s\n", LOG_PATH);
    printf("  Threads in this process before start: %d\n\n", thread_count_self());

    if (logger_start(LOG_PATH, capacity, 1) != 0)
        return EXIT_RUNTIME;
    L.consumer_delay_us = 3000;     /* slow consumer so the bounded buffer really fills */

    logger_log("main", "logger demo started: %d producers x %d events, capacity %d",
               producers, events, capacity);

    pthread_t   tids[16];
    ProducerArg args[16];
    int created = 0;
    for (int i = 0; i < producers; i++) {
        args[i].index  = i + 1;
        args[i].events = events;
        snprintf(args[i].name, sizeof args[i].name, "producer-%d", i + 1);
        int rc = pthread_create(&tids[i], NULL, producer_main, &args[i]);
        if (rc != 0) {
            fprintf(stderr, "error: pthread_create(producer %d): %s\n", i + 1, strerror(rc));
            break;
        }
        created++;
    }
    printf("    [main      ] threads now running: %d (main + consumer + %d producers)\n",
           thread_count_self(), created);

    for (int i = 0; i < created; i++) {
        pthread_join(tids[i], NULL);
    }
    echo_line("    [main      ] all %d producer threads joined\n", created);
    logger_log("main", "all producers joined, requesting logger shutdown");

    LoggerStats st;
    int rc = logger_stop(&st);
    printf("    [main      ] consumer thread joined\n");

    long expected = (long)created * events + 2;      /* + the two "main" entries */
    printf("\n  Summary\n");
    printf("  -------\n");
    printf("  Entries produced              : %ld\n", st.produced);
    printf("  Entries written to log        : %ld\n", st.consumed);
    printf("  Expected entries              : %ld\n", expected);
    printf("  Producer waits (buffer full)  : %ld\n", st.full_waits);
    printf("  Consumer waits (buffer empty) : %ld\n", st.empty_waits);
    printf("  Peak buffer occupancy         : %d/%d\n", st.max_occupancy, st.capacity);
    printf("  Threads in process now        : %d (only main remains)\n", thread_count_self());
    int ok = rc == 0 && created == producers && st.produced == expected && st.consumed == expected;
    printf("  Result                        : %s\n", ok ? "PASS - no entries lost, all threads terminated cleanly"
                                                    : "FAIL - see messages above");
    return ok ? EXIT_OK : EXIT_RUNTIME;
}

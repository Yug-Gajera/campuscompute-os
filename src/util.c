/*
 * util.c - small shared helpers: strict integer parsing, timing, banners.
 */
#include "campus.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/*
 * Parse a whole string as a base-10 integer in [min, max].
 * Rejects empty strings, trailing junk ("5x"), and overflow.
 * Returns 0 on success, -1 on failure.
 */
int parse_int(const char *text, int min, int max, int *out)
{
    if (text == NULL || *text == '\0')
        return -1;
    errno = 0;
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return -1;
    if (v < min || v > max)
        return -1;
    *out = (int)v;
    return 0;
}

/* Monotonic clock in seconds - used for elapsed-time measurements. */
double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void print_banner(const char *title)
{
    printf("\n==================================================================\n");
    printf(" CampusCompute | %s\n", title);
    printf("==================================================================\n");
}

/* Create ./logs if it does not exist. */
int ensure_log_dir(void)
{
    if (mkdir(LOG_DIR, 0755) == -1 && errno != EEXIST) {
        fprintf(stderr, "error: cannot create '%s': %s\n", LOG_DIR, strerror(errno));
        return -1;
    }
    return 0;
}

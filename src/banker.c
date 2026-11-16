/*
 * banker.c - FR5: Banker's safety algorithm.
 *
 * State file format ('#' starts a comment, whitespace is free-form):
 *     processes <n>
 *     resources <m>
 *     available a1 .. am
 *     allocation           followed by n rows of m integers
 *     max                  followed by n rows of m integers
 *
 * Need[i][j] = Max[i][j] - Allocation[i][j].
 * Validation rejects negative values and Allocation > Max.
 *
 * Safety algorithm (Silberschatz):
 *   Work = Available, Finish[i] = false
 *   repeat passes over P0..Pn-1 in index order:
 *       if !Finish[i] and Need[i] <= Work:  Work += Allocation[i], Finish[i] = true,
 *                                           append Pi to the safe sequence
 *   until a full pass makes no progress.
 *   Safe  <=> every Finish[i] is true.
 */
#include "campus.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ---- tiny tokenizer that skips '#' comments ------------------------------ */
typedef struct { FILE *fp; int line; } Tok;

/*
 * Read the next word from the state file into 'out', skipping spaces, blank
 * lines and '#' comments. Keeps a line counter so errors can name the line.
 * Returns -1 at end of file.
 */
static int next_token(Tok *t, char *out, size_t len)
{
    int c;
    size_t n = 0;
    for (;;) {                                         /* skip whitespace + comments */
        c = fgetc(t->fp);
        if (c == EOF) return -1;
        if (c == '\n') { t->line++; continue; }
        if (c == '#') { while ((c = fgetc(t->fp)) != '\n' && c != EOF) {} if (c == '\n') t->line++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') continue;
        break;
    }
    while (c != EOF && c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '#') {
        if (n + 1 < len) out[n++] = (char)c;
        c = fgetc(t->fp);
    }
    if (c != EOF) ungetc(c, t->fp);
    out[n] = '\0';
    return 0;
}

/*
 * The next word must be exactly 'word' (e.g. "allocation"), otherwise report
 * where the file went wrong.
 */
static int expect_word(Tok *t, const char *path, const char *word)
{
    char tok[64];
    if (next_token(t, tok, sizeof tok) != 0) {
        fprintf(stderr, "  %s:%d: unexpected end of file, expected '%s'\n", path, t->line + 1, word);
        return -1;
    }
    if (strcmp(tok, word) != 0) {
        fprintf(stderr, "  %s:%d: expected keyword '%s', found '%s'\n", path, t->line + 1, word, tok);
        return -1;
    }
    return 0;
}

/*
 * The next word must be an integer in [min, max]; 'what' names the value in
 * the error message, e.g. "Allocation[P1][R0]".
 */
static int expect_int(Tok *t, const char *path, const char *what, int min, int max, int *out)
{
    char tok[64];
    if (next_token(t, tok, sizeof tok) != 0) {
        fprintf(stderr, "  %s:%d: unexpected end of file while reading %s\n", path, t->line + 1, what);
        return -1;
    }
    if (parse_int(tok, min, max, out) != 0) {
        fprintf(stderr, "  %s:%d: %s must be an integer in [%d, %d] (got '%s')\n",
                path, t->line + 1, what, min, max, tok);
        return -1;
    }
    return 0;
}

/*
 * Parse and validate a state file in a fixed order:
 * processes n, resources m, available (m values), allocation (n x m), max (n x m).
 * Any problem -> clear message with the line number and the file is rejected.
 */
int banker_load(const char *path, BankState *s)
{
    memset(s, 0, sizeof *s);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "error: cannot open state file '%s': %s\n", path, strerror(errno));
        return -1;
    }
    Tok t = { fp, 0 };
    int rc = -1;
    char what[64];

    if (expect_word(&t, path, "processes") || expect_int(&t, path, "process count", 1, BANK_MAX_P, &s->n)) goto out;
    if (expect_word(&t, path, "resources") || expect_int(&t, path, "resource count", 1, BANK_MAX_R, &s->m)) goto out;
    if (expect_word(&t, path, "available")) goto out;
    for (int j = 0; j < s->m; j++) {
        snprintf(what, sizeof what, "Available[R%d]", j);
        if (expect_int(&t, path, what, 0, 1000000, &s->available[j])) goto out;
    }
    if (expect_word(&t, path, "allocation")) goto out;
    for (int i = 0; i < s->n; i++)
        for (int j = 0; j < s->m; j++) {
            snprintf(what, sizeof what, "Allocation[P%d][R%d]", i, j);
            if (expect_int(&t, path, what, 0, 1000000, &s->alloc[i][j])) goto out;
        }
    if (expect_word(&t, path, "max")) goto out;
    for (int i = 0; i < s->n; i++)
        for (int j = 0; j < s->m; j++) {
            snprintf(what, sizeof what, "Max[P%d][R%d]", i, j);
            if (expect_int(&t, path, what, 0, 1000000, &s->max[i][j])) goto out;
        }
    {
        char extra[64];
        if (next_token(&t, extra, sizeof extra) == 0) {
            fprintf(stderr, "  %s:%d: unexpected extra data '%s' after max matrix\n", path, t.line + 1, extra);
            goto out;
        }
    }
    /* semantic check: a process can never hold more than it declared as maximum */
    /*
     * Semantic check + Need matrix. A process can never hold more than it
     * declared as its maximum, so Allocation > Max is rejected.
     * Need[i][j] = Max[i][j] - Allocation[i][j] = how much more Pi may still ask for.
     */
    rc = 0;
    for (int i = 0; i < s->n; i++)
        for (int j = 0; j < s->m; j++) {
            if (s->alloc[i][j] > s->max[i][j]) {
                fprintf(stderr, "  %s: Allocation[P%d][R%d]=%d exceeds Max=%d\n", path, i, j,
                        s->alloc[i][j], s->max[i][j]);
                rc = -1;
            }
            s->need[i][j] = s->max[i][j] - s->alloc[i][j];
        }
out:
    fclose(fp);
    if (rc != 0)
        fprintf(stderr, "error: state file '%s' rejected\n", path);
    return rc;
}

static void print_vec(const int *v, int m)
{
    printf("(");
    for (int j = 0; j < m; j++) printf("%s%d", j ? " " : "", v[j]);
    printf(")");
}

/* Print the Available vector and the Allocation / Max / Need matrices. */
void banker_print(const BankState *s)
{
    printf("  Processes: %d   Resource types: %d\n", s->n, s->m);
    printf("  Available: ");
    print_vec(s->available, s->m);
    printf("\n\n  %-5s %-*s %-*s %-*s\n", "Proc", s->m * 3 + 3, "Allocation", s->m * 3 + 3, "Max",
           s->m * 3 + 3, "Need (Max-Alloc)");
    for (int i = 0; i < s->n; i++) {
        printf("  P%-4d ", i);
        for (int k = 0; k < 3; k++) {
            const int *row = k == 0 ? s->alloc[i] : k == 1 ? s->max[i] : s->need[i];
            for (int j = 0; j < s->m; j++) printf("%3d", row[j]);
            printf("   ");
        }
        printf("\n");
    }
}

/*
 * SAFETY ALGORITHM. Returns 1 if the state is safe, 0 if unsafe, and fills
 * sequence[] with the order in which the processes can finish.
 *   Work   = resources currently free (starts as Available)
 *   Finish = which processes we have shown can complete
 */
int banker_check(const BankState *s, int *sequence, int verbose)
{
    int work[BANK_MAX_R], finish[BANK_MAX_P] = {0}, count = 0;
    memcpy(work, s->available, sizeof work);

    if (verbose) {
        printf("\n  Safety algorithm trace (Work starts as Available = ");
        print_vec(work, s->m);
        printf(")\n");
    }
    int pass = 0, progress = 1;
    /*
     * Keep making passes over P0..Pn-1 while the previous pass let at least one
     * process finish. When a pass makes no progress, nobody else can finish.
     */
    while (progress && count < s->n) {
        progress = 0;
        pass++;
        for (int i = 0; i < s->n; i++) {
            if (finish[i]) continue;
            /*
             * Can Pi finish with what is free right now? Only if Need[i] <= Work in
             * EVERY resource type.
             */
            int fits = 1;
            for (int j = 0; j < s->m; j++)
                if (s->need[i][j] > work[j]) { fits = 0; break; }
            if (verbose) {
                printf("    pass %d: P%d Need ", pass, i);
                print_vec(s->need[i], s->m);
                printf(fits ? " <= Work " : " >  Work ");
                print_vec(work, s->m);
            }
            if (!fits) {
                if (verbose) printf("  -> must wait\n");
                continue;
            }
            /*
             * Pi gets its remaining need, runs to completion and then RELEASES
             * everything it holds, so its allocation becomes free for the others.
             */
            for (int j = 0; j < s->m; j++) work[j] += s->alloc[i][j];
            finish[i] = 1;
            sequence[count++] = i;
            progress = 1;
            if (verbose) {
                printf("  -> runs, releases Allocation; Work = ");
                print_vec(work, s->m);
                printf("\n");
            }
        }
    }

    /*
     * Safe if and only if every process could finish in some order. Unsafe does
     * not mean deadlock has happened - only that it can no longer be ruled out.
     */
    int safe = count == s->n;
    if (verbose) {
        if (safe) {
            printf("\n  RESULT: SAFE state. Safe sequence: < ");
            for (int k = 0; k < count; k++) printf("P%d%s", sequence[k], k + 1 < count ? ", " : " >\n");
        } else {
            printf("\n  RESULT: UNSAFE state. No safe sequence exists.\n");
            printf("  Completed before getting stuck: ");
            if (count == 0) printf("none");
            for (int k = 0; k < count; k++) printf("P%d ", sequence[k]);
            printf("\n  Processes that can never be guaranteed to finish: ");
            for (int i = 0; i < s->n; i++) if (!finish[i]) printf("P%d ", i);
            printf("\n  (Final Work = ");
            print_vec(work, s->m);
            printf(" cannot cover the Need of any remaining process.)\n");
        }
    }
    return safe;
}

/*
 * 'banker <file>' command: load, print the matrices, run the check with a
 * step-by-step trace, and record the verdict in the activity log.
 */
int banker_command(const char *path, int verbose)
{
    BankState s;
    if (banker_load(path, &s) != 0)
        return EXIT_USAGE;
    if (verbose) {
        printf("  State file: %s\n", path);
        banker_print(&s);
    }
    int seq[BANK_MAX_P];
    int safe = banker_check(&s, seq, verbose);
    char buf[128] = "";
    size_t len = 0;
    for (int k = 0; safe && k < s.n; k++)
        len += (size_t)snprintf(buf + len, sizeof buf - len, "P%d ", seq[k]);
    logger_log("BANKER", "%s -> %s %s", path, safe ? "SAFE" : "UNSAFE", buf);
    return EXIT_OK;      /* the algorithm ran correctly whether the state is safe or not */
}

/*
 * menu.c - FR8: interactive terminal menu (text user interface).
 *
 *   ./campuscompute menu
 *
 * Arrow keys (or j/k) move the highlight, Enter runs the selected feature,
 * 's' changes settings, 'q' quits. Number keys 1-9 jump to an item.
 * Each feature is run as a fresh child process of this same executable
 * (fork + execv of /proc/self/exe), exactly as if its command had been typed,
 * so the menu never changes how a feature behaves. The parent waits with
 * waitpid() and shows the exit status.
 *
 * The terminal is switched to non-canonical, no-echo mode with termios only
 * while the menu is drawn, and is always restored (normal return, Ctrl+C,
 * or before running a feature). If stdin is not a terminal, a plain numbered
 * menu read line-by-line is used instead.
 */
#include "campus.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define C_RESET "\033[0m"
#define C_BOLD  "\033[1m"
#define C_DIM   "\033[2m"
#define C_REV   "\033[7m"
#define C_CYAN  "\033[36m"
#define C_GREEN "\033[32m"
#define C_RED   "\033[31m"
#define C_YEL   "\033[33m"

static struct termios saved_tio;
static int raw_on = 0;

/* Put the terminal back exactly as it was (line editing + echo on). */
static void raw_off(void)
{
    if (raw_on) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
        raw_on = 0;
    }
    fputs("\033[?25h", stdout);                 /* show cursor */
    fflush(stdout);
}

/*
 * 'Raw' input: turn off ICANON (line buffering) and ECHO so every key press
 * reaches the program immediately and is not printed. VMIN=1: read() waits
 * for at least one byte.
 */
static void raw_enable(void)
{
    if (raw_on) return;                         /* keep queued key presses */
    struct termios t = saved_tio;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    raw_on = 1;
    fputs("\033[?25l", stdout);                 /* hide cursor */
}

/*
 * On Ctrl+C / kill: restore the terminal first, then let the signal do its
 * default job, so the user's shell is never left in raw mode.
 */
static void on_signal(int sig)
{
    raw_off();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ---- settings shared by the menu items ---------------------------------- */
static char s_jobs[256]  = "tests/jobs_sample.txt";
static char s_bank[256]  = "tests/banker_unsafe.txt";
static char s_quant[16]  = "2";

typedef struct {
    const char *group;      /* non-NULL: print a group heading before this item */
    const char *label;
    int         id;
} Item;

enum { M_JOBS, M_FCFS, M_RR, M_PRIO, M_CMP, M_LOG, M_IPC, M_BSAFE, M_BFILE,
       M_WNORM, M_WFAIL, M_RLIM, M_CLEAN, M_INVALID, M_DEMO, M_LOGFILE, M_SET, M_QUIT };

static const Item ITEMS[] = {
    { "Jobs & CPU scheduling (FR1, FR2)", "Load and validate jobs",            M_JOBS },
    { NULL, "FCFS scheduling",                                                M_FCFS },
    { NULL, "Round Robin scheduling",                                         M_RR },
    { NULL, "Priority scheduling (non-preemptive)",                           M_PRIO },
    { NULL, "Compare all three policies",                                     M_CMP },
    { "Concurrency & IPC (FR3, FR4)", "Threaded producer-consumer logger",    M_LOG },
    { NULL, "Shared-memory IPC between processes",                            M_IPC },
    { "Resource safety (FR5)", "Banker's algorithm - safe state",             M_BSAFE },
    { NULL, "Banker's algorithm - state file from settings",                  M_BFILE },
    { "Processes, limits & cleanup (FR6, FR7)", "Workers - normal completion", M_WNORM },
    { NULL, "Workers - simulated failures",                                   M_WFAIL },
    { NULL, "Resource limits (setrlimit)",                                    M_RLIM },
    { NULL, "Cleanup verification",                                           M_CLEAN },
    { "Other (FR8)", "Invalid input / error handling tests",                  M_INVALID },
    { NULL, "Integrated end-to-end demo",                                     M_DEMO },
    { NULL, "Show last 20 log entries",                                       M_LOGFILE },
    { NULL, "Settings (job file, quantum, Banker file)",                      M_SET },
    { NULL, "Quit",                                                           M_QUIT },
};
#define NITEMS ((int)(sizeof ITEMS / sizeof ITEMS[0]))

/*
 * Redraw the whole menu. \033[2J\033[H clears the screen and moves the cursor
 * home; the selected item is drawn in reverse video.
 */
static void draw(int sel)
{
    printf("\033[2J\033[H");
    printf(C_CYAN C_BOLD "  ╔══════════════════════════════════════════════════════════════╗\n");
    printf("  ║  CampusCompute - Intelligent Job Scheduling & Resource Mgmt  ║\n");
    printf("  ╚══════════════════════════════════════════════════════════════╝" C_RESET "\n");
    printf(C_DIM "   Job file: " C_RESET "%s" C_DIM "   RR quantum: " C_RESET "%s"
           C_DIM "   Banker file: " C_RESET "%s\n", s_jobs, s_quant, s_bank);
    for (int i = 0; i < NITEMS; i++) {
        if (ITEMS[i].group)
            printf("\n  " C_YEL C_BOLD "%s" C_RESET "\n", ITEMS[i].group);
        if (i == sel)
            printf("   " C_REV C_BOLD " > %2d  %-50s " C_RESET "\n", i + 1, ITEMS[i].label);
        else
            printf("      %2d  %s\n", i + 1, ITEMS[i].label);
    }
    printf("\n" C_DIM "   Up/Down or j/k: move   Enter: run   1-9: jump   s: settings   q: quit" C_RESET "\n");
    fflush(stdout);
}

/* Read one key; returns 'U' / 'D' for arrows, otherwise the character. */
/* Arrow keys arrive as 3-byte escape sequences: ESC '[' 'A' (up) / 'B' (down). */
static int read_key(void)
{
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return 'q';
    if (c == '\033') {
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return 27;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return 27;
        if (seq[0] == '[' && seq[1] == 'A') return 'U';
        if (seq[0] == '[' && seq[1] == 'B') return 'D';
        return 0;
    }
    return c;
}

/* Run "<self> args..." in a child process and report its exit status. */
/*
 * Run one feature: fork() a child that execv()s this same program with the
 * feature's arguments, and wait for it with waitpid() in the parent.
 */
static int run_self(const char *self, char *const args[])
{
    printf(C_CYAN C_BOLD "\n$ ./campuscompute");
    for (int i = 1; args[i]; i++) printf(" %s", args[i]);
    printf(C_RESET "\n");
    fflush(stdout);

    pid_t pid = fork();
    if (pid == -1) { perror("fork"); return -1; }
    if (pid == 0) {
        execv(self, args);
        perror("execv");
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) == -1)
        if (errno != EINTR) { perror("waitpid"); return -1; }
    if (WIFEXITED(st)) {
        int rc = WEXITSTATUS(st);
        printf("\n%s[exit status %d: %s]" C_RESET "\n", rc == 0 ? C_GREEN : C_RED, rc,
               rc == 0 ? "finished OK" : rc == 2 ? "invalid usage/input" : "runtime failure");
        return rc;
    }
    printf(C_RED "\n[terminated by signal %d]" C_RESET "\n", WTERMSIG(st));
    return -1;
}

static void prompt_setting(const char *name, char *buf, size_t len)
{
    char line[256];
    printf("  %s [%s]: ", name, buf);
    fflush(stdout);
    if (fgets(line, sizeof line, stdin) == NULL) return;
    line[strcspn(line, "\n")] = '\0';
    if (line[0] != '\0') snprintf(buf, len, "%s", line);
}

static void settings(void)
{
    printf("\n" C_BOLD "  Settings" C_RESET " (press Enter to keep the current value)\n");
    printf(C_DIM "  Job files: tests/jobs_sample.txt  tests/jobs_ties.txt  tests/jobs_invalid.txt\n");
    printf("  Banker files: tests/banker_safe.txt  tests/banker_unsafe.txt  tests/banker_invalid.txt" C_RESET "\n");
    prompt_setting("Job file", s_jobs, sizeof s_jobs);
    int q;
    char qbuf[16];
    snprintf(qbuf, sizeof qbuf, "%s", s_quant);
    prompt_setting("RR quantum (1-100)", qbuf, sizeof qbuf);
    if (parse_int(qbuf, 1, 100, &q) == 0) snprintf(s_quant, sizeof s_quant, "%d", q);
    else printf(C_RED "  quantum must be an integer 1..100 - kept %s" C_RESET "\n", s_quant);
    prompt_setting("Banker state file", s_bank, sizeof s_bank);
}

/* Perform one menu action. Returns 0 to continue, 1 to quit. */
static int perform(const char *self, int id)
{
    char *s = (char *)self;
    switch (id) {
    case M_JOBS:  { char *a[] = { s, "jobs", s_jobs, NULL };              run_self(self, a); break; }
    case M_FCFS:  { char *a[] = { s, "fcfs", s_jobs, NULL };              run_self(self, a); break; }
    case M_RR:    { char *a[] = { s, "rr", s_jobs, s_quant, NULL };       run_self(self, a); break; }
    case M_PRIO:  { char *a[] = { s, "priority", s_jobs, NULL };          run_self(self, a); break; }
    case M_CMP:   { char *a[] = { s, "compare", s_jobs, s_quant, NULL };  run_self(self, a); break; }
    case M_LOG:   { char *a[] = { s, "logger", "3", "5", "4", NULL };     run_self(self, a); break; }
    case M_IPC:   { char *a[] = { s, "ipc", s_jobs, NULL };               run_self(self, a); break; }
    case M_BSAFE: { char *a[] = { s, "banker", "tests/banker_safe.txt", NULL }; run_self(self, a); break; }
    case M_BFILE: { char *a[] = { s, "banker", s_bank, NULL };            run_self(self, a); break; }
    case M_WNORM: { char *a[] = { s, "workers", "normal", "3", NULL };    run_self(self, a); break; }
    case M_WFAIL: { char *a[] = { s, "workers", "fail", "all", NULL };    run_self(self, a); break; }
    case M_RLIM:  { char *a[] = { s, "rlimit", "all", NULL };             run_self(self, a); break; }
    case M_CLEAN: { char *a[] = { s, "cleanup-check", NULL };             run_self(self, a); break; }
    case M_INVALID: {
        char *a1[] = { s, "frobnicate", NULL };
        char *a2[] = { s, "rr", s_jobs, "0", NULL };
        char *a3[] = { s, "jobs", "tests/jobs_invalid.txt", NULL };
        run_self(self, a1); run_self(self, a2); run_self(self, a3);
        break;
    }
    case M_DEMO:  { char *a[] = { s, "demo", s_jobs, "tests/banker_safe.txt", NULL }; run_self(self, a); break; }
    case M_LOGFILE: {
        printf(C_CYAN C_BOLD "\n$ tail -n 20 %s" C_RESET "\n", LOG_PATH);
        fflush(stdout);
        FILE *fp = fopen(LOG_PATH, "r");
        if (!fp) { printf("  no log file yet - run a feature first\n"); break; }
        char lines[20][320];
        int n = 0;
        while (fgets(lines[n % 20], sizeof lines[0], fp)) n++;
        fclose(fp);
        for (int i = n > 20 ? n - 20 : 0; i < n; i++) fputs(lines[i % 20], stdout);
        break;
    }
    case M_SET:   settings(); return 0;
    case M_QUIT:  return 1;
    }
    return 0;
}

static int menu_plain(const char *self)
{
    char line[64];
    for (;;) {
        printf("\nCampusCompute menu (job file %s, quantum %s, Banker file %s)\n", s_jobs, s_quant, s_bank);
        for (int i = 0; i < NITEMS; i++) printf("  %2d. %s\n", i + 1, ITEMS[i].label);
        printf("Choose 1-%d: ", NITEMS);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) return EXIT_OK;
        line[strcspn(line, "\n")] = '\0';
        int k;
        if (parse_int(line, 1, NITEMS, &k) != 0) { printf("error: enter a number 1-%d\n", NITEMS); continue; }
        if (perform(self, ITEMS[k - 1].id)) return EXIT_OK;
    }
}

/*
 * /proc/self/exe is a link to this program's own executable file, so the
 * menu can start itself again with different arguments.
 */
int menu_run(void)
{
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) { perror("readlink /proc/self/exe"); return EXIT_RUNTIME; }
    self[n] = '\0';

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return menu_plain(self);

    if (tcgetattr(STDIN_FILENO, &saved_tio) == -1) { perror("tcgetattr"); return EXIT_RUNTIME; }
    atexit(raw_off);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int sel = 0;
    for (;;) {
        raw_enable();
        draw(sel);
        int k = read_key();
        if (k == 'U' || k == 'k') { sel = (sel + NITEMS - 1) % NITEMS; continue; }
        if (k == 'D' || k == 'j') { sel = (sel + 1) % NITEMS; continue; }
        if (k >= '1' && k <= '9') { sel = k - '1'; continue; }
        if (k == 'q' || k == 'Q') break;
        if (k == 's' || k == 'S') { raw_off(); settings(); continue; }
        if (k != '\n' && k != '\r') continue;

        raw_off();
        printf("\033[2J\033[H");
        if (perform(self, ITEMS[sel].id)) break;
        printf(C_DIM "\n  Press Enter to return to the menu..." C_RESET);
        fflush(stdout);
        int c;
        while ((c = getchar()) != '\n' && c != EOF) { }
    }
    raw_off();
    printf("\033[2J\033[H  Bye.\n");
    return EXIT_OK;
}

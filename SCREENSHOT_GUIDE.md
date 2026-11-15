# Screenshot run-sheet (S01–S24)

This file is for you only. It is **not** part of the submission ZIP.

## Before you start

1. Copy the `campuscompute` folder into your WSL home (e.g. `~/campuscompute`), not
   `/mnt/c/...`. Shared memory and file permissions behave better there.
2. Open a fresh Ubuntu terminal, maximise it, and set your prompt (use **your** full SRN):

```bash
export SRN='PES1UG24CSXXX'          # <- replace with your real SRN
export PS1='[$SRN] \w\$ '
cd ~/campuscompute
```

3. For long outputs, scroll so the command line and the key result are both visible, or
   take two screenshots of the same command. Each screenshot needs the prompt with your SRN.

The suggested captions are drafts; reword them in your own words.

| # | Run this | Suggested one-line caption |
|---|---|---|
| S01 | `cat /etc/os-release \| head -2; uname -r; pwd` | Ubuntu (WSL) terminal with my SRN in the prompt and the project working directory. |
| S02 | `gcc --version \| head -1; make --version \| head -1` | GCC and GNU Make are installed and available. |
| S03 | `ls -l; ls -l include src tests` | Project directory with the Makefile, header, C source files and test inputs. |
| S04 | `make clean && make` | Clean build from source with -Wall -Wextra -Wpedantic and zero warnings or errors. |
| S05 | `./campuscompute help` | CLI usage listing every command, the tie-breaking rules and exit codes. |
| S06 | `./campuscompute fcfs tests/jobs_sample.txt` | FCFS execution order, Gantt chart, per-job CT/TAT/WT and averages. |
| S07 | `./campuscompute rr tests/jobs_sample.txt 2` | Round Robin with time quantum 2: execution order, Gantt chart and metrics. |
| S08 | `./campuscompute priority tests/jobs_sample.txt` then `./campuscompute priority tests/jobs_ties.txt` | Non-preemptive priority scheduling; the second run shows the tie-break (equal priority → earlier arrival → lower ID). |
| S09 | `./campuscompute compare tests/jobs_sample.txt 2` | Comparison of FCFS, RR and Priority on the same job set. |
| S10 | `./campuscompute logger 3 5 4` (top half) | Three producer threads and one consumer sharing a bounded buffer of 4; producers block when it is full. |
| S11 | same run (bottom half / summary) | All 17 entries written, producers and consumer joined, only the main thread remains. |
| S12 | `./campuscompute ipc tests/jobs_sample.txt` (top half) | Submitter and dispatcher processes exchange jobs through POSIX shared memory. |
| S13 | same run (bottom half), then `ls /dev/shm \| grep campuscompute \|\| echo "no CampusCompute IPC objects"` | Named semaphores coordinate the ring buffer and all shared memory/semaphores are unlinked afterwards. |
| S14 | `./campuscompute banker tests/banker_safe.txt` | Banker's algorithm finds the safe sequence <P1, P3, P4, P0, P2>. |
| S15 | `./campuscompute banker tests/banker_unsafe.txt` | Banker's algorithm detects an unsafe state: P0 and P4 can never be guaranteed to finish. |
| S16 | `./campuscompute workers normal 3` | Three forked workers complete normally and waitpid() reports exit status 0. |
| S17 | `./campuscompute workers fail all` | Simulated failures (exit 3, abort, hang) detected by the parent via waitpid() and a watchdog. |
| S18 | `./campuscompute rlimit all` | setrlimit() in child processes: SIGXCPU after 1 s CPU, EFBIG beyond 4096 bytes, 256 MiB malloc refused. |
| S19 | `./campuscompute frobnicate; ./campuscompute rr tests/jobs_sample.txt 0; ./campuscompute jobs tests/jobs_invalid.txt` | Invalid command, invalid quantum and a malformed job file each produce a specific error message. |
| S20 | `./campuscompute cleanup-check; ps -e \| grep campuscompute \|\| echo "no campuscompute processes"` | No leftover CampusCompute processes, threads, shared memory, semaphores or temp files. |
| S21 | `./campuscompute demo` (scroll to the summary) | Integrated run of all features with an 8/8 PASS summary. |
| S22 | `ls -l logs/; tail -n 20 logs/campuscompute.log` | Log file written by the consumer thread with timestamped, sequenced events. |
| S23 | `make && ls -l Makefile README.md include src tests campuscompute` | Final source files, Makefile and the built executable. |
| S24 | `make zip SRN=$SRN` | Submission ZIP created; `unzip -l` lists its contents and `unzip -t` reports no errors. |

Do S23 before S24: `make zip` runs `make clean` first so the ZIP holds only source files.

## Building the PDF

Put each screenshot in a document in order S01–S24, with the caption under each one,
and export it as `<your SRN>.pdf`. No separate report.

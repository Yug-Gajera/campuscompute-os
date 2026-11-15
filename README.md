# CampusCompute

Intelligent job scheduling and resource management prototype for a shared university
computing lab. Operating Systems (UE25CS242A) Level 3 mini-project, written in C for
Linux / Ubuntu / WSL Ubuntu.

## Prerequisites

Ubuntu 22.04+ or WSL 2 Ubuntu with GCC, GNU Make and zip:

```bash
sudo apt update
sudo apt install build-essential zip
```

## Build

```bash
make            # builds ./campuscompute (gcc -Wall -Wextra -Wpedantic, links -pthread -lrt)
make clean      # removes build/ and the executable
make distclean  # also removes logs/
```

Manual build without make:

```bash
mkdir -p build
gcc -std=gnu11 -Wall -Wextra -Wpedantic -O2 -Iinclude -pthread src/*.c -o campuscompute -lrt
```

## Commands

Run `./campuscompute help` for the full list. Every feature is its own command so it
can be tested independently.

| Requirement | Command | What it shows |
|---|---|---|
| FR1 | `./campuscompute jobs tests/jobs_sample.txt` | jobs loaded, validated, status NEW |
| FR2 | `./campuscompute fcfs tests/jobs_sample.txt` | FCFS order, Gantt chart, CT/TAT/WT/RT, averages |
| FR2 | `./campuscompute rr tests/jobs_sample.txt 2` | Round Robin with quantum 2 |
| FR2 | `./campuscompute priority tests/jobs_sample.txt` | non-preemptive priority |
| FR2 | `./campuscompute compare tests/jobs_sample.txt 2` | all three policies on the same job set |
| FR3 | `./campuscompute logger 3 5 4` | 3 producer threads, 1 consumer, buffer of 4 |
| FR4 | `./campuscompute ipc tests/jobs_sample.txt` | shared memory + named semaphores between 2 processes |
| FR5 | `./campuscompute banker tests/banker_safe.txt` | safe state and safe sequence |
| FR5 | `./campuscompute banker tests/banker_unsafe.txt` | unsafe state detected |
| FR6 | `./campuscompute workers normal 3` | fork() workers that complete normally |
| FR6 | `./campuscompute workers fail all` | simulated exit(3), abort() and hang, detected with waitpid() |
| FR7 | `./campuscompute rlimit all` | RLIMIT_CPU, RLIMIT_FSIZE and RLIMIT_AS applied in children |
| FR7 | `./campuscompute cleanup-check` | no leftover processes, shared memory, semaphores or temp files |
| FR8 | `./campuscompute jobs tests/jobs_invalid.txt`, `./campuscompute xyz` | error and usage messages |
| All | `./campuscompute demo` | integrated run with PASS/FAIL summary |

`make test` runs every feature once.

Exit codes: `0` success, `1` runtime failure, `2` invalid command or input.

## Input files

Job file (`tests/jobs_*.txt`), one job per line, `#` starts a comment:

```
# job_id  arrival  cpu_burst  priority
1   0   5   3
```

* job ID: positive and unique; arrival 0..100000; burst 1..10000; priority 0..99
* **lower priority number = higher priority**
* every invalid line is reported with its line number and the whole file is rejected

Banker state file (`tests/banker_*.txt`): keywords `processes`, `resources`, `available`,
`allocation` (n rows), `max` (n rows). Need is computed as Max − Allocation; Allocation
greater than Max is rejected.

## Scheduling rules

* **FCFS**: earlier arrival first; equal arrival goes to the smaller job ID.
* **Round Robin**: default time quantum **2** (any 1..100 can be passed). FIFO ready queue;
  jobs arriving at the same time join in job-ID order; a job arriving exactly when a quantum
  expires is queued **before** the preempted job.
* **Priority (non-preemptive)**: smallest priority number first; tie goes to earlier
  arrival, then smaller job ID. A running job is never preempted.
* If no job is ready the CPU is shown as `IDLE` until the next arrival.
* CT = completion time, TAT = CT − arrival, WT = TAT − burst, RT = first start − arrival.

## Design notes

* **Logger (FR3)**: bounded circular buffer protected by one `pthread_mutex_t`, with
  condition variables `not_full` (producers wait) and `not_empty` (consumer waits).
  Shutdown sets a `stopping` flag and broadcasts; the consumer drains every remaining
  entry before exiting, and the main thread joins all threads. Log file:
  `logs/campuscompute.log`. Every command except `help`, `logger` and `cleanup-check`
  also records its events through this logger.
* **IPC (FR4)**: `shm_open` + `mmap` region holding a 3-slot ring buffer. A submitter
  process writes jobs, a dispatcher process reads them, coordinated by named semaphores
  `sem_empty`, `sem_full` and `sem_mutex`. Object names include the creating PID
  (`/campuscompute_shm_<pid>`) and are created with `O_EXCL`, so the program only ever
  unlinks objects it created.
* **Workers (FR6)**: the parent polls `waitpid(pid, &status, WNOHANG)` and decodes
  `WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG`. A 2-second watchdog kills hung workers
  with SIGKILL. Failures are **simulated on purpose** and labelled `[SIMULATED]`.
  Core dumps are disabled in the aborting worker so no core file is left behind.
* **Resource limits (FR7)**: limits are set in child processes only, so the shell and
  main program are unaffected. `RLIMIT_CPU` 1 s → SIGXCPU; `RLIMIT_FSIZE` 4096 B →
  `write()` fails with EFBIG; `RLIMIT_AS` = current size + 64 MiB → a 256 MiB `mmap` is refused with ENOMEM.

## Cleanup

The program removes everything it creates: threads are joined, children are reaped,
shared memory and semaphores are unlinked, the temporary file from the FSIZE test is
deleted. To verify:

```bash
./campuscompute cleanup-check
ls /dev/shm | grep campuscompute       # prints nothing when clean
```

If a run is killed with Ctrl+C mid-way, `./campuscompute cleanup-check --remove-stale`
removes only CampusCompute objects whose creating process no longer exists.

## Project layout

```
Makefile  README.md
include/campus.h          shared types and interfaces
src/main.c                FR8 command-line interface
src/jobs.c                FR1 job model and validation
src/scheduler.c           FR2 FCFS / RR / Priority
src/logger.c              FR3 pthread producer-consumer logger
src/ipc.c                 FR4 POSIX shared memory + semaphores
src/banker.c              FR5 Banker's safety algorithm
src/workers.c             FR6 fork/waitpid worker supervision
src/rlimit.c              FR7 setrlimit demonstrations
src/cleanup.c             FR7 cleanup verification
src/demo.c                integrated demonstration
src/util.c                helpers
tests/                    repeatable input files
```

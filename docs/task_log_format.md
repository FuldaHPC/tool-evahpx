# The task log of HPX (prototype)

The task log records what the worker threads of one locality do: which task runs on which worker, when it was created and by whom, when it suspends and resumes, and which future hands a value from one task to another. It needs no profiler. It is built in by `scripts/install-hpx.sh --with-task-trace` (the task hooks of HPX are compiled in; they cost a little even when the log is off) and switched on at run time:

| variable | meaning |
|---|---|
| `HPX_TASK_LOG=<prefix>` | switch the log on. Process `r` writes `<prefix>.tasks.loc<r>.bin` (`r` = `OMPI_COMM_WORLD_RANK` or `PMIX_RANK`, else 0) |
| `HPX_TASK_LOG_SAMPLE=N` | record every N-th task only (HPX's own 1-in-N sampling; a task that is sampled is recorded completely). Default 1: all tasks |
| `HPX_TASK_LOG_MAX_MB=<n>` | size cap of the file of one process (default 2048); later records are dropped and a warning is printed at exit |

`scripts/eduhpx_tasklog.py FILE` prints a summary (tasks, worker threads, durations, the names that take the most time).

## File format (version 1)

Little-endian. A header of 64 bytes, then records of 32 bytes, then the table of names.

| offset | size | header field |
|---|---|---|
| 0 | 8 | `EDUHPXK1` |
| 8 | 4 | version (1) |
| 12 | 4 | record size (32) |
| 16 | 4 | locality (rank) |
| 20 | 4 | process id |
| 24 | 8 | offset of the table of names (0: the file was not closed) |
| 32 | 8 | ticks at the start |
| 40 | 8 | wall clock at the start, ns since the epoch |
| 48 | 8 | ticks at the end |
| 56 | 8 | wall clock at the end, ns since the epoch |

The times in the records are **ticks** of the CPU time stamp counter (`rdtsc`; nanoseconds of `CLOCK_MONOTONIC` on CPUs without it). Reading it costs a few nanoseconds, a call of `clock_gettime` about ten times more. The two pairs (ticks, wall clock) in the header turn ticks into wall clock time (a straight line between them), on the same clock as the times of the parcel log.

Record (32 bytes): `u8 kind, u8 flag, u16 worker, u32 name, u64 t, u64 a, u64 b`.

| kind | | `t` | `a` | `b` | `flag` | `name` |
|---|---|---|---|---|---|---|
| 1 | slice | start | task id | end | how it ended: 0 finished, 1 yielded, 2 suspended | what the task is |
| 2 | created | time | task id | id of the parent task (0: none) | | what the task is |
| 3 | suspend | time | task id | | | the reason |
| 4 | resumed | time | task id | | | what woke it |
| 5 | fulfilled | time | id of the future | task that was running (0: none) | | |
| 6 | continuation | time | id of the future whose value the continuation uses | task that was running | | |
| 7 | stolen | time | task id | victim worker | | |

`worker` is the worker thread (for *stolen*: the thief). A task that suspends and resumes has one *slice* for every time it ran. A task can run another one inside (then the slices are nested). The id of a task is its address, the id of a future the address of its shared state: *fulfilled* and *continuation* of the same future belong together, and show which task handed a value to which.

Table of names: `EDUHPXN1`, `u32 count`, then for every name `u32 id, u16 length, bytes`. The names are the descriptions HPX gives its tasks (mostly the name of the function or of the action).

# The task log of HPX (prototype)

The task log records what the worker threads of one locality do: which task runs on which worker, when it was created and by whom, when it suspends and resumes, and which future hands a value from one task to another. It needs no profiler. It is built in by `scripts/install-hpx.sh --with-task-trace` (the task hooks of HPX are compiled in and HPX keeps a description of every task; this costs a little even when the log is off) and switched on at run time:

| variable | meaning |
|---|---|
| `HPX_TASK_LOG=<prefix>` | switch the log on. Process `r` writes `<prefix>.tasks.loc<r>.bin` (`r` = `OMPI_COMM_WORLD_RANK` or `PMIX_RANK`, else 0) |
| `HPX_TASK_LOG_SAMPLE=N` | record every N-th task only (HPX's own 1-in-N sampling; a task that is sampled is recorded completely). Default 1: all tasks |
| `HPX_TASK_LOG_MAX_MB=<n>` | size cap of the file of one process (default 2048); later records are dropped and a warning is printed at exit |
| `HPX_TASK_LOG_BACKGROUND=1` | also record the task `background_work`, which the idle workers of the scheduler run over and over (left out by default: it only fills the file) |

A thread of its own writes the file, in blocks, so that a worker never waits for the disk. If the disk cannot keep up (at most 64 MB of blocks are kept), the blocks that do not fit are not written and counted: a message at the end of the program says how many records were lost (use a local disk, or `HPX_TASK_LOG_SAMPLE=N`).

`scripts/evahpx_tasklog.py FILE` prints a summary (tasks, worker threads, durations, the names that take the most time).

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
| 1 | slice | start | task number | end | how it ended: 0 finished, 1 yielded, 2 suspended | what the task is |
| 2 | created | time | task number | number of the parent task (0: none) | | what the task is |
| 3 | suspend | time | task number | | | the reason |
| 4 | resumed | time | task number | | | what woke it |
| 5 | fulfilled | time | id of the future | number of the task that was running (0: none) | | |
| 6 | continuation | time | id of the future whose value the continuation uses | task that was running | | |
| 7 | stolen | time | task number | victim worker | | |
| 8 | parcel_begin | time | most significant half of the parcel id | least significant half | | the action |
| 9 | parcel_end | time | same parcel | | | |
| 10 | parcel_sent | time | most significant half of the parcel id | least significant half | | |
| 11 | parcel_sender | time | least significant half of the parcel of the record before | number of the task that sends it (0: none) | | |
| 12 | future_wait | time | id of a future | number of the task that is about to block on it (0: none) | | |

The *parent* of a task is the task that was running when the task was handed to HPX (`async`, `dataflow`, ...). A task is staged first and made later by the scheduler, so the patch takes the number of the running task when the task is staged and carries it along (built with `HPX_WITH_THREAD_PARENT_REFERENCE`, which `HPX_WITH_TASK_TRACE_NAMES` switches on). For a `dataflow` this is the task that fulfilled the last input.

With `HPX_TASK_LOG_BACKGROUND=1` the background work of idle workers (the task `background_work`, which polls the network and handles parcels) is recorded too. The runs that follow each other without another task in between (pause up to about 100 us) are one *slice* per phase, otherwise there would be millions of records. Without the variable they are left out.

*parcel_begin* and *parcel_end* enclose the time in which the action of a received parcel is started on this worker thread: for a direct action (the collectives use them) it is run right there, for any other action only the task is scheduled. *parcel_sent* and *parcel_sender* tell which task on which worker thread sent a parcel. Worker 65535 is a thread that is no worker of HPX. *future_wait* and *fulfilled* with the same future id tell on which worker thread a value was set and which task was waiting for it; the converter joins them with the next slice of the task into a `wake` row. The id is the one of the parcel log (`%016x:%016x`, as in `run.csv`).

`worker` is the worker thread (for *stolen*: the thief). A task that suspends and resumes has one *slice* for every time it ran. A task can run another one inside (then the slices are nested). The **number of a task** is unique for the whole run: the index of the OS thread that created it in the upper bits, a counter of that thread below (HPX reuses its task objects, so their addresses are not unique). The id of a future is the address of its shared state: a *continuation* belongs to the latest earlier *fulfilled* with the same id, and shows which task handed a value to which.

Table of names: `EDUHPXN1`, `u32 count`, then for every name `u32 id, u16 length, bytes`. The name is the description of the task: for an action, for a function given a name with `hpx::annotated_function`, and for the tasks HPX starts itself (`hpx_main`, `pre_main`, ...) that name. For a plain function or a member function the address of the function, written as the name of the function if its file exports it, else `<file>+0x<offset in the file>`, which `addr2line -e <file> 0x<offset>` or `nm -C <file>` turns into a function (the file has to have its symbols). **A lambda or another function object has no name for HPX** (it only knows where the object is, not what it is): such a task is called `<unnamed task>`. Give it a name with `hpx::annotated_function(f, "name")` to see it. HPX keeps the description of the tasks only if it was built with `HPX_WITH_TASK_TRACE_NAMES=ON` (`install-hpx.sh --with-task-trace` does that).

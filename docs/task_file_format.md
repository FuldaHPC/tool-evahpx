# EduHPX task file, format 1

A text file, UTF-8, made by `scripts/eduhpx-tasks` from the task logs of HPX ([task_log_format.md](task_log_format.md)). It holds what the worker threads of one or more localities did. The first line must be `# eduhpx_tasks_csv 1`. The second line holds `key=value` pairs separated by blanks (`run`, `localities`, `tasks`, `slices`, `deps`, `steals`). The first line that is not a comment is the column header. Fields are separated by commas; a field with a comma or a quote is in double quotes (`""` is a quote), as in RFC 4180. Columns are found by name. All times are microseconds since the epoch, on the clock of the first node (nodes corrected as in the run file).

| column | meaning |
|---|---|
| `kind` | `slice`, `dep` or `steal` (below) |
| `loc` | locality |
| `worker` | worker thread of the locality |
| `task` | number of the task, unique for the whole run |
| `name` | what the task is: the name of an action or of a function given a name with `hpx::annotated_function`, a function name, or `<unnamed task>` for a lambda |
| `t0`, `t1` | start and end |
| `end` | how a slice ended: `finished`, `yielded` or `suspended` |
| `parent` | number of the task that created this task (empty: none or not known: the patched HPX takes the running task when a task is staged, see task_log_format.md) |
| `created` | when the task was created |
| `peer` | `dep`: the task that used the value; `steal`: the worker the task was taken from |

* **`slice`**: the task `task` ran on `worker` of `loc` from `t0` to `t1`. A task that suspends and resumes has one row for every time it ran. `name`, `parent` and `created` describe the task.
* **`send`** and **`recv`**: which worker thread sent a parcel (`task` is the sending task, `t0` the time), and which one ran its action (`name`, from `t0` to `t1`; a direct action also has a `slice` row). `peer` is the id of the parcel, as in `msg` of the run file. The viewer uses them to let the arrows of a message start and end in the row of the thread. Worker 65535 stands for a thread that is no worker of HPX.
* **`wake`**: a task (`task`) blocked on a future that was set on the worker thread `worker` at `t0`; the task went on at `t1` on the worker thread `peer`. This is the communication between worker threads inside a locality (the direct action of a collective sets the value on one thread, the task that waits for it goes on on another).
* **`dep`**: a future (`.then()`) that the task `task` fulfilled at `t0` was used by the task `peer` at `t1`. HPX only reports these for `.then()`-style continuations; for `dataflow` the link between the tasks is the `parent` of the task that `dataflow` starts (it is started by the task that fulfilled the last input).
* **`steal`**: `worker` took the task `task` from the queue of the worker `peer` at `t0`.

`eduhpx-tasks RUN_DIR` (or the task logs given one by one) writes it; `eduhpx-run --tasks` does that at the end of a run. The viewer opens it like a run file and shows one row per worker thread, see [the viewer](../gui/README.md).

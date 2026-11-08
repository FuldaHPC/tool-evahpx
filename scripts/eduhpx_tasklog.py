#!/usr/bin/env python3
"""eduhpx_tasklog.py: read the task log of one locality (<prefix>.tasks.loc<rank>.bin, written by HPX with HPX_TASK_LOG).

    eduhpx_tasklog.py FILE [FILE ...]        summary: tasks, worker threads, durations, how the slices end
    from eduhpx_tasklog import read          the records as Python objects, see read()

The format is described in docs/task_log_format.md. Standard library only."""
import collections
import struct
import sys

KINDS = {1: "slice", 2: "created", 3: "suspend", 4: "resumed", 5: "fulfilled", 6: "continuation", 7: "stolen",
         8: "parcel_begin", 9: "parcel_end", 10: "parcel_sent", 11: "parcel_sender", 12: "future_wait"}
ENDS = {0: "finished", 1: "yielded", 2: "suspended"}
_HEADER = struct.Struct("<8sIIiiQQQQQ")
_REC = struct.Struct("<BBHIQQQ")


class TaskLog:
    """One file: header values, the table of names and the records (tuples kind, flag, worker, name, t, a, b)."""

    def __init__(self, path, rank, pid, names, records, ticks0, wall0, ticks1, wall1, complete):
        self.path, self.rank, self.pid = path, rank, pid
        self.names, self.records, self.complete = names, records, complete
        self.ticks0, self.wall0, self.ticks1, self.wall1 = ticks0, wall0, ticks1, wall1
        span = ticks1 - ticks0
        self.ns_per_tick = (wall1 - wall0) / span if span > 0 else 1.0

    def wall_us(self, ticks):
        """Ticks of a record as wall clock time in microseconds since the epoch (the clock of the parcel log)."""
        return (self.wall0 + (ticks - self.ticks0) * self.ns_per_tick) / 1000.0

    def name(self, nid):
        return self.names.get(nid, "?")


def read(path):
    data = open(path, "rb").read()
    if len(data) < 64 or data[:8] != b"EDUHPXK1":
        raise ValueError("%s: not an EduHPX task log" % path)
    _magic, version, rec_size, rank, pid, names_at, t0, w0, t1, w1 = _HEADER.unpack_from(data, 0)
    if version != 1 or rec_size != _REC.size:
        raise ValueError("%s: unsupported task log version %d (record size %d)" % (path, version, rec_size))
    names = {}
    complete = names_at != 0 and data[names_at:names_at + 8] == b"EDUHPXN1"
    end = names_at if complete else len(data) - (len(data) - 64) % _REC.size    # a log without its names was not closed
    if complete:
        (count,) = struct.unpack_from("<I", data, names_at + 8)
        off = names_at + 12
        for _ in range(count):
            nid, ln = struct.unpack_from("<IH", data, off)
            off += 6
            names[nid] = data[off:off + ln].decode("utf-8", "replace")
            off += ln
    else:
        t1, w1 = t0 + 1, w0 + 1    # no calibration at the end: the clock rate is unknown
    records = list(_REC.iter_unpack(memoryview(data)[64:end]))
    return TaskLog(path, rank, pid, names, records, t0, w0, t1, w1, complete)


def summary(log, out=sys.stdout):
    kinds = collections.Counter(KINDS.get(r[0], "?") for r in log.records)
    print("# %s: locality %d, process %d, %d records %s%s" % (log.path, log.rank, log.pid, len(log.records), dict(kinds),
          "" if log.complete else "   (the file was not closed: no names, times not calibrated)"), file=out)
    slices = [r for r in log.records if r[0] == 1]
    if not slices:
        return
    scale = log.ns_per_tick
    dur = sorted((r[6] - r[4]) * scale for r in slices)
    tasks = {r[5] for r in slices}
    print("#   %d tasks ran in %d slices on worker threads %s" % (len(tasks), len(slices), sorted({r[2] for r in slices})), file=out)
    print("#   slice duration: median %.2f us, p90 %.2f us, p99 %.2f us, max %.2f us, total %.3f s" % (
        dur[len(dur) // 2] / 1e3, dur[int(len(dur) * .9)] / 1e3, dur[int(len(dur) * .99)] / 1e3, dur[-1] / 1e3, sum(dur) / 1e9), file=out)
    print("#   slices end: %s" % dict(collections.Counter(ENDS.get(r[1], "?") for r in slices)), file=out)
    by_name = collections.defaultdict(lambda: [0, 0.0])
    for r in slices:
        e = by_name[log.name(r[3])]
        e[0] += 1
        e[1] += (r[6] - r[4]) * scale
    print("#   by name (slices, total time):", file=out)
    for name, (n, total) in sorted(by_name.items(), key=lambda kv: -kv[1][1])[:12]:
        print("#     %-60s %9d  %10.3f ms" % (name[:60], n, total / 1e6), file=out)
    fulfilled = {r[5] for r in log.records if r[0] == 5}
    linked = sum(1 for r in log.records if r[0] == 6 and r[5] in fulfilled)
    nc = kinds.get("continuation", 0)
    if nc:
        print("#   continuations: %d, of them with the future fulfilled in this log: %d" % (nc, linked), file=out)


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0 if len(argv) > 1 else 1
    for path in argv[1:]:
        summary(read(path))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

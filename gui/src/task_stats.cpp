#include "task_file.h"

#include <QHash>
#include <QMap>

#include <algorithm>
#include <cmath>
#include <numeric>

// nameStats(), durationHistogram() and taskInfo(): the numbers behind the table of task names and the page of one task.
// Everything is read from the slices of the task file: a slice is one time a task ran on a worker thread; a task that ended a slice
// suspended or yielded and went on in a later slice (maybe on another thread), the time between is its pause.

namespace {

double quantile(QVector<double> &v, double q)
{
    if (v.isEmpty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(int(v.size()) - 1, int(q * v.size()))];
}

struct Piece
{
    double t0, t1;
    qint32 row;
    quint8 end;
};

} // namespace

// the numbers of the names in [a, b]: slices that start inside
struct TaskFile::NameData
{
    QVector<double> run, wait, pause;
    double total = 0;
    int tasks = 0, moved = 0;
};

void TaskFile::collectNames(double a, double b, QHash<qint32, NameData> &out) const
{
    struct First { double t0, created; qint32 name; };
    QHash<quint64, First> first;
    QHash<quint64, QVector<Piece>> pieces;               // only tasks that have more than one slice are needed later
    QHash<quint64, int> seen;
    for (int r = 0; r < m_rows.size(); ++r)
        for (const Slice &s : m_rows[r]) {
            if (s.t1 <= a || s.t0 >= b || skipped(s))
                continue;
            NameData &d = out[s.name];
            d.run.append(s.t1 - s.t0);
            d.total += s.t1 - s.t0;
            auto f = first.find(s.task);
            if (f == first.end()) {
                first.insert(s.task, {s.t0, s.created, s.name});
                ++d.tasks;
            } else if (s.t0 < f->t0) {
                *f = {s.t0, s.created, s.name};
            }
            pieces[s.task].append({s.t0, s.t1, qint32(r), s.end});
        }
    for (auto f = first.constBegin(); f != first.constEnd(); ++f)
        if (f->created >= 0)
            out[f->name].wait.append(std::max(0.0, f->t0 - f->created));
    for (auto p = pieces.begin(); p != pieces.end(); ++p) {
        if (p->size() < 2)
            continue;
        std::sort(p->begin(), p->end(), [](const Piece &x, const Piece &y) { return x.t0 < y.t0; });
        const qint32 name = first.value(p.key()).name;
        NameData &d = out[name];
        for (int k = 1; k < p->size(); ++k) {
            const Piece &prev = p->at(k - 1), &next = p->at(k);
            if (prev.end == 0 || next.t0 <= prev.t1)        // nested or finished: no pause
                continue;
            d.pause.append(next.t0 - prev.t1);
            d.moved += next.row != prev.row;
        }
    }
}

QVariantList TaskFile::nameStats(double a, double b) const
{
    QVariantList out;
    QHash<qint32, NameData> data;
    collectNames(a, b, data);
    QList<QPair<double, qint32>> order;
    for (auto d = data.constBegin(); d != data.constEnd(); ++d)
        order.append({d->total, d.key()});
    std::sort(order.begin(), order.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
    for (const auto &o : std::as_const(order)) {
        if (out.size() >= 200)
            break;
        NameData &d = data[o.second];
        QVariantMap m;
        m["index"] = o.second;
        m["name"] = m_names.value(o.second);
        m["count"] = int(d.run.size());
        m["tasks"] = d.tasks;
        m["total"] = d.total;
        m["mean"] = d.run.isEmpty() ? 0.0 : d.total / d.run.size();
        m["p50"] = quantile(d.run, 0.5);
        m["p90"] = quantile(d.run, 0.9);
        m["max"] = d.run.isEmpty() ? 0.0 : d.run.last();
        m["waitP50"] = quantile(d.wait, 0.5);
        m["waitP90"] = quantile(d.wait, 0.9);
        m["pauses"] = int(d.pause.size());
        m["pauseTotal"] = std::accumulate(d.pause.begin(), d.pause.end(), 0.0);
        m["pauseP50"] = quantile(d.pause, 0.5);
        m["pauseMax"] = d.pause.isEmpty() ? 0.0 : d.pause.last();
        m["moved"] = d.moved;
        out.append(m);
    }
    return out;
}

// counts in logarithmic bins, four per decade from 1 us: bin k holds 10^(k/4) us <= x < 10^((k+1)/4) us, 33 bins up to 100 s
QVector<double> TaskFile::durationHistogram(int name, double a, double b, const QString &what) const
{
    QVector<double> counts(33, 0.0);
    QHash<qint32, NameData> data;
    collectNames(a, b, data);
    const auto it = data.constFind(name);
    if (it == data.constEnd())
        return counts;
    const QVector<double> &v = what == QLatin1String("wait") ? it->wait : what == QLatin1String("pause") ? it->pause : it->run;
    for (double x : v)
        counts[std::clamp(int(std::floor(4 * std::log10(std::max(x, 1.0)))), 0, 32)] += 1;
    return counts;
}

// one task: its slices, what happened between them and around it
QVariantMap TaskFile::taskInfo(const QString &taskText) const
{
    QVariantMap out;
    const quint64 task = taskText.toULongLong();
    if (!task)
        return out;
    QVector<Piece> ps;
    double created = -1;
    qint32 name = -1;
    quint64 parent = 0;
    for (int r = 0; r < m_rows.size(); ++r)
        for (const Slice &s : m_rows[r])
            if (s.task == task) {
                ps.append({s.t0, s.t1, qint32(r), s.end});
                if (name < 0 || s.t0 < ps.first().t0) {
                    name = s.name;
                    created = s.created;
                    parent = s.parent;
                }
            }
    if (ps.isEmpty())
        return out;
    std::sort(ps.begin(), ps.end(), [](const Piece &x, const Piece &y) { return x.t0 < y.t0; });
    double run = 0;
    for (const Piece &p : std::as_const(ps))
        run += p.t1 - p.t0;
    QVariantList pauses;
    double pauseTotal = 0;
    int moved = 0;
    for (int k = 1; k < ps.size(); ++k) {
        const Piece &prev = ps[k - 1], &next = ps[k];
        if (prev.end == 0 || next.t0 <= prev.t1)
            continue;
        QVariantMap m;
        m["from"] = prev.t1;
        m["to"] = next.t0;
        m["dur"] = next.t0 - prev.t1;
        m["rowFrom"] = prev.row;
        m["rowTo"] = next.row;
        m["why"] = prev.end == 1 ? QStringLiteral("yielded") : QStringLiteral("suspended");
        const QVariantMap w = wakeInto(next.row, next.t0);
        if (!w.isEmpty())
            m["wake"] = w;
        pauseTotal += next.t0 - prev.t1;
        moved += next.row != prev.row;
        if (pauses.size() < 100)
            pauses.append(m);
    }
    out["task"] = taskText;
    out["name"] = m_names.value(name);
    out["nameIndex"] = name;
    out["created"] = created;
    out["start"] = ps.first().t0;
    out["end"] = ps.last().t1;
    out["wait"] = created >= 0 ? std::max(0.0, ps.first().t0 - created) : -1.0;
    out["run"] = run;
    out["slices"] = int(ps.size());
    out["pauses"] = pauses;
    out["pauseTotal"] = pauseTotal;
    out["moved"] = moved;
    out["row"] = ps.first().row;
    out["parent"] = parent ? QString::number(parent) : QString();
    out["parentName"] = parent ? nameOfTask(QString::number(parent)) : QString();
    out["childCount"] = relatives(taskText).value(QStringLiteral("childCount"));
    return out;
}

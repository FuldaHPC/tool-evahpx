#include "task_file.h"

#include <QHash>
#include <QMap>
#include <QPair>
#include <QSet>

#include <algorithm>
#include <cmath>

// findings(): what the task data say about where the worker threads had nothing to do, and why (see task_file.h).
// Everything is computed from the slices of the task file: when a task was made (created), when it first ran, how many tasks
// ran at a time. No claim goes beyond that: the text says what was measured and what it usually means.

namespace {

QString dur(double us)
{
    if (us >= 1e6)
        return QString::number(us / 1e6, 'f', 2) + QStringLiteral(" s");
    if (us >= 1e3)
        return QString::number(us / 1e3, 'f', us >= 1e5 ? 0 : 1) + QStringLiteral(" ms");
    return QString::number(us, 'f', 0) + QStringLiteral(" us");
}

QString pct(double f) { return QString::number(std::lround(100 * f)) + QStringLiteral(" %"); }

struct Work
{
    double t0, t1;
    quint64 task, parent;
    double created;
    qint32 name, row;
};

// the work of one locality: its slices (the program's tasks, not the tasks of HPX and not the polling of idle workers) and the
// counts that follow from them
struct Loc
{
    int workers = 0;
    QVector<Work> ws;
    QVector<double> starts, ends;       // sorted: how many tasks run at time t
    QVector<double> made, begun;        // sorted: how many tasks are ready at time t (made, not yet started)
    bool readyKnown = false;

    int running(double t) const
    {
        const int n = int(std::upper_bound(starts.begin(), starts.end(), t) - starts.begin())
            - int(std::upper_bound(ends.begin(), ends.end(), t) - ends.begin());
        return std::clamp(n, 0, workers);
    }
    int ready(double t) const
    {
        return std::max(0, int(std::upper_bound(made.begin(), made.end(), t) - made.begin())
                               - int(std::upper_bound(begun.begin(), begun.end(), t) - begun.begin()));
    }
};

struct Sample
{
    double ready = 0, idle = 0, both = 0;       // mean ready tasks, mean idle threads, share of the time with both
};

Sample sample(const Loc &l, double a, double b)
{
    Sample s;
    const int n = 300;
    for (int i = 0; i < n; ++i) {
        const double t = a + (b - a) * (i + 0.5) / n;
        const int r = l.ready(t), idle = l.workers - l.running(t);
        s.ready += r;
        s.idle += idle;
        s.both += (r > 0 && idle > 0) ? 1 : 0;
    }
    s.ready /= n;
    s.idle /= n;
    s.both /= n;
    return s;
}

} // namespace

QVariantList TaskFile::findings() const { return findingsIn(m_tmin, m_tmax); }

QVariantList TaskFile::findingsIn(double a0, double b0) const
{
    QVariantList out;
    if (!m_loaded || b0 <= a0)
        return out;
    auto work = [&](const Slice &s) {
        return s.name >= 0 && s.name < m_nameInternal.size() && !m_nameInternal[s.name] && !m_nameBg.value(s.name);
    };
    auto unnamed = [&](int name) { return m_names.value(name).startsWith(QLatin1String("<unnamed")); };

    // the work of every locality
    QMap<int, Loc> locs;
    QHash<quint64, qint32> nameOf;
    for (int r = 0; r < m_rows.size() && r < m_rowLoc.size(); ++r) {
        Loc &l = locs[m_rowLoc[r]];
        ++l.workers;
        for (const Slice &s : m_rows[r])
            if (work(s) && s.t1 > a0 && s.t0 < b0) {
                l.ws.append({s.t0, s.t1, s.task, s.parent, s.created, s.name, qint32(r)});
                if (!nameOf.contains(s.task))
                    nameOf.insert(s.task, s.name);
            }
    }
    for (auto it = locs.begin(); it != locs.end();) {
        if (it->ws.isEmpty() || it->workers == 0) {
            it = locs.erase(it);
            continue;
        }
        Loc &l = *it;
        QHash<quint64, QPair<double, double>> first;          // task -> first start, created
        for (const Work &w : std::as_const(l.ws)) {
            l.starts.append(w.t0);
            l.ends.append(w.t1);
            auto f = first.find(w.task);
            if (f == first.end())
                first.insert(w.task, {w.t0, w.created});
            else if (w.t0 < f->first)
                *f = {w.t0, w.created};
        }
        std::sort(l.starts.begin(), l.starts.end());
        std::sort(l.ends.begin(), l.ends.end());
        for (const auto &f : std::as_const(first))
            if (f.second >= 0) {
                l.made.append(f.second);
                l.begun.append(std::max(f.first, f.second));
            }
        l.readyKnown = l.made.size() * 2 > first.size();
        std::sort(l.made.begin(), l.made.end());
        std::sort(l.begun.begin(), l.begun.end());
        ++it;
    }
    if (locs.isEmpty())
        return out;
    const bool many = locs.size() > 1;
    const double span = b0 - a0;

    struct Finding
    {
        QString kind, level, title, brief, text;
        double a, b, weight;
        int loc = -1;
    };
    QVector<Finding> found;

    for (auto li = locs.constBegin(); li != locs.constEnd(); ++li) {
        const int loc = li.key();
        const Loc &l = li.value();
        auto rel = [&](double t) { return QStringLiteral("+") + dur(t - m_tmin); };

        // how busy the threads were, in equal parts of the run (tasks that run inside another one count once)
        const int bins = 160;
        const double width = span / bins;
        QVector<double> busy(bins, 0.0);
        for (int r = 0; r < m_rows.size() && r < m_rowLoc.size(); ++r) {
            if (m_rowLoc[r] != loc)
                continue;
            double covered = a0;
            for (const Slice &s : m_rows[r]) {
                if (!work(s))
                    continue;
                const double t0 = std::max({s.t0, covered, a0}), t1 = std::min(s.t1, b0);
                covered = std::max(covered, s.t1);
                if (t1 <= t0)
                    continue;
                int k = std::clamp(int((t0 - a0) / width), 0, bins - 1);
                for (double t = t0; t < t1 && k < bins; ++k) {
                    const double e = std::min(t1, a0 + (k + 1) * width);
                    busy[k] += e - t;
                    t = e;
                }
            }
        }
        QVector<double> util(bins);
        for (int k = 0; k < bins; ++k)
            util[k] = std::min(1.0, busy[k] / (width * l.workers));

        // parts of the run with most threads idle: from a bin below 70 % to the next one above it
        const double low = 0.7;
        for (int i = 0; i < bins;) {
            if (util[i] >= low) {
                ++i;
                continue;
            }
            int j = i;
            while (j < bins && util[j] < low)
                ++j;
            const int len = j - i;
            const double a = a0 + i * width, b = a0 + j * width;
            double mean = 0;
            for (int k = i; k < j; ++k)
                mean += util[k];
            mean /= len;
            const double idleShare = (1 - mean) * (b - a) / span;         // of all the thread time of the run
            i = j;
            if (len < 3 || idleShare < 0.02)
                continue;

            QStringList lines;
            QString brief = QStringLiteral("Threads idle");
            lines << QStringLiteral("On average %1 of the %2 worker threads were busy for %3. This part holds %4 of all the thread time of the run that nobody used.")
                         .arg(pct(mean)).arg(l.workers).arg(dur(b - a)).arg(pct(idleShare));
            // what ran there
            QHash<qint32, double> byName;
            double all = 0;
            for (const Work &w : l.ws) {
                const double o = std::min(w.t1, b) - std::max(w.t0, a);
                if (o > 0) {
                    byName[w.name] += o;
                    all += o;
                }
            }
            QList<QPair<double, qint32>> top;
            for (auto n = byName.constBegin(); n != byName.constEnd(); ++n)
                top.append({n.value(), n.key()});
            std::sort(top.begin(), top.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
            QStringList names;
            for (int k = 0; k < top.size() && k < 2; ++k)
                names << QStringLiteral("%1 (%2)").arg(m_names.value(top[k].second), pct(top[k].first / std::max(all, 1.0)));
            if (!names.isEmpty())
                lines << QStringLiteral("Running: ") + names.join(QStringLiteral(", ")) + QStringLiteral(".");
            if (!top.isEmpty() && unnamed(top[0].second) && top[0].first > 0.5 * all)
                lines << QStringLiteral("These tasks have no name. Give them one in the program (hpx::annotated_function) to see which part of the code this is.");
            if (!top.isEmpty()) {                 // who made the tasks that ran most
                QHash<QString, int> by;
                int n = 0;
                QSet<quint64> seen;
                for (const Work &w : l.ws)
                    if (w.name == top[0].second && w.t0 >= a && w.t0 < b && w.parent != 0 && !seen.contains(w.task)) {
                        seen.insert(w.task);
                        ++n;
                        by[m_names.value(nameOf.value(w.parent, -1), QStringLiteral("?"))]++;
                    }
                QList<QPair<int, QString>> mk;
                for (auto k = by.constBegin(); k != by.constEnd(); ++k)
                    mk.append({k.value(), k.key()});
                std::sort(mk.begin(), mk.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
                QStringList parts;
                for (int k = 0; k < mk.size() && k < 2; ++k)
                    parts << QStringLiteral("%1 (%2)").arg(mk[k].second).arg(mk[k].first);
                if (n >= 20 && !parts.isEmpty())
                    lines << QStringLiteral("Made by: ") + parts.join(QStringLiteral(", ")) + QStringLiteral(".");
            }
            // tasks that wait while threads are idle, or no tasks to run?
            if (l.readyKnown) {
                const Sample s = sample(l, a, b);
                const QString counts = QStringLiteral("On average %1 tasks were ready and %2 threads idle").arg(s.ready, 0, 'f', 1).arg(s.idle, 0, 'f', 1);
                if (s.both < 0.15) {
                    brief = QStringLiteral("Not enough work at the same time");
                    lines << counts + QStringLiteral(": hardly ever was a task waiting while a thread was idle. There is not enough work at the same time. "
                                                    "Tasks are made only when earlier ones end, so look at what the program waits for here (a barrier or get() after every step, a sequential part, too few tasks).");
                } else if (s.both >= 0.4) {
                    brief = QStringLiteral("Threads idle although tasks were ready");
                    lines << counts + QStringLiteral(", and in %1 of the time there were both. Threads were idle although tasks were ready: starting a task costs about as much as running it (tasks too small?) or stealing work is slow.")
                                           .arg(pct(s.both));
                } else {
                    brief = QStringLiteral("Sometimes tasks waited although threads were idle");
                    lines << counts + QStringLiteral(". In %1 of the time a task waited although a thread was idle, otherwise there was no work for it.").arg(pct(s.both));
                }
            }
            // waves: all threads (nearly) idle again and again, with a group of tasks in between: a barrier after every step
            {
                const int limit = std::max(2, l.workers / 16);
                struct Ev { double t; int d; };
                QVector<Ev> ev;
                for (const Work &w : l.ws) {
                    if (w.t1 < a || w.t0 > b)
                        continue;
                    ev.append({w.t0, 1});
                    ev.append({w.t1, -1});
                }
                std::sort(ev.begin(), ev.end(), [](const Ev &x, const Ev &y) { return x.t < y.t || (x.t == y.t && x.d > y.d); });
                int n = 0;
                double since = -1;
                bool started = false;
                QVector<QPair<double, double>> gaps;
                for (const Ev &e : std::as_const(ev)) {
                    if (!started && e.t >= a) {            // the slices that began before a are counted now
                        started = true;
                        if (n <= limit)
                            since = a;
                    }
                    n += e.d;
                    if (e.t < a)
                        continue;
                    if (e.t > b)
                        break;
                    if (n <= limit && since < 0)
                        since = e.t;
                    if (n > limit && since >= 0) {
                        if (e.t - since >= 300)
                            gaps.append({since, e.t});
                        since = -1;
                    }
                }
                if (gaps.size() >= 8) {
                    QVector<int> sizes;
                    double gapSum = 0;
                    for (int k = 0; k + 1 < gaps.size(); ++k) {
                        gapSum += gaps[k].second - gaps[k].first;
                        const int c = int(std::lower_bound(l.starts.begin(), l.starts.end(), gaps[k + 1].first) - l.starts.begin())
                            - int(std::lower_bound(l.starts.begin(), l.starts.end(), gaps[k].second) - l.starts.begin());
                        sizes.append(c);
                    }
                    auto med3 = [&](int from) {
                        QVector<int> v{sizes[from], sizes[from + 1], sizes[from + 2]};
                        std::sort(v.begin(), v.end());
                        return v[1];
                    };
                    if (sizes.size() >= 4 && gapSum >= 0.05 * (b - a)) {
                        const int firstN = med3(0), lastN = med3(sizes.size() - 3);
                        const QString trend = lastN * 10 < firstN * 6 ? QStringLiteral(", the groups get smaller (%1 tasks at the start, %2 at the end)").arg(firstN).arg(lastN)
                            : firstN * 6 < lastN * 10 ? QStringLiteral(", the groups get bigger (%1 tasks at the start, %2 at the end)").arg(firstN).arg(lastN)
                                                      : QString();
                        lines << QStringLiteral("%1 waves: groups of tasks, between them (nearly) all threads were idle for %2 in all%3. Typical for a barrier or a wait after every step; the next step starts only when the last task of the one before is done.")
                                     .arg(sizes.size()).arg(dur(gapSum)).arg(trend);
                        brief += QStringLiteral(", in waves (a barrier?)");
                    }
                }
            }
            found.append({QStringLiteral("window"), QStringLiteral("warn"),
                          QStringLiteral("Threads mostly idle, %1 to %2 (%3 busy)").arg(rel(a), rel(b), pct(mean)), brief,
                          lines.join(QLatin1Char('\n')), a, b, idleShare * 2, loc});
        }

        // very many tiny tasks, while threads are idle and tasks are ready: the work of starting them is as big as the work itself
        {
            QHash<qint32, QVector<double>> tiny;
            for (const Work &w : l.ws)
                if (w.t1 - w.t0 < 50)
                    tiny[w.name].append(w.t0);
            for (auto t = tiny.constBegin(); t != tiny.constEnd(); ++t) {
                if (t->size() < 1000 || !l.readyKnown)
                    continue;
                // where they are: the longest stretch of the run in which they come in large numbers
                QVector<int> per(bins, 0);
                int most = 0;
                for (double x : *t)
                    most = std::max(most, ++per[std::clamp(int((x - a0) / width), 0, bins - 1)]);
                const int level = std::max(10, int(0.15 * most));
                int bestFrom = -1, bestTo = -1, bestSum = 0;
                for (int i = 0; i < bins;) {
                    if (per[i] < level) {
                        ++i;
                        continue;
                    }
                    int last = i, sum = 0;                  // up to two bins below the level do not end the stretch
                    for (int k = i; k < bins && k - last <= 2; ++k)
                        if (per[k] >= level)
                            last = k;
                    for (int k = i; k <= last; ++k)
                        sum += per[k];
                    if (sum > bestSum) {
                        bestSum = sum;
                        bestFrom = i;
                        bestTo = last + 1;
                    }
                    i = last + 1;
                }
                if (bestSum < 1000)
                    continue;
                const double a = a0 + bestFrom * width, b = a0 + bestTo * width;
                const Sample s = sample(l, a, b);
                if (s.both < 0.3 || s.ready < 1)
                    continue;
                found.append({QStringLiteral("tiny"), QStringLiteral("warn"),
                              QStringLiteral("%1 tiny tasks (%2), %3 to %4").arg(bestSum).arg(m_names.value(t.key()), rel(a), rel(b)),
                              QStringLiteral("Tasks too small: starting them costs as much as running them"),
                              QStringLiteral("%1 tasks of this name ran shorter than 50 us in this part. On average %2 tasks were ready while %3 threads were idle (in %4 of the time both).\n"
                                             "A task this small costs about as much to make and start as it computes. Join them into bigger chunks (a grain size for the loop, one task for several elements).")
                                  .arg(bestSum).arg(s.ready, 0, 'f', 1).arg(s.idle, 0, 'f', 1).arg(pct(s.both)),
                              a, b, 0.5 * double(bestSum) / 10000.0, loc});
            }
        }
    }

    // the chain that decided the run time: how long does it wait, and what is going on while it does?
    const QVariantMap chain = chainIn(a0, b0);
    if (chain.value(QStringLiteral("count")).toInt() > 0) {
        const QVector<double> sl = chain.value(QStringLiteral("slices")).value<QVector<double>>();
        const double spanUs = chain.value(QStringLiteral("spanUs")).toDouble(), runUs = chain.value(QStringLiteral("runUs")).toDouble();
        const int row0 = sl.isEmpty() ? 0 : int(sl[0]);
        const int loc = row0 >= 0 && row0 < m_rowLoc.size() ? m_rowLoc[row0] : 0;
        const auto li = locs.constFind(loc);
        if (spanUs > 0 && li != locs.constEnd()) {
            const Loc &l = li.value();
            const double ran = runUs / spanUs;
            QString text = QStringLiteral("The chain of tasks that decided the run time has %1 tasks, each made by the one before. From its first to its last task %2 pass; its tasks run %3 of that (%4).")
                               .arg(chain.value(QStringLiteral("count")).toInt()).arg(dur(spanUs)).arg(dur(runUs)).arg(pct(ran));
            QString level = QStringLiteral("info");
            QString brief = QStringLiteral("The chain waits for %1 of its span").arg(pct(1 - ran));
            if (ran >= 0.7) {
                brief = QStringLiteral("The run is limited by the chain itself");
                text += QStringLiteral("\nThe run is limited by this chain: it is busy most of the time, more threads would not make the run shorter. Only shorter tasks on the chain help.");
            } else {
                // between two tasks of the chain: until the next one was made (its inputs came, or the program made it) and from then on until
                // it began (it stood in the queue; all threads busy: other work first; threads idle: the scheduler was slow)
                const QVector<double> links = chain.value(QStringLiteral("links")).value<QVector<double>>();
                double before = 0, queue = 0, busyQ = 0, idleQ = 0;
                for (int k = 0; k + 3 < links.size(); k += 4) {
                    const double end = links[k], made = links[k + 1], start = links[k + 2];
                    if (made < 0 || start <= end)
                        continue;
                    const double m = std::clamp(made, end, start);
                    before += m - end;
                    queue += start - m;
                    if (start > m) {
                        if (l.workers - l.running((m + start) / 2) == 0)
                            busyQ += start - m;
                        else
                            idleQ += start - m;
                    }
                }
                const double wait = before + queue;
                if (wait > 0) {
                    text += QStringLiteral("\nWhile the chain waited (%1): %2 of that until the next task of the chain was made (%3), %4 from then on until a thread began it (%5).")
                                .arg(dur(wait), pct(before / wait), dur(before), pct(queue / wait), dur(queue));
                    if (before / wait >= 0.5) {
                        text += QStringLiteral("\nMost of the waiting is before the next task even exists: it was made long after the task before ended. Its other inputs came later, or the program makes it late "
                                               "(look at what makes the tasks of the chain: a loop that makes them one after the other, or a get() before the next step).");
                        brief = QStringLiteral("The chain waits %1 for the next task to be made, only %2 in the queue").arg(pct(before / wait), pct(queue / wait));
                        level = QStringLiteral("warn");
                    }
                    if (queue > 0)
                        text += QStringLiteral("\nIn the queue: in %1 of that time all threads were busy with other tasks (a higher priority for the tasks of the chain, for Cholesky potrf and trsm, would let them go on earlier), "
                                               "in %2 threads were idle although the task was ready (the scheduler needs long to hand it out).")
                                    .arg(pct(busyQ / queue), pct(idleQ / queue));
                    if (before / wait < 0.5 && queue / wait >= 0.3 && busyQ / queue >= 0.5) {
                        brief = QStringLiteral("The chain mostly stands in the queue behind other work: a higher priority would help");
                        level = QStringLiteral("warn");
                    } else if (before / wait < 0.5 && queue / wait >= 0.3 && idleQ / queue >= 0.3) {
                        brief = QStringLiteral("Threads idle although the chain had a ready task");
                        level = QStringLiteral("warn");
                    }
                }
            }
            found.append({QStringLiteral("chain"), level,
                          QStringLiteral("Critical chain: %1 of %2 busy").arg(dur(runUs), dur(spanUs)), brief, text,
                          chain.value(QStringLiteral("start")).toDouble(), chain.value(QStringLiteral("end")).toDouble(), 0.01, -1});
        }
    }

    // the same thing on several localities (a broadcast: everybody waits for the same data) is one finding
    struct Group
    {
        Finding f;
        QVector<int> locs;
    };
    QVector<Group> groups;
    for (const Finding &f : std::as_const(found)) {
        bool merged = false;
        if (f.loc >= 0)
            for (Group &g : groups)
                if (g.f.kind == f.kind && g.f.brief == f.brief && std::abs(g.f.a - f.a) <= 0.1 * (g.f.b - g.f.a) + 1 && std::abs(g.f.b - f.b) <= 0.1 * (g.f.b - g.f.a) + 1) {
                    g.locs.append(f.loc);
                    g.f.weight = std::max(g.f.weight, f.weight);
                    merged = true;
                    break;
                }
        if (!merged)
            groups.append({f, f.loc >= 0 ? QVector<int>{f.loc} : QVector<int>()});
    }
    std::stable_sort(groups.begin(), groups.end(), [](const Group &x, const Group &y) { return x.f.weight > y.f.weight; });
    for (int k = 0; k < groups.size() && k < 8; ++k) {
        const Group &g = groups[k];
        QString title = g.f.title, text = g.f.text;
        if (g.locs.size() > 1) {
            title = QStringLiteral("%1 localities: ").arg(g.locs.size()) + title;
            text = QStringLiteral("The same on %1 localities. The numbers are those of the first one.\n").arg(g.locs.size()) + text;
        } else if (many && g.f.loc >= 0) {
            title = QStringLiteral("L%1: ").arg(g.f.loc) + title;
        }
        out.append(QVariantMap{{"kind", g.f.kind}, {"level", g.f.level}, {"title", title}, {"brief", g.f.brief}, {"text", text},
                               {"from", g.f.a}, {"to", g.f.b}});
    }
    return out;
}

// The stretches of the run in which most of the work is done by named tasks, and those in which it is done by tasks without a name.
// A program often measures only a part of what it does (the named part, because that is what the author annotated); the findings
// of the other part are of no use. The driver hpx_main does not count: it only waits.
QVariantList TaskFile::phases() const
{
    QVariantList out;
    if (!m_loaded || m_tmax <= m_tmin)
        return out;
    const int bins = 160;
    const double width = (m_tmax - m_tmin) / bins;
    int workers = 0;
    QVector<double> named(bins, 0.0), anon(bins, 0.0);
    QHash<qint32, double> byName;
    auto isAnon = [&](int name) { return m_names.value(name).startsWith(QLatin1String("<unnamed")); };
    auto isDriver = [&](int name) { return m_names.value(name).startsWith(QLatin1String("hpx_main")); };
    for (int r = 0; r < m_rows.size(); ++r) {
        ++workers;
        for (const Slice &s : m_rows[r]) {
            if (s.name < 0 || s.name >= m_nameInternal.size() || m_nameInternal[s.name] || m_nameBg.value(s.name) || isDriver(s.name))
                continue;
            QVector<double> &to = isAnon(s.name) ? anon : named;
            for (int k = std::clamp(int((s.t0 - m_tmin) / width), 0, bins - 1); k < bins && m_tmin + k * width < s.t1; ++k)
                to[k] += std::max(0.0, std::min(s.t1, m_tmin + (k + 1) * width) - std::max(s.t0, m_tmin + k * width));
        }
    }
    if (workers == 0)
        return out;
    // the kind of every bin: 1 named, 2 without a name, 0 hardly anything ran (it goes on as before)
    QVector<int> kind(bins, 0);
    for (int k = 0; k < bins; ++k)
        if (named[k] + anon[k] >= 0.02 * width * workers)
            kind[k] = anon[k] > named[k] ? 2 : 1;
    for (int k = 1; k < bins; ++k)
        if (kind[k] == 0)
            kind[k] = kind[k - 1];
    for (int k = bins - 2; k >= 0; --k)
        if (kind[k] == 0)
            kind[k] = kind[k + 1];
    if (kind[0] == 0)
        return out;
    struct Stretch { int from, to, kind; };
    QVector<Stretch> st;
    for (int k = 0; k < bins; ++k) {
        if (!st.isEmpty() && st.last().kind == kind[k])
            st.last().to = k + 1;
        else
            st.append({k, k + 1, kind[k]});
    }
    for (int k = 0; k < st.size();) {                // a stretch shorter than three bins belongs to the one before
        if (st.size() > 1 && st[k].to - st[k].from < 3) {
            if (k > 0) {
                st[k - 1].to = st[k].to;
                st.removeAt(k);
                if (k < st.size() && st[k].kind == st[k - 1].kind) {
                    st[k - 1].to = st[k].to;
                    st.removeAt(k);
                }
            } else {
                st[1].from = st[0].from;
                st.removeAt(0);
            }
            continue;
        }
        ++k;
    }
    if (st.size() < 2)
        return out;
    // the edges to the time of the first task of the new kind near the edge
    QVector<double> edge(st.size() + 1);
    edge[0] = m_tmin;
    edge[st.size()] = m_tmax;
    for (int k = 1; k < st.size(); ++k) {
        const double e = m_tmin + st[k].from * width;
        double best = e;
        for (const QVector<Slice> &v : m_rows)
            for (const Slice &s : v) {
                if (s.t0 < e - width || s.t0 > e + width || s.name < 0 || s.name >= m_nameInternal.size() || m_nameInternal[s.name] || isDriver(s.name))
                    continue;
                if ((isAnon(s.name) ? 2 : 1) == st[k].kind && s.t0 < best)
                    best = s.t0;
            }
        edge[k] = best;
    }
    for (int k = 0; k < st.size(); ++k) {
        double busy = 0;
        byName.clear();
        for (int b = st[k].from; b < st[k].to; ++b)
            busy += named[b] + anon[b];
        for (const QVector<Slice> &v : m_rows)
            for (const Slice &s : v)
                if (s.t0 >= edge[k] && s.t0 < edge[k + 1] && s.name >= 0 && s.name < m_nameInternal.size() && !m_nameInternal[s.name] && !m_nameBg.value(s.name) && !isDriver(s.name))
                    byName[s.name] += s.t1 - s.t0;
        QString top;
        double most = 0;
        for (auto n = byName.constBegin(); n != byName.constEnd(); ++n)
            if (n.value() > most) {
                most = n.value();
                top = m_names.value(n.key());
            }
        out.append(QVariantMap{{"from", edge[k]}, {"to", edge[k + 1]}, {"named", st[k].kind == 1},
                               {"util", std::min(1.0, busy / (width * (st[k].to - st[k].from) * workers))}, {"name", top}});
    }
    return out;
}

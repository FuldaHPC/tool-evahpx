#include "task_file.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QList>
#include <QPair>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>

namespace {

// Splits a CSV line in place: the fields end with NUL, a field in quotes is unquoted ("" is a quote).
// Returns the number of fields, -1 if a quote is not closed.
int splitInPlace(char *s, char **fields, int maxFields)
{
    int n = 0;
    while (n < maxFields) {
        if (*s == '"') {
            char *out = ++s;
            fields[n++] = out;
            for (;;) {
                if (*s == '"') {
                    if (s[1] == '"') {
                        *out++ = '"';
                        s += 2;
                        continue;
                    }
                    ++s;
                    break;
                }
                if (*s == 0)
                    return -1;
                *out++ = *s++;
            }
            *out = 0;
            if (*s == ',') {
                ++s;
                continue;
            }
            return n;
        }
        fields[n++] = s;
        while (*s && *s != ',')
            ++s;
        if (*s == ',') {
            *s++ = 0;
            continue;
        }
        return n;
    }
    return n;
}

constexpr int kMaxFields = 16;

} // namespace

TaskFile::TaskFile(QObject *parent) : QObject(parent) {}

bool TaskFile::isTaskFile(const QUrl &url) const
{
    QString path = url.scheme() == QLatin1String("qrc") ? QLatin1Char(':') + url.path()
                                                          : (url.isLocalFile() ? url.toLocalFile() : url.toString());
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    return f.readLine(64).startsWith("# eduhpx_tasks_csv");
}

QUrl TaskFile::companion(const QUrl &url) const
{
    if (!url.isLocalFile())
        return QUrl();
    const QFileInfo info(url.toLocalFile());
    const QDir dir = info.dir();
    const QString base = info.completeBaseName();      // "run", "foo", "foo.tasks"
    const bool isTasks = isTaskFile(url);
    QStringList candidates;
    if (isTasks) {
        if (base.endsWith(QLatin1String(".tasks")))
            candidates << base.left(base.size() - 6) + QLatin1String(".csv");
        candidates << QStringLiteral("run.csv");
    } else {
        candidates << base + QLatin1String(".tasks.csv") << QStringLiteral("tasks.csv");
    }
    for (const QString &name : std::as_const(candidates)) {
        const QString path = dir.filePath(name);
        if (path == info.absoluteFilePath() || !QFile::exists(path))
            continue;
        if (isTaskFile(QUrl::fromLocalFile(path)) != isTasks)
            return QUrl::fromLocalFile(path);
    }
    return QUrl();
}

void TaskFile::fail(const QString &message)
{
    m_loaded = false;
    m_error = message;
    m_rows.clear();
    m_deps.clear();
    emit changed();
}

void TaskFile::unload()
{
    m_loaded = false;
    m_error.clear();
    m_name.clear();
    m_rows.clear();
    m_deps.clear();
    m_rowLabels.clear();
    m_rowLoc.clear();
    m_parcels.clear();
    m_wakes.clear();
    m_rowIndex.clear();
    m_rowWorker.clear();
    m_hands.clear();
    m_handsBuilt = false;
    m_internalSlices = 0;
    m_nameInternal.clear();
    m_names.clear();
    m_nameCount.clear();
    m_nameTotal.clear();
    m_taskCount = m_sliceCount = m_localities = 0;
    emit changed();
}

bool TaskFile::load(const QUrl &url)
{
    const QString path = url.scheme() == QLatin1String("qrc") ? QLatin1Char(':') + url.path()
                                                                : (url.isLocalFile() ? url.toLocalFile() : url.toString());
    const QString shown = QFileInfo(path).fileName();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(tr("Cannot open %1").arg(shown)), false;

    unload();
    std::map<quint32, QVector<Slice>> rows;      // (locality << 16 | worker) -> slices
    QHash<QByteArray, int> nameIndex;
    QSet<quint64> tasks;
    struct RawDep { quint64 from, to; double t0, t1; };
    QVector<RawDep> rawDeps;
    QString runName;
    int col[11];                                  // kind loc worker task name t0 t1 end parent created peer
    static const char *const colNames[11] = {"kind", "loc", "worker", "task", "name", "t0", "t1", "end", "parent", "created", "peer"};
    std::fill(col, col + 11, -1);
    bool versionSeen = false, headerSeen = false;
    int lineNo = 0;
    double tmin = 1e300, tmax = -1e300;
    QSet<int> locs;

    while (!f.atEnd()) {
        QByteArray line = f.readLine();
        ++lineNo;
        while (!line.isEmpty() && (line.back() == '\n' || line.back() == '\r'))
            line.chop(1);
        if (line.isEmpty())
            continue;
        if (line[0] == '#') {
            if (line.startsWith("# eduhpx_tasks_csv")) {
                if (line.mid(18).trimmed().toInt() != 1)
                    return fail(tr("%1: format version %2 is not supported").arg(shown, QString::fromUtf8(line.mid(18).trimmed()))), false;
                versionSeen = true;
            } else {
                for (const QByteArray &part : line.mid(1).trimmed().split(' ')) {
                    if (part.startsWith("run="))
                        runName = QString::fromUtf8(part.mid(4));
                }
            }
            continue;
        }
        if (!versionSeen)
            return fail(tr("%1 is not an EduHPX task file (it must start with \"# eduhpx_tasks_csv 1\")").arg(shown)), false;
        char *fields[kMaxFields];
        const int n = splitInPlace(line.data(), fields, kMaxFields);
        if (n < 0)
            return fail(tr("%1, line %2: a quote is not closed").arg(shown).arg(lineNo)), false;
        if (!headerSeen) {
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < 11; ++c)
                    if (qstrcmp(fields[i], colNames[c]) == 0)
                        col[c] = i;
            for (int c : {0, 1, 2, 3, 4, 5, 6})
                if (col[c] < 0)
                    return fail(tr("%1: the column \"%2\" is missing").arg(shown, QString::fromLatin1(colNames[c]))), false;
            headerSeen = true;
            continue;
        }
        auto field = [&](int c) -> const char * { return col[c] >= 0 && col[c] < n ? fields[col[c]] : ""; };
        const char *kind = field(0);
        if (qstrcmp(kind, "slice") == 0) {
            Slice s;
            const int loc = std::atoi(field(1)), worker = std::atoi(field(2));
            s.t0 = std::strtod(field(5), nullptr);
            s.t1 = std::strtod(field(6), nullptr);
            s.task = std::strtoull(field(3), nullptr, 10);
            s.parent = std::strtoull(field(8), nullptr, 10);
            s.created = *field(9) ? std::strtod(field(9), nullptr) : -1;
            const char *e = field(7);
            s.end = qstrcmp(e, "yielded") == 0 ? 1 : qstrcmp(e, "suspended") == 0 ? 2 : 0;
            const char *nm = field(4);
            const QByteArray key = QByteArray::fromRawData(nm, int(strlen(nm)));
            auto it = nameIndex.find(key);
            if (it == nameIndex.end()) {
                it = nameIndex.insert(QByteArray(nm), m_names.size());
                m_names.append(QString::fromUtf8(nm));
            }
            s.name = it.value();
            rows[(quint32(loc) << 16) | quint32(worker & 0xFFFF)].append(s);
            tasks.insert(s.task);
            locs.insert(loc);
            tmin = std::min(tmin, s.t0);
            tmax = std::max(tmax, s.t1);
        } else if (qstrcmp(kind, "send") == 0 || qstrcmp(kind, "recv") == 0) {      // which thread sent a parcel / ran its action
            ParcelEnds &e = m_parcels[QByteArray(field(10))];
            if (kind[0] == 's') {
                e.sendLoc = std::atoi(field(1));
                e.sendWorker = std::atoi(field(2));
            } else {
                e.recvLoc = std::atoi(field(1));
                e.recvWorker = std::atoi(field(2));
            }
        } else if (qstrcmp(kind, "wake") == 0) {     // a value set on one worker thread woke a task that went on on another
            m_wakes.append({std::atoi(field(1)), std::atoi(field(2)), std::atoi(field(10)), std::strtod(field(5), nullptr), std::strtod(field(6), nullptr)});
        } else if (qstrcmp(kind, "dep") == 0) {
            rawDeps.append({std::strtoull(field(3), nullptr, 10), std::strtoull(field(10), nullptr, 10),
                            std::strtod(field(5), nullptr), std::strtod(field(6), nullptr)});
        }
    }
    if (!versionSeen)
        return fail(tr("%1 is not an EduHPX task file (it must start with \"# eduhpx_tasks_csv 1\")").arg(shown)), false;
    if (rows.empty())
        return fail(tr("%1 holds no task").arg(shown)), false;

    m_nameInternal = QVector<char>(m_names.size(), 0);
    m_nameBg = QVector<char>(m_names.size(), 0);
    for (int i = 0; i < m_names.size(); ++i) {
        m_nameBg[i] = m_names[i] == QLatin1String("background_work");
        // tasks that HPX runs for itself: timers, AGAS (the namespaces), loading of components, start and stop, the stack switch
        static const char *const marks[] = {"timer", "namespace", "shutdown", "load_components", "pre_main", "run_helper",
                                            "trace_on_new_stack", "background", "agas"};
        const QString n = m_names[i].toLower();
        for (const char *m : marks)
            if (n.contains(QLatin1String(m)))
                m_nameInternal[i] = 1;
    }
    m_nameCount = QVector<int>(m_names.size(), 0);
    m_nameTotal = QVector<double>(m_names.size(), 0.0);
    for (auto &kv : rows) {
        QVector<Slice> &v = kv.second;
        std::stable_sort(v.begin(), v.end(), [](const Slice &a, const Slice &b) { return a.t0 < b.t0; });
        // the time a slice ran itself: a task that runs another one inside (hpx_main waiting, for example) does not count that time
        {
            QVector<int> open;
            for (int i = 0; i < v.size(); ++i) {
                v[i].excl = float(v[i].t1 - v[i].t0);
                while (!open.isEmpty() && v[open.last()].t1 <= v[i].t0)
                    open.removeLast();
                if (!open.isEmpty())
                    v[open.last()].excl -= float(std::min(v[i].t1, v[open.last()].t1) - v[i].t0);
                open.append(i);
            }
        }
        for (const Slice &s : v) {
            ++m_nameCount[s.name];
            if (m_nameInternal[s.name])
                ++m_internalSlices;
            m_nameTotal[s.name] += s.t1 - s.t0;
        }
        m_rowIndex.insert(kv.first, m_rows.size());
        m_rowLoc.append(int(kv.first >> 16));
        m_rowWorker.append(int(kv.first & 0xFFFF));
        // worker 65535: a thread that is no worker of HPX (the parcel is handled there, for example by the thread of the MPI progress)
        m_rowLabels.append((kv.first & 0xFFFF) == 0xFFFF ? QStringLiteral("L%1 other").arg(kv.first >> 16)
                                                         : QStringLiteral("L%1 w%2").arg(kv.first >> 16).arg(kv.first & 0xFFFF));
        m_sliceCount += int(v.size());
        m_rows.append(std::move(v));
    }
    // the rows of the dependencies: the row of the first slice of a task
    if (!rawDeps.isEmpty()) {
        QHash<quint64, int> rowOf;
        for (const RawDep &d : rawDeps) {
            rowOf.insert(d.from, -1);
            rowOf.insert(d.to, -1);
        }
        for (int r = 0; r < m_rows.size(); ++r)
            for (const Slice &s : m_rows[r]) {
                auto it = rowOf.find(s.task);
                if (it != rowOf.end() && it.value() < 0)
                    it.value() = r;
            }
        for (const RawDep &d : rawDeps)
            if (rowOf.value(d.from, -1) >= 0 && rowOf.value(d.to, -1) >= 0)
                m_deps.append({d.from, d.to, d.t0, d.t1, rowOf.value(d.from), rowOf.value(d.to)});
    }
    std::sort(m_deps.begin(), m_deps.end(), [](const Dep &a, const Dep &b) { return a.tFrom < b.tFrom; });
    m_name = runName.isEmpty() ? shown : runName;
    m_tmin = tmin;
    m_tmax = tmax;
    m_taskCount = int(tasks.size());
    m_localities = int(locs.size());
    m_loaded = true;
    m_error.clear();
    emit changed();
    return true;
}

void TaskFile::buildHandovers() const
{
    m_hands.clear();
    m_handsBuilt = true;
    struct T
    {
        double t0 = 1e300, t1 = -1e300;
        int firstRow = -1, lastRow = -1;
        quint64 parent = 0;
        qint32 name = -1;
    };
    QHash<quint64, T> tasks;
    for (int r = 0; r < m_rows.size(); ++r)
        for (const Slice &s : m_rows[r]) {
            T &t = tasks[s.task];
            if (s.t0 < t.t0) {
                t.t0 = s.t0;
                t.firstRow = r;
                t.parent = s.parent;
                t.name = s.name;
            }
            if (s.t1 > t.t1) {
                t.t1 = s.t1;
                t.lastRow = r;
            }
        }
    auto ours = [&](const T &t) {      // a task of the program: not HPX itself, not the driver hpx_main
        if (t.name < 0)
            return false;
        if (m_names.value(t.name).startsWith(QLatin1String("hpx_main")))
            return false;
        return !(m_hideInternal && m_nameInternal.value(t.name) != 0);
    };
    for (auto it = tasks.constBegin(); it != tasks.constEnd(); ++it) {
        const T &c = it.value();
        if (c.parent == 0 || !ours(c))
            continue;
        const auto pit = tasks.constFind(c.parent);
        if (pit == tasks.constEnd() || !ours(*pit))
            continue;
        const int pr = pit->lastRow, cr = c.firstRow;
        if (pr < 0 || cr < 0 || m_rowLoc[pr] != m_rowLoc[cr])
            continue;
        m_hands.append({m_rowLoc[cr], m_rowWorker[pr], m_rowWorker[cr], c.t0, pit->t1, false});
    }
    for (const Wake &w : std::as_const(m_wakes))
        m_hands.append({w.loc, w.from, w.to, w.t1, w.t0, true});
}

QVariantMap TaskFile::wakeInto(int row, double t0) const
{
    QVariantMap out;
    if (row < 0 || row >= m_rowLoc.size())
        return out;
    for (const Wake &w : std::as_const(m_wakes))
        if (w.loc == m_rowLoc[row] && w.to == m_rowWorker[row] && std::abs(w.t1 - t0) < 2.0) {
            out["from"] = w.from;
            out["set"] = w.t0;
            return out;
        }
    return out;
}

QVector<double> TaskFile::handoverLines(double a, double b, int limit) const
{
    if (!m_handsBuilt)
        buildHandovers();
    QVector<double> out;
    for (const Hand &h : std::as_const(m_hands)) {
        if (out.size() / 6 >= limit)
            break;
        if (h.t < a || h.tFrom > b)
            continue;
        const int fr = m_rowIndex.value((quint32(h.loc) << 16) | quint32(h.from & 0xFFFF), -1);
        const int to = m_rowIndex.value((quint32(h.loc) << 16) | quint32(h.to & 0xFFFF), -1);
        if (fr < 0 || to < 0)
            continue;
        out << h.loc << fr << to << h.tFrom << h.t << (h.wake ? 1 : 0);
    }
    return out;
}

QVector<double> TaskFile::handoverTotals(double a, double b) const
{
    if (!m_handsBuilt)
        buildHandovers();
    QMap<int, QPair<double, double>> perLoc;
    for (const Hand &h : std::as_const(m_hands))
        if (h.t >= a && h.t <= b) {
            auto &e = perLoc[h.loc];
            e.first += 1;
            e.second += h.from != h.to ? 1 : 0;
        }
    QVector<double> out;
    for (auto it = perLoc.constBegin(); it != perLoc.constEnd(); ++it)
        out << it.key() << it.value().first << it.value().second;
    return out;
}

QVariantMap TaskFile::handoverMatrix(int loc, double a, double b) const
{
    if (!m_handsBuilt)
        buildHandovers();
    QVector<int> workers;
    for (int r = 0; r < m_rows.size(); ++r)
        if (m_rowLoc[r] == loc)
            workers << m_rowWorker[r];
    std::sort(workers.begin(), workers.end());
    const int n = int(workers.size());
    QVector<double> counts(n * n, 0.0);
    for (const Hand &h : std::as_const(m_hands))
        if (h.loc == loc && h.t >= a && h.t <= b) {
            const int i = int(std::lower_bound(workers.begin(), workers.end(), h.from) - workers.begin());
            const int j = int(std::lower_bound(workers.begin(), workers.end(), h.to) - workers.begin());
            if (i < n && j < n)
                counts[i * n + j] += 1;
        }
    QVariantList w;
    for (int x : std::as_const(workers))
        w << x;
    QVariantMap out;
    out["workers"] = w;
    out["counts"] = QVariant::fromValue(counts);
    return out;
}

void TaskFile::setShowBackground(bool show)
{
    if (show == m_showBackground)
        return;
    m_showBackground = show;
    m_handsBuilt = false;
    emit filterChanged();
}

void TaskFile::setHideInternal(bool hide)
{
    if (hide == m_hideInternal)
        return;
    m_hideInternal = hide;
    m_handsBuilt = false;
    emit filterChanged();
}

QVariantMap TaskFile::criticalChain() const
{
    struct T
    {
        double t0 = 1e300, t1 = -1e300, run = 0, cum = -1;
        quint64 parent = 0;
        qint32 name = -1;
    };
    QVariantMap out;
    QHash<quint64, T> tasks;
    bool anyParent = false;
    for (const QVector<Slice> &v : m_rows)
        for (const Slice &s : v) {
            T &t = tasks[s.task];
            t.t0 = std::min(t.t0, s.t0);
            t.t1 = std::max(t.t1, s.t1);
            t.run += std::max(0.0f, s.excl);
            if (t.name < 0) {
                t.name = s.name;
                t.parent = s.parent;
            }
            anyParent = anyParent || s.parent != 0;
        }
    if (!anyParent)
        return out;
    // the chain with the most running time: for every task the time of the tasks before it on its way back along "made by", plus
    // its own (the tasks of HPX itself count only when they are shown)
    // hpx_main only starts the work and waits for it: it is not part of the chain
    auto driver = [&](int name) { return name >= 0 && m_names.value(name).startsWith(QLatin1String("hpx_main")); };
    auto weight = [&](const T &t) { return ((m_hideInternal && t.name >= 0 && m_nameInternal.value(t.name) != 0) || driver(t.name)) ? 0.0 : t.run; };
    for (auto it = tasks.begin(); it != tasks.end(); ++it) {
        QVector<quint64> path;
        quint64 k = it.key();
        while (k != 0 && tasks.contains(k) && tasks[k].cum < 0 && path.size() < 1000000) {
            path.append(k);
            k = tasks[k].parent;
            if (path.contains(k))
                break;
        }
        double base = (k != 0 && tasks.contains(k) && tasks[k].cum >= 0) ? tasks[k].cum : 0.0;
        for (int n = path.size() - 1; n >= 0; --n) {
            T &t = tasks[path[n]];
            base += weight(t);
            t.cum = base;
        }
    }
    quint64 last = 0;
    double best = -1;
    for (auto it = tasks.constBegin(); it != tasks.constEnd(); ++it)
        if (it->cum > best) {
            best = it->cum;
            last = it.key();
        }
    QVector<quint64> chain;
    QSet<quint64> seen;
    for (quint64 t = last; t != 0 && tasks.contains(t) && !seen.contains(t) && chain.size() < 1000000; t = tasks[t].parent) {
        seen.insert(t);
        chain.append(t);
    }
    if (chain.size() < 2)
        return out;
    // the slices of the chain, and the time in which at least one task of it ran
    // (a slice that mostly holds other tasks, like hpx_main waiting, is a container and is left out)
    struct Piece
    {
        double row, t0, t1;
    };
    QVector<Piece> pieces;
    QHash<int, double> byName;
    double run = 0;
    for (int r = 0; r < m_rows.size(); ++r)
        for (const Slice &s : m_rows[r])
            if (seen.contains(s.task) && !skipped(s) && !driver(s.name) && s.excl >= 0.5 * (s.t1 - s.t0)) {
                pieces.append({double(r), s.t0, s.t1});
                byName[s.name] += s.excl;
                run += s.excl;
            }
    if (pieces.size() < 4)             // two or three tasks are no chain
        return out;
    std::sort(pieces.begin(), pieces.end(), [](const Piece &x, const Piece &y) { return x.t0 < y.t0; });
    QVector<double> slices;
    for (const Piece &p : std::as_const(pieces))
        slices << p.row << p.t0 << p.t1;
    double start = 1e300, lastEnd = -1e300;           // from the first to the last task of the chain that is shown
    for (quint64 t : std::as_const(chain)) {
        const T &tt = tasks[t];
        if ((m_hideInternal && tt.name >= 0 && m_nameInternal.value(tt.name) != 0) || driver(tt.name))
            continue;
        start = std::min(start, tt.t0);
        lastEnd = std::max(lastEnd, tt.t1);
    }
    if (start > lastEnd)
        return out;
    QList<QPair<double, int>> names;
    for (auto it = byName.constBegin(); it != byName.constEnd(); ++it)
        names.append({it.value(), it.key()});
    std::sort(names.begin(), names.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
    QVariantList top;
    for (int k = 0; k < names.size() && k < 4; ++k)
        top.append(QVariantMap{{"name", m_names.value(names[k].second)}, {"us", names[k].first}});
    out["count"] = int(chain.size());
    out["spanUs"] = lastEnd - start;
    out["runUs"] = run;
    out["start"] = start;
    out["end"] = lastEnd;
    out["byName"] = top;
    out["slices"] = QVariant::fromValue(slices);
    return out;
}

QVector<int> TaskFile::parcelRows(const QString &id) const
{
    QVector<int> out{-1, -1};
    const auto it = m_parcels.constFind(id.toLatin1());
    if (it == m_parcels.constEnd())
        return out;
    if (it->sendLoc >= 0)
        out[0] = m_rowIndex.value((quint32(it->sendLoc) << 16) | quint32(it->sendWorker & 0xFFFF), -1);
    if (it->recvLoc >= 0)
        out[1] = m_rowIndex.value((quint32(it->recvLoc) << 16) | quint32(it->recvWorker & 0xFFFF), -1);
    return out;
}

QVector<int> TaskFile::rowsOf(int loc) const
{
    QVector<int> out;
    for (int r = 0; r < m_rows.size() && r < m_rowLoc.size(); ++r)
        if (m_rowLoc[r] == loc)
            out << r;
    return out;
}

bool TaskFile::hasLocality(int loc) const
{
    return m_rowLoc.size() == m_rows.size() && m_rowLoc.contains(loc);
}

QVector<double> TaskFile::utilization(int loc, double a, double b, int bins) const
{
    QVector<double> out;
    if (bins <= 0 || b <= a)
        return out;
    int workers = 0;
    QVector<double> busy(bins, 0.0);
    const double width = (b - a) / bins;
    for (int r = 0; r < m_rows.size(); ++r) {
        if (m_rowLoc[r] != loc)
            continue;
        ++workers;
        const QVector<Slice> &v = m_rows[r];
        int i = int(std::lower_bound(v.begin(), v.end(), a, [](const Slice &s, double t) { return s.t0 < t; }) - v.begin());
        i = std::max(0, i - 64);
        double covered = a;                    // nested slices (a task that runs another one) count once
        for (; i < v.size() && v[i].t0 < b; ++i) {
            if (skipped(v[i]))
                continue;
            const double t0 = std::max({v[i].t0, covered, a}), t1 = std::min(v[i].t1, b);
            covered = std::max(covered, v[i].t1);
            if (t1 <= t0)
                continue;
            int k = std::clamp(int((t0 - a) / width), 0, bins - 1);
            for (double t = t0; t < t1 && k < bins; ++k) {
                const double e = std::min(t1, a + (k + 1) * width);
                busy[k] += e - t;
                t = e;
            }
        }
    }
    if (workers == 0)
        return out;
    out.resize(bins);
    for (int k = 0; k < bins; ++k)
        out[k] = std::min(1.0, busy[k] / (width * workers));
    return out;
}

QVector<double> TaskFile::spans(int row, double a, double b, double usPerPx) const
{
    QVector<double> out;
    if (row < 0 || row >= m_rows.size())
        return out;
    const QVector<Slice> &v = m_rows[row];
    // first slice that starts at or after a, and a few before it: an outer task can start earlier and end inside the window
    int i = int(std::lower_bound(v.begin(), v.end(), a, [](const Slice &s, double t) { return s.t0 < t; }) - v.begin());
    i = std::max(0, i - 64);
    double cs = 0, ce = 0, busy = 0;
    int cn = 0, cname = 0;
    QHash<int, double> inSpan;                    // time of every name in the span that is built
    auto flush = [&] {
        if (cn) {
            int dominant = cname;
            if (cname < 0) {                      // several names: the one with the most time
                double best = -1;
                for (auto it = inSpan.constBegin(); it != inSpan.constEnd(); ++it)
                    if (it.value() > best) {
                        best = it.value();
                        dominant = it.key();
                    }
            }
            out << cs << ce << double(dominant) << double(cn) << (ce > cs ? std::min(1.0, busy / (ce - cs)) : 1.0)
                << double(inSpan.size() > 1 ? 1 : 0);
            cn = 0;
            busy = 0;
            inSpan.clear();
        }
    };
    for (; i < v.size() && v[i].t0 <= b; ++i) {
        const Slice &s = v[i];
        if (s.t1 < a || skipped(s))
            continue;
        // merge what is smaller than a pixel and touches the span before it, and slices with the same name
        if (cn && s.t0 - ce < usPerPx && ((s.t1 - s.t0) < usPerPx || (ce - cs) < usPerPx || s.name == cname)) {
            if (s.name != cname)
                cname = -1;
            ce = std::max(ce, s.t1);
            ++cn;
            busy += s.t1 - s.t0;
            inSpan[s.name] += s.t1 - s.t0;
        } else {
            flush();
            cs = s.t0;
            ce = s.t1;
            cname = s.name;
            cn = 1;
            busy = s.t1 - s.t0;
            inSpan[s.name] = s.t1 - s.t0;
        }
    }
    flush();
    return out;
}

QVariantMap TaskFile::sliceAt(int row, double t, double tolerance) const
{
    QVariantMap m;
    if (row < 0 || row >= m_rows.size())
        return m;
    const QVector<Slice> &v = m_rows[row];
    int i = int(std::lower_bound(v.begin(), v.end(), t - tolerance, [](const Slice &s, double x) { return s.t0 < x; }) - v.begin());
    i = std::max(0, i - 64);
    int best = -1;
    double bestDist = 1e300, bestDur = 1e300;
    for (; i < v.size() && v[i].t0 <= t + tolerance; ++i) {
        const Slice &s = v[i];
        if (skipped(s))
            continue;
        const double dist = t < s.t0 ? s.t0 - t : (t > s.t1 ? t - s.t1 : 0.0);
        if (dist > tolerance)
            continue;
        const double dur = s.t1 - s.t0;
        if (dist < bestDist - 1e-9 || (dist <= bestDist + 1e-9 && dur < bestDur)) {    // the one under the mouse, the inner of nested ones
            best = i;
            bestDist = dist;
            bestDur = dur;
        }
    }
    if (best < 0)
        return m;
    const Slice &s = v[best];
    static const char *const ends[] = {"finished", "yielded", "suspended"};
    m["name"] = m_names.value(s.name);
    m["task"] = QString::number(s.task);
    m["parent"] = s.parent ? QString::number(s.parent) : QString();
    m["t0"] = s.t0;
    m["t1"] = s.t1;
    m["created"] = s.created;
    m["end"] = QString::fromLatin1(ends[s.end % 3]);
    m["row"] = row;
    m["index"] = best;
    return m;
}

int TaskFile::rowOfTask(quint64 task, int *index) const
{
    for (int r = 0; r < m_rows.size(); ++r)
        for (int i = 0; i < m_rows[r].size(); ++i)
            if (m_rows[r][i].task == task) {
                if (index)
                    *index = i;
                return r;
            }
    return -1;
}

QVariantList TaskFile::slicesOf(const QString &taskText) const
{
    QVariantList out;
    const quint64 task = taskText.toULongLong();
    if (!task)
        return out;
    for (int r = 0; r < m_rows.size() && out.size() < 200; ++r)
        for (const Slice &s : m_rows[r])
            if (s.task == task) {
                QVariantMap m;
                m["row"] = r;
                m["t0"] = s.t0;
                m["t1"] = s.t1;
                m["end"] = s.end;
                out.append(m);
            }
    return out;
}

QString TaskFile::nameOfTask(const QString &taskText) const
{
    int i = 0;
    const int r = rowOfTask(taskText.toULongLong(), &i);
    return r < 0 ? QString() : m_names.value(m_rows[r][i].name);
}

QVariantMap TaskFile::relatives(const QString &taskText) const
{
    QVariantMap m;
    const quint64 task = taskText.toULongLong();
    QVariantList children;
    int count = 0;
    quint64 parent = 0;
    QSet<quint64> seen;
    for (const QVector<Slice> &v : m_rows)
        for (const Slice &s : v) {
            if (s.task == task && !parent)
                parent = s.parent;
            if (s.parent == task && task && !seen.contains(s.task)) {
                seen.insert(s.task);
                ++count;
                if (children.size() < 200) {
                    QVariantMap c;
                    c["task"] = QString::number(s.task);
                    c["name"] = m_names.value(s.name);
                    c["t0"] = s.t0;
                    children.append(c);
                }
            }
        }
    m["parent"] = parent ? QString::number(parent) : QString();
    m["parentName"] = parent ? nameOfTask(QString::number(parent)) : QString();
    m["children"] = children;
    m["childCount"] = count;
    return m;
}

QVariantList TaskFile::topNames(int limit) const
{
    QVector<int> order(m_names.size());
    for (int i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [this](int a, int b) { return m_nameTotal[a] > m_nameTotal[b]; });
    QVariantList out;
    if (m_hideInternal)
        order.erase(std::remove_if(order.begin(), order.end(), [this](int n) { return m_nameBg.value(n) == 0 && m_nameInternal.value(n) != 0; }), order.end());
    if (!m_showBackground)
        order.erase(std::remove_if(order.begin(), order.end(), [this](int n) { return m_nameBg.value(n) != 0; }), order.end());
    for (int k = 0; k < order.size() && k < limit; ++k) {
        QVariantMap m;
        m["index"] = order[k];
        m["name"] = m_names[order[k]];
        m["count"] = m_nameCount[order[k]];
        m["total"] = m_nameTotal[order[k]];
        out.append(m);
    }
    return out;
}

QVector<double> TaskFile::dependencies(double a, double b, int limit) const
{
    QVector<double> out;
    // sorted by the time of the producer; a consumer comes later, so the window is searched from a bit before a
    for (int i = 0; i < m_deps.size() && out.size() / 4 < limit; ++i) {
        const Dep &d = m_deps[i];
        if (d.tFrom > b)
            break;
        if (d.tTo < a)
            continue;
        out << d.fromRow << d.toRow << d.tFrom << d.tTo;
    }
    return out;
}

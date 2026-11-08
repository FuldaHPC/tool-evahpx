#include "task_file.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>

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
        } else if (qstrcmp(kind, "dep") == 0) {
            rawDeps.append({std::strtoull(field(3), nullptr, 10), std::strtoull(field(10), nullptr, 10),
                            std::strtod(field(5), nullptr), std::strtod(field(6), nullptr)});
        }
    }
    if (!versionSeen)
        return fail(tr("%1 is not an EduHPX task file (it must start with \"# eduhpx_tasks_csv 1\")").arg(shown)), false;
    if (rows.empty())
        return fail(tr("%1 holds no task").arg(shown)), false;

    m_nameCount = QVector<int>(m_names.size(), 0);
    m_nameTotal = QVector<double>(m_names.size(), 0.0);
    for (auto &kv : rows) {
        QVector<Slice> &v = kv.second;
        std::stable_sort(v.begin(), v.end(), [](const Slice &a, const Slice &b) { return a.t0 < b.t0; });
        for (const Slice &s : v) {
            ++m_nameCount[s.name];
            m_nameTotal[s.name] += s.t1 - s.t0;
        }
        m_rowLabels.append(QStringLiteral("L%1 w%2").arg(kv.first >> 16).arg(kv.first & 0xFFFF));
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
        if (s.t1 < a)
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

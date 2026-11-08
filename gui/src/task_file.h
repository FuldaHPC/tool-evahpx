#ifndef EDUHPX_TASK_FILE_H
#define EDUHPX_TASK_FILE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>

// One task file of EduHPX (CSV, format 1, see docs/task_file_format.md): what the worker threads of HPX did. One row for every
// time a task ran on a worker thread (a "slice"), made by eduhpx-tasks from the task logs of HPX. Times are microseconds
// since the epoch (as in the run file).
//
// The file can be big (millions of slices), so the view does not get them one by one: spans() merges what is smaller than a
// pixel, sliceAt() finds the one under the mouse.
class TaskFile : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QStringList rowLabels READ rowLabels NOTIFY changed)   // "L0 w3": one row per locality and worker thread
    Q_PROPERTY(QStringList names READ names NOTIFY changed)           // what the tasks are called, spans() and sliceAt() give numbers
    Q_PROPERTY(double tmin READ tmin NOTIFY changed)                  // start of the first slice
    Q_PROPERTY(double tmax READ tmax NOTIFY changed)                  // end of the last slice
    Q_PROPERTY(int taskCount READ taskCount NOTIFY changed)
    Q_PROPERTY(int sliceCount READ sliceCount NOTIFY changed)
    Q_PROPERTY(int localities READ localities NOTIFY changed)

public:
    explicit TaskFile(QObject *parent = nullptr);

    // Is this a task file (does it start with "# eduhpx_tasks_csv")?
    Q_INVOKABLE bool isTaskFile(const QUrl &file) const;
    // The file that belongs to this one in the same folder, or an empty URL: for a run file the task file of the same run
    // (tasks.csv, <name>.tasks.csv), for a task file the run file (run.csv, <name>.csv). eduhpx-run writes run.csv and tasks.csv.
    Q_INVOKABLE QUrl companion(const QUrl &file) const;
    Q_INVOKABLE bool load(const QUrl &file);
    Q_INVOKABLE void unload();

    // How busy the worker threads of one locality are: `bins` values from 0 (all idle) to 1 (all busy), for equal parts of the
    // window [a, b] (absolute microseconds). Empty if the locality has no task data.
    Q_INVOKABLE QVector<double> utilization(int loc, double a, double b, int bins) const;
    Q_INVOKABLE bool hasLocality(int loc) const;
    // The slices of a row in the window [a, b] (absolute microseconds), as one list of numbers, six for every span:
    // start, end, name number (of several merged slices: the name with the most time), count of slices, how much of the
    // span is covered by tasks (0 to 1), and 1 if the span holds more than one name.
    // Slices that are shorter than usPerPx and closer than that are merged.
    Q_INVOKABLE QVector<double> spans(int row, double a, double b, double usPerPx) const;
    // The slice of a row at time t (within tolerance microseconds), as a map; empty if there is none.
    // name, task, t0, t1, end, parent, created, row, loc, worker, index
    Q_INVOKABLE QVariantMap sliceAt(int row, double t, double tolerance) const;
    // All slices of a task: [{row, t0, t1, end}], and its relatives: the parent task and the tasks it created.
    Q_INVOKABLE QVariantList slicesOf(const QString &task) const;
    Q_INVOKABLE QVariantMap relatives(const QString &task) const;
    // The names with the most time: [{index, name, count, total}], total in microseconds
    Q_INVOKABLE QVariantList topNames(int limit) const;
    // Name of a task (the name of its first slice), "" if it is not in the file
    Q_INVOKABLE QString nameOfTask(const QString &task) const;
    // Dependencies (a future that was fulfilled by one task and used by another) in the window: flat list of
    // [fromRow, toRow, tFrom, tTo] ... as far as both tasks have a slice there; at most limit
    Q_INVOKABLE QVector<double> dependencies(double a, double b, int limit) const;

    bool loaded() const { return m_loaded; }
    QString name() const { return m_name; }
    QString error() const { return m_error; }
    QStringList rowLabels() const { return m_rowLabels; }
    QStringList names() const { return m_names; }
    double tmin() const { return m_tmin; }
    double tmax() const { return m_tmax; }
    int taskCount() const { return m_taskCount; }
    int sliceCount() const { return m_sliceCount; }
    int localities() const { return m_localities; }

signals:
    void changed();

private:
    struct Slice
    {
        double t0, t1;
        quint64 task, parent;
        double created;       // -1: unknown
        qint32 name;
        quint8 end;           // 0 finished, 1 yielded, 2 suspended
    };
    struct Dep
    {
        quint64 from, to;
        double tFrom, tTo;
        qint32 fromRow, toRow;
    };
    void fail(const QString &message);
    int rowOfTask(quint64 task, int *index) const;    // row and index of the first slice of a task, -1 if none

    bool m_loaded = false;
    QString m_name;
    QString m_error;
    QStringList m_rowLabels;
    QStringList m_names;
    QVector<QVector<Slice>> m_rows;       // sorted by start time
    QVector<int> m_rowLoc;                // the locality of every row
    QVector<Dep> m_deps;
    QVector<int> m_nameCount;
    QVector<double> m_nameTotal;
    double m_tmin = 0, m_tmax = 0;
    int m_taskCount = 0, m_sliceCount = 0, m_localities = 0;
};

#endif // EDUHPX_TASK_FILE_H

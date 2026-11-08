#ifndef EDUHPX_TASK_FILE_H
#define EDUHPX_TASK_FILE_H

#include <QHash>
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
    Q_PROPERTY(bool hideInternal READ hideInternal WRITE setHideInternal NOTIFY filterChanged)   // leave out the tasks of HPX itself (timers, AGAS, start and stop)
    Q_PROPERTY(int internalCount READ internalCount NOTIFY changed)                            // how many slices of them there are

public:
    explicit TaskFile(QObject *parent = nullptr);

    // Is this a task file (does it start with "# eduhpx_tasks_csv")?
    Q_INVOKABLE bool isTaskFile(const QUrl &file) const;
    // The file that belongs to this one in the same folder, or an empty URL: for a run file the task file of the same run
    // (tasks.csv, <name>.tasks.csv), for a task file the run file (run.csv, <name>.csv). eduhpx-run writes run.csv and tasks.csv.
    Q_INVOKABLE QUrl companion(const QUrl &file) const;
    Q_INVOKABLE bool load(const QUrl &file);
    Q_INVOKABLE void unload();

    // Values that one task handed to another one inside a locality (the consumer was made by the producer: for a dataflow
    // the task that finished its last input), counted when the consumer starts in the window [a, b]. Only tasks of the program.
    // handoverTotals: flat list of loc, count, count on another worker thread. handoverMatrix: workers (their numbers) and counts,
    // n * n values, row = the worker of the producer, column = the worker of the consumer.
    Q_INVOKABLE QVector<double> handoverTotals(double a, double b) const;
    Q_INVOKABLE QVariantMap handoverMatrix(int loc, double a, double b) const;
    // The hand-overs that overlap the window [a, b], at most limit, as a flat list of loc, row of the producer's thread, row of the
    // consumer's thread, time of the producer, time of the consumer, 1 if the consumer was woken by a value (else made by the producer)
    // The wake-up that let a task go on at time t0 on the thread of the row: from (worker), set (time the value was set), empty if none
    Q_INVOKABLE QVariantMap wakeInto(int row, double t0) const;
    Q_INVOKABLE QVector<double> handoverLines(double a, double b, int limit) const;
    // The chain of tasks that decided how long the run took: from the task that ended last, back along "made by"
    // (for a dataflow that is the task that finished its last input) to the first one. A map: count (tasks), spanUs (from the
    // start of the first to the end of the last), runUs (time one of them was running), byName [{name, us}] and slices, a flat list
    // of row, start, end for every slice of the chain. Empty if there is no parent information.
    Q_INVOKABLE QVariantMap criticalChain() const;
    // How busy the worker threads of one locality are: `bins` values from 0 (all idle) to 1 (all busy), for equal parts of the
    // window [a, b] (absolute microseconds). Empty if the locality has no task data.
    Q_INVOKABLE QVector<double> utilization(int loc, double a, double b, int bins) const;
    Q_INVOKABLE bool hasLocality(int loc) const;
    // The worker threads of a parcel (its id as in the run file): [row of the sender's thread, row of the thread that ran its action],
    // -1 where it is not known (see spans() for the rows; the task data must have been recorded with parcels)
    Q_INVOKABLE QVector<int> parcelRows(const QString &id) const;
    // The rows (see spans()) of the worker threads of a locality, in the order of the thread numbers
    Q_INVOKABLE QVector<int> rowsOf(int loc) const;
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

    bool hideInternal() const { return m_hideInternal; }
    void setHideInternal(bool hide);
    int internalCount() const { return m_internalSlices; }
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
    void filterChanged();

private:
    struct Slice
    {
        double t0, t1;
        quint64 task, parent;
        double created;       // -1: unknown
        qint32 name;
        quint8 end;           // 0 finished, 1 yielded, 2 suspended
        float excl = 0;       // microseconds in which this slice ran itself (not the tasks that ran inside it)
    };
    struct Dep
    {
        quint64 from, to;
        double tFrom, tTo;
        qint32 fromRow, toRow;
    };
    bool skipped(const Slice &s) const { return m_hideInternal && s.name >= 0 && s.name < m_nameInternal.size() && m_nameInternal[s.name]; }
    void fail(const QString &message);
    int rowOfTask(quint64 task, int *index) const;    // row and index of the first slice of a task, -1 if none

    bool m_loaded = false;
    QString m_name;
    QString m_error;
    QStringList m_rowLabels;
    QStringList m_names;
    QVector<QVector<Slice>> m_rows;       // sorted by start time
    bool m_hideInternal = true;
    QVector<char> m_nameInternal;         // per name: a task of HPX itself
    int m_internalSlices = 0;
    struct Hand
    {
        int loc, from, to;      // the locality and the worker threads of the producer and of the consumer
        double t;               // when the consumer starts
        double tFrom;           // when the producer ended / the value was set
        bool wake;              // a task woken by a value (else: a task that was made by the producer)
    };
    struct Wake
    {
        qint32 loc, from, to;
        double t0, t1;
    };
    QVector<Wake> m_wakes;
    void buildHandovers() const;
    mutable QVector<Hand> m_hands;
    mutable bool m_handsBuilt = false;
    struct ParcelEnds
    {
        qint32 sendLoc = -1, sendWorker = -1, recvLoc = -1, recvWorker = -1;
    };
    QHash<QByteArray, ParcelEnds> m_parcels;
    QHash<quint32, int> m_rowIndex;       // (locality << 16 | worker) -> row
    QVector<int> m_rowWorker;
    QVector<int> m_rowLoc;                // the locality of every row
    QVector<Dep> m_deps;
    QVector<int> m_nameCount;
    QVector<double> m_nameTotal;
    double m_tmin = 0, m_tmax = 0;
    int m_taskCount = 0, m_sliceCount = 0, m_localities = 0;
};

#endif // EDUHPX_TASK_FILE_H

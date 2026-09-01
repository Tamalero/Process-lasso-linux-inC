#pragma once
#include "processinfo.h"
#include "ruleengine.h"
#include "probalance.h"
#include "sensors.h"
#include <QThread>
#include <QMutex>
#include <QSet>
#include <QHash>
#include <QJsonObject>

class ProcessMonitor : public QThread {
    Q_OBJECT
public:
    ProcessMonitor(RuleEngine *ruleEngine,
                   ProBalance *proBalance,
                   const QJsonObject &config,
                   QObject *parent = nullptr);

    void stop();
    void updateConfig(const QJsonObject &config);
    void reapplyAllDefaults();
    void resetAllAffinities();
    void setGamingMode(bool active, bool elevateNice);
    void setManualAffinityOverride(int pid, double durationSeconds = 30.0);
    void setPbExempt(int pid, bool exempt);
    // Observe-only: keep monitoring, stop applying anything config-driven.
    void setSafeMode(bool on);
    QSet<int> pbManualExempt() const;

signals:
    void processSnapshotReady(QList<ProcessInfo> snapshot);
    void cpuSnapshotReady(QList<double> percpu);
    void sensorsReady(SensorSnapshot sensors);
    void logMessage(QString msg);

protected:
    void run() override;

private:
    // Process stats for CPU% tracking
    struct ProcCpuState {
        long long prevTicks  = -1;
        qint64    prevWallNs = 0;
        double    cpuPercent = 0.0;
    };

    // Per-CPU system stats for percpu bars
    struct SysCpuStat {
        long long idle  = 0;
        long long total = 0;
    };

    RuleEngine   *m_ruleEngine;
    ProBalance   *m_proBalance;
    QJsonObject   m_config;
    mutable QMutex m_configMux;
    bool          m_stop = false;
    bool          m_safeMode = false;   // guarded by m_configMux

    QSet<int>               m_knownPids;
    QHash<int, QSet<int>>   m_originalAffinities; // pid → original affinity
    QHash<int, ProcCpuState>m_cpuStates;
    QHash<int, SysCpuStat>  m_sysCpuPrev;

    bool              m_gamingMode      = false;
    bool              m_gamingNice      = false;
    QHash<int, int>   m_gamingNiced;     // pid → original nice

    QHash<int, double> m_manualOverrides;  // pid → expiry monotonic s
    QSet<int>          m_pbManualExempt;   // pids manually exempt from ProBalance

    QString defaultAffinity() const;
    void    applyNewPid(const ProcessInfo &info);
    void    restoreGamingNices();
    void    captureOriginal(int pid);
    void    emitLog(const QString &msg);

    // /proc readers
    static bool readProcStat(int pid, long long &utime, long long &stime,
                              int &nice, qint64 &rss);
    static QString readComm(int pid);
    static QStringList readCmdline(int pid);
    static QString readAffinityStr(int pid);
    static QString readIoNice(int pid);

    // System-wide per-CPU usage
    QList<double> readPercpuUsage();
};

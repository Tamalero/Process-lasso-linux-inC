#pragma once
#include <QString>
#include <QSet>

enum class TopologyKind { Uniform, AmdX3D, IntelHybrid };

struct CpuTopology {
    TopologyKind kind        = TopologyKind::Uniform;
    QSet<int>    preferred;
    QSet<int>    nonPreferred;
    QString      description;

    bool hasAsymmetry() const { return !nonPreferred.isEmpty(); }
};

CpuTopology detectTopology();
QSet<int>   getSmtSiblingsOf(const QSet<int> &cpus);
QSet<int>   parseCpulistFile(const QString &path);
QSet<int>   getOnlineCpuSet();
QSet<int>   getOfflineCpuSet();
QString     formatCpuSet(const QSet<int> &cpus);

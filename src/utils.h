#pragma once
#include <QString>
#include <QSet>
#include <QList>

namespace Utils {

QSet<int>  cpulistToSet(const QString &cpulist);
QString    cpusetToCpulist(const QSet<int> &cpus);
QList<int> getTids(int pid);

bool    setAffinity(int pid, const QString &cpulist);
QString getAffinityStr(int pid);

bool setNice(int pid, int nice);
bool setIoNice(int pid, int ioclass, int iolevel);

QSet<int> getOnlineCpus();
int       getCpuCount();
bool      validateCpulist(const QString &cpulist);

QString resolveName(const QString &comm, const QStringList &cmdline);

} // namespace Utils

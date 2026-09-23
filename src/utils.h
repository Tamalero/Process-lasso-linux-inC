#pragma once
#include <QString>
#include <QSet>
#include <QList>

namespace Utils {

QSet<int>  cpulistToSet(const QString &cpulist);
QString    cpusetToCpulist(const QSet<int> &cpus);
QList<int> getTids(int pid);

// errnoOut, when given, receives 0 on success or the first failing errno.
// "Failed to set affinity" on its own is useless: EPERM (someone else's
// process), EINVAL (every requested CPU parked) and ESRCH (it exited a
// microsecond ago) are three completely different situations.
bool    setAffinity(int pid, const QString &cpulist, int *errnoOut = nullptr);
// Plain-language explanation of a setAffinity failure, naming the owner when
// the problem is ownership.
QString describeAffinityError(int err, int pid);
QString getAffinityStr(int pid);

bool setNice(int pid, int nice);
bool setIoNice(int pid, int ioclass, int iolevel);

// Readers used to skip no-op writes. Each returns false when the value cannot
// be read (process gone), in which case the caller should just try the write.
bool getNice(int pid, int &nice);
bool getIoNice(int pid, int &ioclass, int &iolevel);

QSet<int> getOnlineCpus();
int       getCpuCount();
bool      validateCpulist(const QString &cpulist);

QString resolveName(const QString &comm, const QStringList &cmdline);

} // namespace Utils

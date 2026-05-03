#pragma once
#include "cputopology.h"
#include <QSet>
#include <functional>

namespace CpuPark {

inline constexpr auto HELPER      = "/usr/local/bin/process-lasso-helper";
inline constexpr auto SUDOERS_FILE = "/etc/sudoers.d/process-lasso";

bool isHelperInstalled();
bool isSudoersInstalled();
bool isHelperCurrent();

// Install helper + sudoers rule using pkexec (no password prompt needed).
// Returns {ok, message}.
std::pair<bool, QString> installHelper(const QString &username = {});

// Park/unpark via helper
bool parkCpus(const QSet<int> &cpus,
              std::function<void(const QString &)> logCb = nullptr);
bool unParkAll(std::function<void(const QString &)> logCb = nullptr);

// Renice via helper (needed for negative nice values requiring root)
bool setProcessNiceViaHelper(int pid, int niceVal);

} // namespace CpuPark

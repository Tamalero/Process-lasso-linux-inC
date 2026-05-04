# CLAUDE.md — LLM Context for process-lasso-qt

C++17/Qt6 Linux process manager for CachyOS/Arch. Replaces a Python/PyQt6 upstream with
direct syscalls. No Python, no psutil, no subprocess (except the privileged helper).

---

## Quick orientation

```
CMakeLists.txt          — two targets: process-lasso-qt, process-lasso-helper
src/
  main.cpp              — entry point; creates MainWindow; defines gVerbose
  verbose.h             — gVerbose flag + VLOG macro; enable with --verbose CLI flag
  processinfo.h         — plain struct, no QObject
  config.{h,cpp}        — load/save ~/.config/process-lasso-qt/config.json
  ruleengine.{h,cpp}    — match + apply rules to PIDs
  probalance.{h,cpp}    — CPU throttle state machine (not a QObject)
  processmonitor.{h,cpp}— QThread background loop; reads /proc, fires signals
  cpupark.{h,cpp}       — park/unpark CPUs via helper binary
  cputopology.{h,cpp}   — detect AMD X3D / Intel Hybrid / Uniform
  utils.{h,cpp}         — affinity, nice, ionice, /proc helpers
  gui/
    mainwindow.{h,cpp}  — QMainWindow; owns all objects; wires all signals
    cpubarwidget.{h,cpp}— CpuBarsWidget (per-core bars, dynamic height via applyNeededHeight/resizeEvent)
                          + CpuHistoryWidget (avg CPU area graph, fixed 80 px height)
    processtablewidget.{h,cpp} — QTableWidget subclass with context menu
    ruleseditor.{h,cpp} — rule list table + add/edit/delete/presets
    probalancetab.{h,cpp}— ProBalance settings form
    gamingmodetab.{h,cpp}— park/unpark, profiles, launcher, game watcher
    settingstab.{h,cpp} — default affinity, intervals, theme, autostart
    dialogs.{h,cpp}     — RuleDialog, AffinityDialog, NiceDialog,
                          SteamGamePickerDialog, LutrisGamePickerDialog
helper/
  main.cpp              — privileged C binary (no Qt), commands below
packaging/
  PKGBUILD              — Arch Linux package
  process-lasso.desktop — XDG desktop entry
  install-helper.sh     — root install script for helper + sudoers
```

---

## Thread model

```
GUI thread (main)
  └─ MainWindow owns:
       RuleEngine       (no thread, called from monitor thread via signal)
       ProBalance*      (no thread, called from monitor thread via signal)
       ProcessMonitor*  (QThread — run() is the background loop)

ProcessMonitor::run()  [background thread]
  reads /proc every 100 ms
  emits processSnapshotReady(QList<ProcessInfo>)  → MainWindow::onSnapshot()
  emits cpuSnapshotReady(QList<double>)           → MainWindow::onCpuForTray()
  emits logMessage(QString)                       → MainWindow::appendLog()
  calls m_ruleEngine->applyToProcess()  [direct call — RuleEngine has no mutex]
  calls m_proBalance->tick()            [direct call — ProBalance has no mutex]

GamingModeTab park/unpark workers
  QThread* workers created inline, moved-to-thread objects handle
  CpuPark::parkCpus() / CpuPark::unParkAll()
  emit done signal back to GUI thread
```

**Mutex rule**: `ProcessMonitor::m_configMux` is declared `mutable` so it can be
locked inside `defaultAffinity() const`. Any time you add a const getter that reads
`m_config`, lock this mutex. Non-const setters also lock it.

---

## Ownership and lifetimes

| Object | Owner | Notes |
|--------|-------|-------|
| `RuleEngine m_ruleEngine` | `MainWindow` (by value) | shared with ProcessMonitor* ptr |
| `ProBalance *m_proBalance` | `MainWindow` | raw new, deleted in ~MainWindow **after** `m_monitor->wait()` — the monitor thread calls `tick()` directly, so the thread must be stopped first |
| `ProcessMonitor *m_monitor` | `MainWindow` | QThread; call stop() then wait() before delete |
| All tab widgets | `MainWindow` via QTabWidget | Qt parent chain owns them |
| `GamingModeTab::m_watchTimer` | `GamingModeTab` | created once in buildUi() |
| `GamingModeTab::m_launchProc` | `GamingModeTab` | QProcess*, may be nullptr |

`ProBalance` constructor: `ProBalance(const QJsonObject &cfg, LogCb logCb)` — takes a
`std::function`, **not** a QObject parent. Do not pass `this` as second arg.

---

## Key data flows

### New process appears
```
ProcessMonitor::run()
  → newPids detected
  → applyNewPid(info)
      → captureOriginal(pid)          saves original CPU affinity
      → m_ruleEngine->applyToProcess()
          if match: apply affinity/nice/ionice via Utils::set*
          else: apply default affinity from config
      → if gamingMode && gamingNice: renice via helper
```

### Rule enforcement cycle (every enforceInterval ms, default 500)
```
ProcessMonitor::run()
  → for each pid in snapshot (skip manualOverrides)
      → m_ruleEngine->applyToProcess(pid, name)
```

### ProBalance tick (every 1 s)
```
ProcessMonitor::run()
  → m_proBalance->tick(snapshot, tickSeconds)
      → for each process: update ProcState (Normal/Throttled)
      → throttle: setpriority(PRIO_PROCESS, pid, throttleNice)
      → restore: setpriority(PRIO_PROCESS, pid, originalNice)
```

### CPU snapshot → tray icon
```
ProcessMonitor::cpuSnapshotReady(QList<double>)
  → MainWindow::onCpuForTray()
      avg = mean of all per-cpu values
      → makeTrayIcon(avg)   draws 22×22 QPainter bar
      → m_tray->setIcon() + setToolTip()
```

---

## Privileged helper

Binary: `/usr/local/bin/process-lasso-helper`  
Installed by: `packaging/install-helper.sh` (run as root)  
Invoked by: `CpuPark::parkCpus()`, `CpuPark::unParkAll()`, `CpuPark::setProcessNiceViaHelper()`

```
process-lasso-helper cpu-online <N> <0|1>    # park/unpark single CPU
process-lasso-helper cpu-unpark-all          # reads /sys/.../offline, brings all back
process-lasso-helper renice-pid <nice> <pid> # setpriority(PRIO_PROCESS, pid, nice)
process-lasso-helper --check-only            # exit 0, used to verify sudo access
```

Sudoers entry written by install-helper.sh:
```
ALL ALL=(root) NOPASSWD: /usr/local/bin/process-lasso-helper
```

`CpuPark::isHelperInstalled()` — checks file exists and is executable  
`CpuPark::isSudoersInstalled()` — checks `/etc/sudoers.d/process-lasso` exists  
`CpuPark::installHelper()` — copies via pkexec, writes sudoers

**Critical**: `HELPER` constant in `cpupark.h` is `inline constexpr auto` (C string),
not a macro. **Never** pass it to `QStringLiteral()` — use `helperPath()` helper
function defined in cpupark.cpp which returns `QStringLiteral("/usr/local/bin/process-lasso-helper")`.

---

## Config schema (config.json)

Stored at: `~/.config/process-lasso-qt/config.json`  
Loaded by: `Config::load()` → deep-merged with `Config::defaultConfig()`  
Saved by: `Config::save()` — atomic write (`.tmp` + rename)

```jsonc
{
  "cpu": {
    "default_affinity": "",           // cpulist string, e.g. "0-7"; empty = no default
    "gaming_mode": false,
    "gaming_profiles": {}             // name → { affinity, parkCpus[], ... }
  },
  "monitor": {
    "rule_enforce_interval_ms": 500,
    "display_refresh_interval_ms": 2000
  },
  "probalance": {
    "enabled": true,
    "cpu_threshold": 80.0,            // % to trigger throttle
    "consecutive_seconds": 3,
    "nice_adjustment": 5,
    "nice_floor": 10,                 // max nice value during throttle
    "restore_threshold": 60.0,
    "restore_hysteresis_seconds": 5,
    "exempt": ["systemd", "Xorg", "kwin_wayland", "plasmashell"]
  },
  "rules": [],                        // array of Rule JSON objects
  "ui": {
    "system_theme": false,
    "opacity": 100
  }
}
```

`Config::deepMerge(base, override)` recursively merges — override wins on leaf keys,
both objects merged for nested objects.

---

## Rule struct

```cpp
struct Rule {
    QString  ruleId;      // UUID without braces
    QString  name;        // display label
    QString  pattern;     // match string
    QString  matchType;   // "contains" | "exact" | "regex"
    std::optional<QString> affinity;     // cpulist or empty
    std::optional<int>     nice;
    std::optional<int>     ioniceClass;  // 0=none 1=RT 2=BE 3=idle
    std::optional<int>     ioniceLevel;  // 0-7
    bool     enabled;
};
```

`Rule::matches(name)` uses Qt case-insensitive contains/exact/QRegularExpression.  
`RuleEngine::applyToProcess()` iterates `m_rules`, calls `rule.matches()`, applies all
matching rules, returns QStringList of action descriptions. **Empty return = no match.**

---

## CPU topology detection

`detectTopology()` in cputopology.cpp — returns `CpuTopology`:

- **AMD X3D**: reads `/sys/.../cpu*/cache/index3/size` — if top 50% of CPUs have ≥2× larger
  L3 than bottom 50%, those are the "preferred" (X3D) cores.
- **Intel Hybrid**: reads `/sys/.../cpu*/cpufreq/cpuinfo_max_freq` — CPUs with freq ≥80%
  of max are "preferred" (P-cores). Rest are E-cores.
- **Uniform**: all CPUs identical.

`GamingModeTab::detectTopology()` — **this is a void method** that sets `m_topo`.
The global free function `::detectTopology()` returns `CpuTopology`.
Inside GamingModeTab, call `m_topo = ::detectTopology()` (not `detectTopology()`) to
avoid infinite recursion through the method shadowing the free function.

---

## Qt6-specific gotchas (already fixed, do not regress)

| Issue | Location | Fix applied |
|-------|----------|-------------|
| `QVariant::operator<` removed | processtablewidget.cpp | Explicit typed switch in sort lambda |
| `qAsConst` deprecated (Qt 6.6+) | processmonitor.cpp | Use `std::as_const` |
| `QStandardPaths::DataLocation` renamed | cpupark.cpp | Use `AppDataLocation` |
| `QTextEdit::setMaximumBlockCount` DNE | mainwindow.cpp | Use `->document()->setMaximumBlockCount()` |
| `QStringLiteral(CONSTEXPR_VAR)` fails | cpupark.cpp | Use inline `helperPath()` function |
| `QHelpEvent` incomplete | cpubarwidget.cpp | `#include <QHelpEvent>` |
| `QProcess` incomplete | dialogs.cpp | `#include <QProcess>` |
| `QThread`/`csignal` incomplete | gamingmodetab.cpp | Added includes |
| `mutable QMutex` for const method | processmonitor.h | `mutable QMutex m_configMux` |
| ProBalance ctor takes LogCb not QObject* | mainwindow.cpp | Lambda `[this](const QString &msg){ appendLog(msg); }` |
| `QStringList{helperPath(), ...}` fails | cpupark.cpp | Use `QStringList() << a << b` idiom |

## /proc virtual filesystem gotcha (critical)

**`QFile::atEnd()` always returns `true` for `/proc` virtual files.**  
Virtual files report `size() == 0` to the VFS layer, so Qt's `atEnd()` check
(`pos() >= size()`) short-circuits immediately and the loop body never runs.

**Wrong** — loop body never executes:
```cpp
while (!f.atEnd()) {
    const QByteArray line = f.readLine(); // never reached
    ...
}
```

**Correct** — read everything at once, then split:
```cpp
const QByteArray data = f.readAll();
for (const QByteArray &line : data.split('\n')) {
    ...
}
```

This applies to every `/proc` file: `/proc/stat`, `/proc/[pid]/stat`, `/proc/[pid]/comm`, etc.
All existing readers in this codebase use `readAll()` — do not introduce `readLine()` loops.

---

## Verbose / debug mode

Pass `--verbose` on the command line (or via `run.sh --verbose`) to enable runtime
diagnostics. All output goes to stderr prefixed with `[V]`.

```
./run.sh --verbose 2>&1 | grep '\[V\]'
```

Instrumented code paths:
- `ProcessMonitor::run()` — logs `percpu size` and whether `cpuSnapshotReady` was emitted
- `CpuBarsWidget::updateCpu` — CPU count, widget geometry, visibility
- `CpuBarsWidget::applyNeededHeight` — n, cols, rows, needed vs current height
- `CpuBarsWidget::resizeEvent` — new geometry on every resize
- `CpuBarsWidget::paintEvent` — first paint + every 20th (rate-limited)
- `CpuHistoryWidget::updateCpu` — average, history depth, widget geometry

The macro is defined in `src/verbose.h`; `gVerbose` is defined in `src/main.cpp`.

---

## Build

```bash
cd process-lasso-qt
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
# Outputs:
#   build/process-lasso-qt    (main application)
#   build/process-lasso-helper (privileged helper)
```

Requirements: `qt6-base`, `cmake ≥ 3.20`, `gcc`/`clang` with C++17.  
No Python. No Qt5. No extra Qt6 modules beyond `Widgets`.

---

## Signal wiring (MainWindow)

All wiring lives in `MainWindow::buildUi()` and `MainWindow::startMonitor()`.

```
ProcessMonitor::processSnapshotReady  → MainWindow::onSnapshot
ProcessMonitor::cpuSnapshotReady      → CpuHistoryWidget::updateCpu
                                      → CpuBarsWidget::updateCpu
                                      → MainWindow::onCpuForTray
ProcessMonitor::logMessage            → MainWindow::appendLog
RulesEditor::rulesChanged             → MainWindow::onRulesChanged
ProcessTableWidget::ruleAddRequested  → MainWindow::onRuleAddFromTable
ProcessTableWidget::affinityManually  → MainWindow::onAffinityManualChange
ProBalanceTab::settingsChanged        → MainWindow::onPbSettingsChanged
GamingModeTab::gamingModeChanged      → MainWindow::onGamingModeChanged
GamingModeTab::resetRequested         → MainWindow::onResetRequested
GamingModeTab::logMessage             → MainWindow::appendLog
SettingsTab::settingsChanged          → MainWindow::onSettingsChanged
```

`MainWindow::saveConfig()` serialises `m_config`, calls `Config::save()`, and calls
`m_monitor->updateConfig()` (thread-safe via mutex).

---

## ProcessTableWidget sort

Column indices:

| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|
| PID | Name | CPU% | Mem(MB) | Nice | Affinity | I/O Nice | Cmdline |

Default sort: column 2 (CPU%), descending. Header click toggles asc/desc.
Sort comparator uses explicit `switch(m_sortCol)` with typed comparisons — no QVariant.

---

## Wine/Proton name resolution

`Utils::resolveName(comm, cmdline)` in utils.cpp:

1. If `comm.length() == 15` (truncated by kernel) and cmdline[0] ends in `.exe`,
   use the Windows `.exe` basename.
2. Otherwise use `comm`.

This ensures Wine/Proton games match rules written for their Windows executable names.

---

## Gaming Mode profiles

Stored under `config["cpu"]["gaming_profiles"]` as a JSON object keyed by profile name.
Each profile value is a JSON object with at minimum `{ "affinity": "...", "parkCpus": [...] }`.

`GamingModeTab::refreshProfilesCombo()` reads these keys into `m_profileCombo`.  
`GamingModeTab::saveProfile()` writes current UI state to the profile key, emits `configChanged`.  
`GamingModeTab::loadProfile()` reads the selected profile and updates UI controls.

---

## Autostart

`SettingsTab::applyAutostart()` writes/removes:
```
~/.config/systemd/user/process-lasso.service
```
Then calls `systemctl --user enable/disable process-lasso.service` via `QProcess::execute`.

---

## Catppuccin Mocha theme

Applied in `MainWindow::applyTheme()` when `config["ui"]["system_theme"]` is false.
~30-line QSS stylesheet hard-coded in mainwindow.cpp. Cleared (empty string) for system theme.
Window opacity comes from `config["ui"]["opacity"]` (0–100), set via `setWindowOpacity(v/100.0)`.

---

## /proc reading — field layout

`ProcessMonitor::readProcStat()` parses `/proc/[pid]/stat`:

The process name in field 2 may contain spaces and parentheses. The parser finds
`lastIndexOf(')')` to locate the end of the name field, then splits everything after
`") "` by spaces. **Field indices after the last `)` are 0-based**:

| Index | Meaning |
|-------|---------|
| 0 | state |
| 1 | ppid |
| 11 | utime (jiffies) |
| 12 | stime (jiffies) |
| 16 | nice |
| 21 | rss (pages) |

CPU% formula: `(delta_ticks / HZ) / elapsed_wall_seconds * 100`  
`HZ = sysconf(_SC_CLK_TCK)` — initialised in ProcessMonitor constructor.

---

## Extending the codebase

**Adding a new tab**: create `src/gui/mytab.{h,cpp}`, add to `APP_SOURCES` in CMakeLists.txt,
include in mainwindow.h, construct and add in `MainWindow::buildUi()`.

**Adding a new rule action**: add field to `Rule` struct (use `std::optional<T>`),
update `Rule::toJson()`/`fromJson()`, update `RuleEngine::applyToProcess()` to call
the relevant `Utils::set*()` function, update `dialogs.cpp` RuleDialog form.

**Adding a new helper command**: add handler in `helper/main.cpp` (pure C, no Qt),
add a wrapper in `cpupark.{h,cpp}`, invoke via `QProcess::execute("sudo", {"path", ...})`.

**Thread safety**: RuleEngine and ProBalance have no internal mutex — they are only
ever called from the monitor thread. Do not call them from the GUI thread directly.
If you need GUI → engine communication, go through `ProcessMonitor::updateConfig()`.

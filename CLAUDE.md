# CLAUDE.md — LLM Context for process-lasso-qt

C++17/Qt6 Linux process manager for CachyOS/Arch. Replaces a Python/PyQt6 upstream with
direct syscalls. No Python, no psutil, no subprocess (except the privileged helper).
Current version: **1.3.3**.

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
  sensors.{h,cpp}       — hwmon temperature sweep (CPU package/cores, DIMMs)
  runstate.{h,cpp}      — unclean-shutdown marker, crash counter, safe mode
  cpupark.{h,cpp}       — park/unpark CPUs via helper binary
  cputopology.{h,cpp}   — detect AMD X3D / Intel Hybrid / Uniform
  utils.{h,cpp}         — affinity, nice, ionice, /proc helpers
  gui/
    mainwindow.{h,cpp}  — QMainWindow; owns all objects; wires all signals
    cpubarwidget.{h,cpp}— CpuBarsWidget (per-core bars, dynamic height via applyNeededHeight/resizeEvent)
                          + CpuHistoryWidget (avg CPU area graph, expands to match bars column height,
                            title drawn inside the graph as text overlay)
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
  process-lasso.png     — 256×256 app icon (Catppuccin Mocha CPU chip)
  install-helper.sh     — root install script for helper + sudoers
  build-appimage.sh     — Type 2 AppImage build script (see AppImage section below)
```

---

## Branches

`main` is the released line (currently 1.3.0). One feature lives off it:

**`fan-control`** — hwmon PWM fan control (Fan Control tab, curve editor, six
new privileged-helper commands, v1.4.0). Complete, builds clean, and verified
end-to-end on real hardware. Deliberately kept **off `main`** at Cesar's request
on 2026-09-01, because motherboard fan control does not currently work on his
Gigabyte Z690 AORUS PRO — a mainline `it87` limitation, not a bug in this code.

Before doing any fan-related work, read `CLAUDE.md` **on that branch**: it has
the register-level diagnosis (mainline `it87` never clears the IT8689E
SmartGuardian bit), the upstream issue/PR references, and the three fixes to try
in order. That research is not repeated here — do not redo it.

```bash
git show fan-control:CLAUDE.md | sed -n '/Hardware reality check/,/Qt6-specific/p'
```

One change on that branch is *not* fan-specific and is worth cherry-picking if
the topic comes up: `src/main.cpp` gains a SIGTERM/SIGINT/SIGHUP handler (Qt
socketpair pattern) that routes through `MainWindow::quitApp()`, so a
signal-terminated process still saves config and unparks CPUs. That addresses
the "unclean shutdown may leave CPUs parked" limitation in README.md.

---

## Shutdown path (v1.3.1)

```
SIGTERM/SIGINT/SIGHUP ─▶ handler writes 1 byte to a socketpair (async-signal-safe)
                          └▶ QSocketNotifier on the event loop ─▶ MainWindow::quitApp()
tray Quit ────────────────────────────────────────────────────▶ MainWindow::quitApp()
window close (no tray) ───────────────────────────────────────▶ MainWindow::quitApp()
```

`quitApp()` is a **public slot** so `main.cpp` can reach it; do not move it back
to private. Never call Qt from inside the signal handler itself — only `write()`.

`quitApp()` calls `restoreParkedCpus()` first. Before this existed, *nothing*
brought CPUs back at exit — `unParkAll()` was reachable only from the Gaming
Mode tab — so even a clean quit stranded parked cores. `~MainWindow()` calls it
again as a backstop; the `getOfflineCpuSet().isEmpty()` check makes it a no-op.

### `isParked()` does not mean "this session parked them"

`GamingModeTab::detectTopology()` sets `m_parked = true` whenever **any** CPU is
offline at startup, adopting cores stranded by a previous crash and emitting
`gamingModeChanged(true, …)`. So `isParked()` is the app's *ownership* claim,
not a record of what it parked. `restoreParkedCpus()` gates on it deliberately:
that is what makes an unclean shutdown self-heal on the next run. The accepted
cost is that a CPU offlined by other means is adopted and restored on exit.

Verified 2026-09-01: 4 CPUs offlined externally → app launched → `SIGTERM` →
all 32 back online. With nothing offline the helper is not invoked at all.

---

## Crash detection / safe mode (v1.3.2)

`src/runstate.{h,cpp}` — marker at `~/.local/state/process-lasso/runstate.json`
(XDG **state**, not config).

### Two ordering rules, both load-bearing

1. **Re-arm before applying config.** `RunState::beginSession()` runs in the
   `MainWindow` ctor immediately after `Config::load()` and *before* the monitor
   starts. Everything between reading the marker and re-arming it is an
   unprotected window: if applying the config is what kills the process, a
   marker cleared *afterwards* would still read "clean" and the next run would
   load the same config again. That is the stale-marker loop this exists to stop.
2. **Mark clean last.** `RunState::markClean()` is the final act of `quitApp()`,
   after `restoreParkedCpus()` and `saveConfig()`. Recording "clean" before the
   hardware is actually released writes a lie the next run will trust.

### boot_id is what makes it correct

`beginSession()` compares the stored boot id against
`/proc/sys/kernel/random/boot_id`:

- armed + **same** boot → crashed this boot; parked CPUs are stale and real →
  `MainWindow::recoverFromUncleanShutdown()` unparks them.
- armed + **different** boot → crashed, but rebooted since. CPU online state is
  kernel runtime state that a reboot resets, so there is **nothing to repair**.

Without that check the power-loss path runs a pointless "recovery". A missing
marker is a first run, **not** a crash — do not regress that, or every fresh
install looks broken.

### Safe mode

3 consecutive unclean starts → `ProcessMonitor::setSafeMode(true)`, gating three
places: `applyNewPid()` (early return after `captureOriginal`), the
rule-enforcement block in `run()`, and the ProBalance tick.

**Rules are still loaded into `RuleEngine`** in safe mode. That is deliberate:
`saveConfig()` does `m_config["rules"] = m_ruleEngine.toJsonArray()`, so *not*
loading them would silently erase the user's rules on the next save. Safe mode
suppresses application, never the config itself.

The counter resets only after `HEALTHY_UPTIME_MS` (60 s) of uptime, and **never
while safe mode is active** — safe mode is sticky until the user presses Resume
Normal. Both deliberate: a loop that dies after ten seconds would otherwise
reset the counter every time and never trip the protection.

### Durability

`.tmp` → `fsync(file)` → POSIX `rename()` → `fsync(dir)`. Cesar's `/home` is
btrfs mounted `commit=120`, so without the fsyncs a write can sit two minutes
before reaching disk — useless for a marker meant to survive a power cut. POSIX
`rename()` is used directly rather than `QFile::rename()`, which refuses an
existing target and would leave a window with no marker at all.

### Verified 2026-09-01

First run not treated as a crash; clean restart holds the counter at 0; three
SIGKILLs climb 0→1→2 and trip safe mode on the 4th launch; `sameBoot=0` leaves
parked CPUs alone while `sameBoot=1` unparks them at startup; a probe process
started with `taskset -c 0-31` is narrowed to the configured `0-15` in normal
mode and left at `0-31` in safe mode, with `config.json` unchanged.

**Testing note:** on its first scan every existing PID counts as "new", so a
`default_affinity` in a test config is applied to *every process on the
machine*, desktop session included. Use a throwaway `HOME` **and** a harmless
value, and restore with `taskset -acp <full-set> <pid>` afterwards.

---

## Parking vs affinity (v1.3.3)

CPU parking and affinity assignment interact badly in two places. Both were
measured on real hardware, not reasoned about.

### The kernel is fine — the app was not

`sched_setaffinity` stores the *requested* mask and restores it when CPUs come
back online. Measured: request `24-27` → park all four → kernel forces
`0-23,28-31` → unpark → **back to `24-27`**. A process with no restriction
(`0-31`) round-trips the same way.

### 1. captureOriginal() must not run while CPUs are parked

`sched_getaffinity` returns the *already truncated* mask while CPUs are offline.
Measured: a process whose true original is `0-31` reads back as `0-23,28-31`
with 24-27 parked. `resetAllAffinities()` then writes captured masks back with
an explicit `sched_setaffinity` — which the kernel treats as a **new user
request**, pinning the process off those cores permanently and defeating its own
restore. `captureOriginal()` now returns early when `m_cpusParked`.

`m_cpusParked` is refreshed **once per monitor loop**, not per PID — the naive
version re-read `/sys` for every process on the first scan (~400 reads).
Deferring is safe: an uncaptured pid falls through to the "all CPUs" branch in
`resetAllAffinities()`, which is what the kernel would do anyway.

### 2. Affinity failures were silent

The success log lived *inside* `if (Utils::setAffinity(...))`, so a rule whose
CPUs were all parked did nothing and said nothing. Now reported — but **only for
the parked case**.

That restriction matters. `setAffinity()` also returns false on ESRCH when a
short-lived process exits between the snapshot and the syscall, which is
constant and unactionable; an early version logged it 126 times in one run.
Only "every requested CPU is parked" reaches the user log. Everything else is
`VLOG` only.

Dedupe is keyed on the **requested cpulist**, not on the parked set, and re-armed
by the success branch. Keying on the parked set re-fired on every intermediate
state while CPUs were unparked one at a time — the monitor thread observes that
in progress (measured: 5 warnings for one unpark of 4 CPUs).

### 3. AffinityDialog shows parked CPUs in red, and warns

Parked CPUs used to be `setEnabled(false)`. They are now **selectable** and
styled red (`#f38ba8`) with an explanatory tooltip, because parking is transient
while a rule or default affinity is persistent config that may legitimately be
authored during Gaming Mode.

`validateAndAccept()` warns on OK when the selection intersects the parked set:
Save/Cancel (defaulting to Cancel) when *every* selected CPU is parked, Yes/Cancel
when only some are. Selecting **all** CPUs skips the warning — that is equivalent
to no restriction at all and is harmless.

### Testing this safely — read before writing a test config

- A `default_affinity` in a test config is applied to **every process on the
  machine**: on the first scan `m_knownPids` is empty, so every existing PID
  counts as new. It hit 171 processes once and 140 another time — the whole
  Plasma session, editors, browsers, Steam. Prefer a **rule** with a narrow
  pattern; if a default really is needed, restore afterwards with
  `taskset -acp 0-31 <pid>`.
- Cesar's own rules pin brave/firefox/"Isolated Web Co" to `16-31` and chromium
  to `8,16-31` (E-cores on the 14900K: P-cores are 0-15, E-cores 16-31). **Do
  not "restore" those** — check `~/.config/process-lasso/config.json` before
  mass-resetting anything.
- `[ -s /proc/<pid>/cmdline ]` is always false — proc files report size 0, the
  same gotcha as `QFile::atEnd()` below. A shell scan using it silently matches
  nothing and looks like a clean result. Read the content and test for emptiness
  instead.

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

### install-helper.sh — fixed bug (do not regress)

`CpuPark::installHelper()` calls:
```
pkexec bash /path/to/install-helper.sh
```
The script resolves the helper binary **relative to its own location**:
```bash
HELPER_SRC="$(cd "$(dirname "$0")/.." && pwd)/bin/process-lasso-helper"
```
This works for both system installs (`/usr/share/…` → `../bin/…` = `/usr/bin/…`) and
AppImage mounts (`$APPDIR/usr/share/…` → `$APPDIR/usr/bin/…`).

**Do not** restore the old `${1:-...}` form. The C++ caller used to pass a username
as `$1`; the script incorrectly used it as the helper source path, causing a silent
install failure (`[[ ! -f "cesarin" ]]`).

### CpuPark::installHelper() — AppImage path fallback

`installHelper()` in `cpupark.cpp` searches for `install-helper.sh` in two places:

1. `QStandardPaths::locate(AppDataLocation, "install-helper.sh")` — standard XDG
   locations; works for system and user installs.
2. `QCoreApplication::applicationDirPath() + "/../share/process-lasso-qt/install-helper.sh"`
   — fallback for AppImage and portable builds where `QStandardPaths` does not
   search inside `$APPDIR`.

**Both** must remain in place. Do not remove the fallback.

---

## Config schema (config.json)

Stored at: `~/.config/process-lasso-qt/config.json`  
Loaded by: `Config::load()` → deep-merged with `Config::defaultConfig()`  
Saved by: `Config::save()` — atomic write (`.tmp` + rename)

```jsonc
{
  "show_temperatures": true,          // top-level; see Temperature monitoring
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

## Single-instance enforcement

`main.cpp` uses `QLocalServer`/`QLocalSocket` (Qt6::Network, linked in CMakeLists.txt).

- Socket name: `process-lasso-qt-$USER` (UID fallback for headless environments).
- On startup, before `QApplication`, a `QLocalSocket` tries to connect with a 300 ms timeout.
- If connected → running instance found: write `"raise\n"`, flush, exit 0.
- If not connected → first instance: call `QLocalServer::removeServer()` (clears stale socket from a crash), then `server.listen()`.
- Server's `newConnection` signal raises the window: `win.show(); win.raise(); win.activateWindow()`.
- `QLocalServer` is stack-allocated in `main()`, lives for the duration of `app.exec()`.

**Do not** move the single-instance check after `QApplication` construction — the socket probe
works without a window system connection and avoids flickering a window before exiting.

---

## ProBalance per-process exemptions (v1.2.0)

Two exemption paths are merged in `ProcessMonitor::run()` before each `ProBalance::tick()` call:

### 1. Manual per-PID (Processes tab context menu)
- Right-click → "Exempt from ProBalance" / "Remove ProBalance Exemption"
- Calls `ProcessMonitor::setPbExempt(pid, bool)` (locks `m_configMux`).
- Stored in `m_pbManualExempt` (`QSet<int>`); read back via `pbManualExempt()`.
- Session-only — not persisted (PIDs are ephemeral).
- `ProcessTableWidget::updatePbExempt(QSet<int>)` keeps the table in sync; teal row colour + "⚡ PB Exempt" status.

### 2. Rule-based (Rules tab → "ProBalance: Exempt matching processes from ProBalance")
- `Rule::pbExempt` (`std::optional<bool>`, JSON key `"pb_exempt"`).
- `RuleEngine::isPbExempt(procName)` — iterates enabled rules with `pbExempt=true`, returns true on first match.
- Applied by name to every process in the snapshot; converts to PIDs before the tick.
- Persisted in `config.json` with the rule.

### Merge in run()
```cpp
QSet<int> pbExempt;
{ QMutexLocker lk(&m_configMux); pbExempt = m_pbManualExempt; }
for (const auto &proc : snapshot)
    if (m_ruleEngine->isPbExempt(proc.name)) pbExempt.insert(proc.pid);
m_proBalance->tick(snapshot, tickSec, pbExempt);
```

`ProBalance::tick()` signature: `void tick(const QList<ProcessInfo>&, double, const QSet<int>& exemptPids = {})`.
The existing name-pattern exemption (`exempt_patterns` in config) continues to work independently.

---

## Temperature monitoring (v1.3.0)

Toggle: **Settings → Appearance → "Show CPU and RAM temperatures"**, config key
`show_temperatures` (top-level bool, default `true`).

### src/sensors.{h,cpp}

`Sensors::read()` returns a `SensorSnapshot`:

```cpp
struct SensorSnapshot {
    bool                 hasPackage;   // CPU package / Tdie present
    double               packageC;
    QHash<int, double>   perCpu;       // logical CPU index → °C
    QList<SensorReading> memory;       // DIMM label + °C
    bool cpuMax(double &out) const;    // package, else hottest core
    bool memoryMax(double &out) const; // hottest DIMM
};
```

Recognised hwmon `name` values:

| Driver | Role | Labels consumed |
|--------|------|-----------------|
| `coretemp` | Intel CPU | `Package id N`, `Core N` |
| `k10temp`, `zenpower`, `zenpower3` | AMD CPU | `Tdie` (preferred), `Tctl` |
| `spd5118` | DDR5 on-DIMM | none — synthesised `DIMM 1`, `DIMM 2`, … |
| `jc42` | DDR3/DDR4 on-DIMM | none — same synthesis |

`Tccd*` and NVMe `Composite` are deliberately skipped. AMD parts expose no
per-core sensor, so `perCpu` is empty there and only the package line shows —
this is expected, not a bug.

**Caching — do not regress.** Sensor file paths are discovered once into a
static `g_sources` list, and logical-CPU → (package, core_id) into `g_cpuTopo`.
The pre-1.3.0 code lived in `CpuBarsWidget::readTemps()` and re-opened
`/sys/devices/system/cpu/cpuN/topology/core_id` for *every* sensor × *every*
CPU on *every* refresh (~800 file opens per tick on a 14900K), on the **GUI
thread**. Discovery re-runs automatically only when a cached path fails to open
(module unloaded / device removed).

Core temperatures are keyed on `(physical_package_id, core_id)`, not `core_id`
alone — core ids repeat across sockets, and both SMT siblings of a core must map
to that core's reading.

### Threading

`Sensors::read()` runs on the **monitor thread**, inside the same
`display_refresh_interval_ms` block that emits `cpuSnapshotReady`, and is skipped
entirely when `show_temperatures` is false (no hwmon I/O at all when off).

```
ProcessMonitor::sensorsReady(SensorSnapshot) → MainWindow::onSensors()
    → CpuBarsWidget::setTemps(perCpu)
    → m_tempStatus (status-bar permanent widget) rich-text summary
```

`SettingsTab`'s constructor calls `Sensors::available()` to grey out the
checkbox on machines with no supported sensor. That is the **only** GUI-thread
call into `Sensors`, and it happens before `startMonitor()` — keep it that way,
the caches have no mutex.

### Display

- **Per-core**: `°C` painted under `Core N` in the bar's 52 px label zone
  (9 px monospace, coloured by `temperatureColor()`), leaving the existing GHz
  sub-line on the right untouched.
- **Status bar**: `CPU 93°C · RAM 52°C` as a *permanent* widget, so
  `onSnapshot()`'s `showMessage("N processes")` cannot overwrite it. Tooltip
  breaks out each DIMM.
- **Tray tooltip**: appends `· 93°C` when a CPU temperature is known.
- The pre-existing orange heat *tint* on the bar fill above 40 °C is unchanged
  and is **not** gated by the toggle — only the numeric readouts are.

`temperatureColor(double)` (declared in `gui/cpubarwidget.h`) is the shared
absolute-temperature ramp: blue 40 → green 60 → yellow 75 → peach 88 → red 100.
Distinct from `barColor(pct)`, which ramps on CPU *load*.

---

## Qt6-specific gotchas (already fixed, do not regress)

| Issue | Location | Fix applied |
|-------|----------|-------------|
| `QVariant::operator<` removed | processtablewidget.cpp | Explicit typed switch in sort lambda |
| `qAsConst` deprecated (Qt 6.6+) | processmonitor.cpp | Use `std::as_const` |
| `QStandardPaths::DataLocation` renamed | cpupark.cpp | Use `AppDataLocation` |
| `installHelper()` script not found in AppImage | cpupark.cpp | `applicationDirPath()` fallback added; `#include <QCoreApplication>` required |
| `install-helper.sh` silent failure | packaging/install-helper.sh | `$1` was username, not path; script now always uses `dirname "$0"/../bin/…` |
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

## AppImage packaging

```bash
cd process-lasso-qt
bash packaging/build-appimage.sh
# Outputs: process-lasso-qt-1.3.0-x86_64.AppImage  (~68 MB)
#          process-lasso-qt-1.3.0-x86_64.AppImage.zsync  (~238 KB)
```

`packaging/build-appimage.sh` is a self-contained build script:

1. **Downloads tools** into `packaging/appimage-tools/` (skips if already cached):
   - `linuxdeploy-x86_64.AppImage`
   - `linuxdeploy-plugin-qt-x86_64.AppImage`
   - `appimagetool-x86_64.AppImage`

2. **Patches bundled `strip`** — both linuxdeploy and its Qt plugin bundle an old
   `strip` binary that fails on `.relr.dyn` ELF sections (RELR relocations used by
   modern Arch/CachyOS glibc). The script extracts each AppImage once into
   `packaging/appimage-tools/linuxdeploy-unpacked/` and
   `packaging/appimage-tools/linuxdeploy-plugin-qt-unpacked/`, then replaces
   the bundled `strip` with `/usr/bin/strip` from the host.  
   **Re-extraction is triggered automatically if the AppImage is newer than the
   unpacked directory.**

3. **Release build** via `cmake -B build-appimage` (separate from `build/`).

4. **Populates `AppDir/`** via `DESTDIR=AppDir cmake --install build-appimage`.
   CMake installs both binaries, the `.desktop` file, and `install-helper.sh`
   into the correct `usr/…` hierarchy.

5. **Bundles Qt** using `linuxdeploy-unpacked/AppRun` with `--plugin qt`.
   The Qt plugin copies platform plugins (XCB), image format plugins, input
   context plugins, and all shared-library dependencies. It also writes a
   `qt.conf` next to the binary so Qt finds its plugins at runtime.

6. **Patches AppRun** to prepend `$APPDIR/usr/share` to `XDG_DATA_DIRS` so
   `QStandardPaths::AppDataLocation` finds `install-helper.sh` inside the
   AppImage mount (belt-and-suspenders alongside the `applicationDirPath()`
   fallback in `cpupark.cpp`).

7. **Packages** with `appimagetool --comp zstd --updateinformation <gh-releases-zsync URL>`
   → Type 2 squashfs AppImage with embedded update metadata.  
   `appimagetool` then calls `zsyncmake` automatically to produce a companion
   `.zsync` file. Upload **both** `*.AppImage` and `*.AppImage.zsync` to every
   GitHub release so Gear Lever / AppImageUpdate can perform delta updates.

### AppImage auto-update (Gear Lever / AppImageUpdate)

The embedded update information string:
```
gh-releases-zsync|Tamalero|Process-lasso-linux-inC|latest|process-lasso-qt-*-x86_64.AppImage.zsync
```
This tells any AppImage-aware update client to:
- Query the GitHub releases API for the latest release of the repo
- Download the `.zsync` file from that release
- Apply delta (rsync-style) patching — only changed blocks are downloaded

**Requirement**: `zsync` (`pacman -S zsync`) must be installed on the host to run
the build script. `appimagetool` bundles `zsyncmake` internally as a fallback,
but system `zsyncmake` takes precedence when on `PATH`.

**Build artifacts excluded from git** (`.gitignore`):
`AppDir/`, `build-appimage/`, `*.AppImage`, `*.AppImage.zsync`, `packaging/appimage-tools/`

**AppImage runtime dependencies** (everything else is bundled):
- FUSE 2 / FUSE 3 compat (`fuse2` on Arch) to mount the squashfs
- `polkit` (`pkexec`) for the one-time helper installation
- `sudo` with NOPASSWD for `process-lasso-helper` at runtime

---

## Release procedure

GitHub repo: **`Tamalero/Process-lasso-linux-inC`** (note: the `url=` in
`packaging/PKGBUILD` still says `acorninteractive/process-lasso-qt` — that is
wrong and unused by the build). Releases land **directly on `main`**; there is
no PR flow on this repo.

1. Bump the version in **`CMakeLists.txt`** — `build-appimage.sh` greps it from
   there, so that single line drives the artifact filenames. Also bump
   `packaging/PKGBUILD` `pkgver` and the "Current version" line at the top of
   this file.
2. Update `README.md` (feature table, config schema, relevant section) and this
   file. Check the AppImage section above for stale example filenames.
3. `git commit` on `main`, `git push origin main`.
4. `bash packaging/build-appimage.sh`
5. `gh release create vX.Y.Z <AppImage> <AppImage>.zsync --target main --title … --notes …`

**Upload BOTH artifacts.** The `.zsync` is what Gear Lever / AppImageUpdate use
for delta updates; the embedded update-information glob
(`process-lasso-qt-*-x86_64.AppImage.zsync`) resolves against the *latest*
release's assets, so a release missing its `.zsync` silently breaks auto-update
for everyone.

Verify before publishing:
```bash
./process-lasso-qt-X.Y.Z-x86_64.AppImage --appimage-updateinformation
strings -el AppDir/usr/bin/process-lasso-qt | grep '<a new UI string>'
```
`strings` without `-el` will **not** find Qt UI text — `QStringLiteral` stores
UTF-16, so plain ASCII `strings` finds only `QLatin1String` comparison literals.

### Stale CMake caches (recurring trap)

This project was moved from `Personal/ProcessLasso/` to
`Personal/Utilities/ProcessLasso/`. Every build directory created before the
move has the old absolute path baked into `CMakeCache.txt` and fails with
*"does not match the source … used to generate cache"*.

`build/` is still in that state. `build-appimage/` and `AppDir/` were renamed
aside as `*.stale-<timestamp>` during the 1.3.0 release and rebuilt clean.
When you hit this, **rename the directory aside** (never `rm -rf` — see the
workspace deletion rule) and let CMake regenerate.

---

---

## CPU graph widget design notes

### CpuBarsWidget (per-core bars)
- Each bar has a **left label zone** (`labelW = 52 px`) painted with `"Core N"` left-aligned.
- Bar fill starts at `x + labelW + 1`; percentage right-aligned in remaining space; frequency
  sub-line at bottom (`"%1 GHz"` format, 9 px font).
- Column count calculated by `cols()` using `w / 120` as max-columns divisor; minimum bar
  width floor is `110 px` (both in `paintEvent` and `barIndexAt`).
- Layout stretch in `cpuRow` QHBoxLayout: history column = **1**, bars column = **3**
  (25 % / 75 % split).

### CpuHistoryWidget (avg history graph)
- Vertical size policy is `Expanding` so it fills the full left-column height set by the
  taller bars column — no external QLabel above it.
- `"CPU History (avg)"` text is drawn as an **in-widget overlay** (top-left, 10 px monospace,
  alpha 180) rather than as a separate QLabel in the layout.
- `paintEvent` draws background + border unconditionally; graph path only when `n >= 2`.
- **Do not** add `setFixedHeight()` back — it breaks the full-height fill.

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

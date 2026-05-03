# Process Lasso Qt

A native C++17/Qt6 process manager for Arch Linux and CachyOS, inspired by the Windows application *Process Lasso*. Originally forked from [franzjeger/process-lasso-linux](https://github.com/franzjeger/process-lasso-linux) and fully rewritten without Python — no psutil, no PyQt6, no subprocess calls. Everything runs as compiled native code using direct Linux kernel interfaces.

---

## Table of Contents

1. [Feature Overview](#feature-overview)
2. [Requirements](#requirements)
3. [Building](#building)
4. [Installation](#installation)
5. [Privileged Helper](#privileged-helper)
6. [Configuration File](#configuration-file)
7. [Tabs Reference](#tabs-reference)
   - [Processes](#processes-tab)
   - [Rules](#rules-tab)
   - [ProBalance](#probalance-tab)
   - [Gaming Mode](#gaming-mode-tab)
   - [Settings](#settings-tab)
   - [Log](#log-tab)
8. [System Tray](#system-tray)
9. [Rules Engine Deep Dive](#rules-engine-deep-dive)
10. [ProBalance Deep Dive](#probalance-deep-dive)
11. [Gaming Mode Deep Dive](#gaming-mode-deep-dive)
12. [CPU Topology Detection](#cpu-topology-detection)
13. [How CPU% is Measured](#how-cpu-is-measured)
14. [Preset Rules](#preset-rules)
15. [Keyboard Shortcuts](#keyboard-shortcuts)
16. [Architecture Notes](#architecture-notes)
17. [Known Limitations](#known-limitations)

---

## Feature Overview

| Feature | Description |
|---|---|
| Live process table | PID, name, CPU%, RSS memory, nice, CPU affinity, I/O class, ProBalance status |
| Per-process CPU affinity | Topology-aware checkbox picker; `sched_setaffinity` on all TIDs |
| Per-process nice priority | Full -20 to 19 range; presets for High / Normal / Low / Idle |
| Per-process I/O priority | `ioprio_set` syscall; classes None / Realtime / Best-effort / Idle |
| Rules Engine | Pattern-based automation (contains / exact / regex); applies affinity + nice + I/O on process start |
| ProBalance | Automatic CPU throttle: raises nice when a process monopolises the CPU; restores on cool-down |
| Gaming Mode | Parks non-preferred CPUs offline; AMD X3D and Intel Hybrid topology aware |
| Game Launcher | Integrated Steam and Lutris game picker; `/proc`-based game watcher; auto-restore on exit |
| Gaming Mode profiles | Named profiles saved to config; instant load/switch |
| Settings | Default affinity, poll intervals, dark/system theme, window opacity, systemd autostart |
| System tray | CPU load bar icon; show/hide; gaming mode toggle; quit |
| Dark theme | Catppuccin Mocha colour scheme applied at startup |
| Config persistence | Atomic JSON write to `~/.config/process-lasso/config.json` |

---

## Requirements

**Runtime:**

| Package | Purpose |
|---|---|
| `qt6-base` | Qt6 Widgets, Core, Gui |
| `polkit` | `pkexec` for privileged helper installation |
| `sudo` | Passwordless helper execution at runtime |
| `sqlite` *(optional)* | Lutris game library scanning (CLI `sqlite3` binary) |

**Build:**

| Package | Purpose |
|---|---|
| `cmake >= 3.20` | Build system |
| `ninja` *(recommended)* | Fast parallel builds |
| `gcc >= 13` or `clang >= 16` | C++17 support |
| `qt6-base` | Qt6 development headers |

Install on Arch / CachyOS:
```bash
sudo pacman -S qt6-base cmake ninja polkit sudo
# Optional: sudo pacman -S sqlite
```

---

## Building

```bash
git clone https://github.com/Tamalero/Process-lasso-linux-inC.git
cd Process-lasso-linux-inC

cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -GNinja
cmake --build build --parallel
```

The build produces two binaries inside `build/`:

| Binary | Description |
|---|---|
| `process-lasso-qt` | Main GUI application |
| `process-lasso-helper` | Small privileged helper (see below) |

---

## Installation

**Manual:**
```bash
sudo cmake --install build
```
This installs:
- `/usr/bin/process-lasso-qt`
- `/usr/bin/process-lasso-helper`
- `/usr/share/applications/process-lasso.desktop`
- `/usr/share/process-lasso-qt/install-helper.sh`

**AUR / CachyOS package** (using the included PKGBUILD):
```bash
cd packaging
makepkg -si
```

---

## Privileged Helper

Several operations require root privileges:

- Taking CPUs offline / online (`/sys/devices/system/cpu/cpuN/online`)
- Setting negative nice values (below 0) via `setpriority(PRIO_PROCESS, ...)`

These are handled by a small compiled C++ binary (`process-lasso-helper`) that validates all arguments strictly before acting.

**Installation** (one-time, from the Gaming Mode tab):

1. Open the *Gaming Mode* tab.
2. Click **Install / Update Helper (root)**.
3. A `pkexec` password prompt appears; enter your password.
4. The script copies the helper to `/usr/local/bin/process-lasso-helper` and writes a sudoers rule to `/etc/sudoers.d/process-lasso` so the helper can be called without a password at runtime.

**Accepted commands:**

| Command | Effect |
|---|---|
| `cpu-online N 0` | Take CPU *N* offline |
| `cpu-online N 1` | Bring CPU *N* online |
| `cpu-unpark-all` | Bring all offline CPUs back online |
| `renice-pid NICE PID` | Call `setpriority(PRIO_PROCESS, PID, NICE)` |
| `--check-only` | No-op; returns 0 to confirm the helper is accessible |

All inputs are validated; the helper refuses unknown commands and rejects CPU 0 for the `cpu-online 0` offline request (bootstrap CPU protection).

---

## Configuration File

Path: `~/.config/process-lasso/config.json`

The file is written atomically (write to `.tmp`, then rename) to prevent corruption on crash. Missing keys are filled from built-in defaults at load time via a recursive deep-merge.

**Top-level structure:**

```json
{
  "version": 1,
  "rules": [],
  "cpu": {
    "default_affinity": null,
    "apply_default_affinity": false
  },
  "probalance": {
    "enabled": true,
    "cpu_threshold_percent": 85.0,
    "consecutive_seconds": 3,
    "nice_adjustment": 10,
    "nice_floor": 15,
    "restore_threshold_percent": 40.0,
    "restore_hysteresis_seconds": 5,
    "exempt_patterns": ["kwin", "plasmashell", "systemd", "kthreadd", "Xorg", "xwayland"]
  },
  "monitor": {
    "display_refresh_interval_ms": 2000,
    "rule_enforce_interval_ms": 500
  },
  "ui": {
    "start_minimized": false,
    "use_system_theme": false,
    "opacity": 100
  },
  "gaming_mode": {
    "profiles": {}
  }
}
```

---

## Tabs Reference

### Processes Tab

The main live view of all running processes.

**Columns:**

| Column | Content | Notes |
|---|---|---|
| PID | Process ID | |
| Name | Process name | Wine/Proton `.exe` names resolved; 15-char kernel truncation corrected via cmdline |
| CPU% | Per-process CPU usage | Calculated from `/proc/[pid]/stat` jiffie deltas |
| Mem(MB) | RSS memory in megabytes | From `/proc/[pid]/stat` field 24 (pages × page size) |
| Nice | Current nice priority | -20 (highest) to 19 (lowest) |
| Affinity | Active CPU affinity mask | Displayed as cpulist, e.g. `0-7,16-23` |
| I/O | I/O priority class | e.g. `be/4` (best-effort, level 4) |
| Status | ProBalance state | Shows `⏸ Throttled` when ProBalance has raised nice |

**Sorting:** Click any column header to sort ascending; click again for descending. The active sort column shows a ▲ or ▼ indicator.

**Filtering:** Type in the *Filter* box above the table to show only rows whose name or PID contains the search text (case-insensitive).

**Row colour coding:**

| Colour | Meaning |
|---|---|
| Orange | ProBalance has throttled this process |
| Red | CPU% ≥ 80% |
| Yellow | CPU% ≥ 40% |
| Green | CPU% ≥ 10% |
| Default | CPU% < 10% |

**Column visibility:** Right-click any column header to show/hide individual columns.

**Context menu** (right-click on a process row):

| Action | Effect |
|---|---|
| Kill *name* (PID) | Send SIGTERM |
| Force Kill *name* (PID) | Send SIGKILL |
| Kill *N* selected | SIGTERM to all selected (with confirmation dialog) |
| Force Kill *N* selected | SIGKILL to all selected (with confirmation dialog) |
| Set Affinity for *name*… | Opens topology-aware AffinityDialog |
| Set Priority (nice) for *name*… | Opens NicePriorityDialog |
| Set I/O Priority for *name*… | Opens IoNiceDialog |
| Add Rule for '*name*'… | Opens RuleEditDialog pre-filled with process name |

**Keyboard shortcut:** `Delete` or `Backspace` on a selected row kills the selected process(es) (SIGTERM, no confirmation for single kills).

**Manual affinity protection:** When you manually change a process's affinity from the context menu, the monitor suppresses rule re-enforcement for that PID for 30 seconds, so your manual change is not immediately overwritten.

---

### Rules Tab

Create and manage persistent per-process rules that are automatically applied whenever a matching process is seen.

**Table columns:** Enabled, Name, Pattern, Match Type, Affinity, Nice, I/O Class, I/O Level.

**Buttons:**

| Button | Effect |
|---|---|
| Add Rule | Opens empty RuleEditDialog |
| Templates… | Opens the 13-preset template picker |
| Edit | Opens RuleEditDialog for the selected rule |
| Delete | Deletes the selected rule (with confirmation) |
| Enable/Disable | Toggles the enabled flag on the selected rule |
| Export… | Saves all rules to a `.json` file |
| Import… | Imports rules from a `.json` file (merges; does not replace) |

**Rule fields:**

| Field | Type | Description |
|---|---|---|
| Name | Text | Display label (free text) |
| Pattern | Text | Matched against the process name |
| Match type | Enum | `contains` — substring match; `exact` — full equality; `regex` — Qt QRegularExpression |
| CPU Affinity | Optional cpulist | e.g. `0-7` or `0,2,4,6` — applied via `sched_setaffinity` to all TIDs |
| Nice | Optional int (-20 to 19) | Applied via `setpriority(PRIO_PROCESS, ...)` |
| I/O Class | Optional int (0-3) | 0=None, 1=Realtime (root required), 2=Best-effort, 3=Idle |
| I/O Level | Optional int (0-7) | Only active for classes 1 and 2; 0=highest, 7=lowest |
| Enabled | Bool | Disabled rules are stored but never applied |

**Process picker:** Click *Select from running processes…* in RuleEditDialog to choose a process from a live `/proc` snapshot, which pre-fills the pattern and affinity fields.

Rules are applied once when a new PID is detected and re-applied on the configured enforcement interval (default 500 ms). Rules saved to `config.json` persist across restarts.

---

### ProBalance Tab

Automatic CPU throttling that prevents one runaway process from starving the rest of the system.

**How it works:**

1. Every 1 second the monitor calls `ProBalance::tick()` with the current process snapshot.
2. For each process, if its CPU% stays above the **CPU threshold** for at least **consecutive seconds**, ProBalance raises the process's nice value by **nice adjustment** (capped at **nice floor**).
3. Once the system-wide average CPU drops below **restore threshold** and stays there for **restore hysteresis seconds**, the process's original nice value is restored.

**Configuration fields:**

| Field | Default | Description |
|---|---|---|
| ProBalance Enabled | Yes | Master on/off switch |
| CPU threshold | 85% | Per-process CPU% that triggers throttling |
| Consecutive seconds above threshold | 3 s | How long the process must exceed the threshold |
| Nice adjustment (added on throttle) | 10 | Added to the process's current nice value |
| Nice floor (max nice applied) | 15 | The nice value is never raised above this |
| Restore when CPU below | 40% | System-wide CPU below which restoration begins |
| Restore hysteresis (seconds below restore threshold) | 5 s | How long CPU must stay low before restoring |
| Exempt Processes (pattern contains) | kwin, plasmashell, systemd, kthreadd, Xorg, xwayland | Processes that are never throttled |

Click **Apply Settings** to save and propagate changes to the running monitor.

Throttled processes are shown in orange in the Processes tab with the label `⏸ Throttled` in the Status column.

---

### Gaming Mode Tab

Optimises the system for a single game by concentrating the OS scheduler on the highest-performance CPU cores and optionally elevating the game's scheduling priority.

#### CPU Topology

At startup the tab detects the CPU topology:

| Topology | Detection method | Preferred cores | Non-preferred cores |
|---|---|---|---|
| **AMD X3D** (e.g. 7950X3D) | Compares L3 cache size across CCDs via `/sys/devices/system/cpu/cpuN/cache/index3/size`; larger = V-Cache CCD | V-Cache CCD (higher L3) | Non-V-Cache CCD |
| **Intel Hybrid** (e.g. 12th–14th gen) | Compares `cpuinfo_max_freq` across cores; cores with freq ≥ 80% of maximum = P-cores | P-cores | E-cores |
| **Uniform** | All cores identical | — | Parking disabled |

SMT siblings (hyperthreads) are detected via `/sys/devices/system/cpu/cpuN/topology/core_id`.

#### Core Selection

On asymmetric topologies the preferred-CCD cores are shown as a checkbox grid. Each CPU has a checkbox; hyperthreads are labelled `(HT)`.

Quick-select buttons:
- **All** — enable all preferred-CCD CPUs
- **No SMT (physical only)** — disable hyperthread siblings
- **None** — disable all (park everything except the toggle engine)

#### Enable/Disable Gaming Mode

Clicking **▶ Enable Gaming Mode** runs a background thread that:
1. Collects all non-preferred CPUs plus any manually unchecked preferred CPUs.
2. Calls `process-lasso-helper cpu-online N 0` for each (skipping CPU 0).
3. Emits `gamingModeChanged(true, elevateNice)` to the monitor thread.

If **Elevate game priority (nice -1)** is checked, the monitor will apply `nice -1` to every new process while gaming mode is active, via `process-lasso-helper renice-pid -1 <PID>`.

Clicking **⏹ Disable Gaming Mode** calls `process-lasso-helper cpu-unpark-all` and restores the nice values of any processes that were elevated.

The button and CPU status label update in real time. If gaming mode was active when the app starts (CPUs are already offline), this is detected automatically via `/sys/devices/system/cpu/offline`.

#### Reset All Changes

The **↩ Reset All Changes** button:
1. Unparks any parked CPUs.
2. Tells the monitor thread to restore all recorded original CPU affinities.

#### Game Launcher

| Field | Description |
|---|---|
| Game (name) | Name used to identify the game process in `/proc` |
| Command | Full launch command, e.g. `steam -applaunch 238960` |
| Steam… | Opens SteamGamePickerDialog — scans `~/.steam/steam/steamapps/*.acf` manifests and all library paths from `libraryfolders.vdf`; fills in game name and `steam -applaunch <appid>` |
| Lutris… | Opens LutrisGamePickerDialog — queries `~/.local/share/lutris/pga.db` via the `sqlite3` CLI; fills in game name and `lutris lutris:rungame/<slug>` |
| ▶ Launch | Enables Gaming Mode (if not already active), launches the command via `QProcess`, then begins polling `/proc` every 2 s for the game's PID |
| ⏹ Kill Game | Sends SIGTERM to the detected game PID |
| Auto-disable Gaming Mode when game exits | When checked, disables Gaming Mode and unparks CPUs automatically after the game process disappears |

**Game process detection:** The launcher strips non-alphanumeric characters from both the game name and each candidate process's `comm`/`cmdline` before comparing, allowing fuzzy matching across launchers and Wine.

**PID tracking:** Once the game PID is found, the poll interval increases to 5 s. If the PID disappears but another process matching the name appears (common with launchers that re-exec), the new PID is adopted without stopping Gaming Mode.

#### Profiles

A profile stores: game name, launch command, per-CPU checkbox states, and the elevate-nice setting.

| Button | Effect |
|---|---|
| Save | Prompts for a name and writes the current state to `config.json` |
| Delete | Removes the currently selected profile |
| Combo box change | Loads the selected profile immediately; if Gaming Mode is active, re-applies CPU parking with the new core selection |

---

### Settings Tab

#### Default Process Affinity

| Control | Description |
|---|---|
| Apply default affinity to new processes | When enabled, every new PID that does not match any rule receives the configured default affinity |
| CPU list | Cpulist string, e.g. `0-7` or `0,2,4,6` |
| Pick… | Opens AffinityDialog to choose visually |
| All | Clears the field (all CPUs = no restriction) |
| P-cores | Fills the field with the preferred CPU set detected by the topology engine |

#### Monitor Intervals

| Control | Default | Description |
|---|---|---|
| Rule enforcement interval | 500 ms | How often rules are checked against running processes |
| Display refresh interval | 2000 ms | How often the process table is updated |

#### Appearance

| Control | Default | Description |
|---|---|---|
| Follow system theme | Off | When checked, clears the built-in Catppuccin Mocha stylesheet and lets Qt use the desktop theme |
| Window opacity | 100% | Sets `QMainWindow::setWindowOpacity`; range 30–100% |

#### Autostart

When **Start with desktop session** is checked, the app writes a systemd user service file to `~/.config/systemd/user/process-lasso.service` and runs `systemctl --user enable process-lasso.service`. Unchecking disables and removes the service file.

---

### Log Tab

A scrollable text log of all actions taken by the monitor thread, ProBalance, the rules engine, gaming mode, and the launcher.

| Control | Description |
|---|---|
| Auto-scroll | When checked, the log scrolls to the newest entry automatically |
| Clear | Empties the log widget |

The log is capped at 2 000 lines. Each entry is prefixed with a `[HH:mm:ss]` timestamp.

Example log entries:
```
[14:22:01] [Rule] affinity=0-7 nice=5 → firefox(12345)
[14:22:04] [ProBalance] Throttling krita(67890): nice 0→10
[14:22:09] [ProBalance] Restoring krita(67890): nice 10→0
[14:23:15] [Gaming Mode] Parking CPUs…
[14:23:15] [Park] CPU 8 → offline
[14:23:16] [Gaming Mode] ACTIVE — non-preferred CPUs offline.
[14:25:40] [Launcher] Game process found: PID 77001
```

---

## System Tray

The application minimises to a system tray icon instead of exiting when the window is closed (as long as the desktop has a tray area).

**Icon:** A 22×22 vertical bar showing current average CPU usage. Colour: green < 40%, yellow ≤ 80%, red > 80%.

**Tooltip:** `Process Lasso Qt — CPU: X.X%` (updated every monitor tick).

**Left/double-click:** Toggle window visibility.

**Right-click menu:**

| Entry | Effect |
|---|---|
| Show / Hide | Toggle main window |
| Enable / Disable Gaming Mode | Toggle Gaming Mode without opening the window |
| Quit | Save config, stop monitor thread, exit cleanly |

---

## Rules Engine Deep Dive

Rules are stored as a JSON array in `config.json` and loaded into `RuleEngine` at startup. Each rule has a UUID (`ruleId`), so editing and deleting are stable across reorders.

**Match types:**

| Type | Behaviour |
|---|---|
| `contains` | `procName.contains(pattern, Qt::CaseInsensitive)` |
| `exact` | `procName == pattern` |
| `regex` | `QRegularExpression(pattern).match(procName).hasMatch()` |

**Application cadence:** The monitor thread applies rules to every new PID once on first detection. It then re-applies all rules to all known PIDs on every `rule_enforce_interval_ms` tick (default 500 ms), so rules are re-enforced if a process resets its own affinity. Rules are suppressed for a PID during a 30-second manual override window.

**Affinity application:** `sched_setaffinity` is called on the main thread *and* all TIDs found in `/proc/[pid]/task/`, ensuring the entire process (including worker threads already created) is migrated.

**Wine/Proton name resolution:** If the process's `/proc/[pid]/comm` is exactly 15 characters (kernel truncation limit), the cmdline is checked for a Windows path containing `\` and `.exe`. If found, the `.exe` basename is used as the process name, which allows rules using the game's `.exe` name to match correctly.

---

## ProBalance Deep Dive

ProBalance runs inside the monitor thread and is ticked every 1 second with the current snapshot.

**State machine per process:**

```
Normal  ──[ CPU > threshold for N consecutive seconds ]──▶  Throttled
                                                            (nice raised)

Throttled ──[ system avg CPU < restore_threshold for M seconds ]──▶  Normal
                                                                     (nice restored)
```

- The per-process `consecutiveHigh` and `consecutiveLow` counters accumulate wall-clock seconds between ticks, so accuracy is not affected by variable tick timing.
- The `nice_floor` cap ensures ProBalance cannot lower a process's scheduling priority below a configured maximum even through repeated triggering.
- Exempt patterns are checked as case-insensitive substrings of the process name. The default exempt list covers the KDE compositor, Plasma shell, systemd, and the X server.
- ProBalance does **not** apply to processes that are already managed by a Rule; the Rule takes precedence.

---

## Gaming Mode Deep Dive

### CPU Parking mechanism

CPU parking writes `0` to `/sys/devices/system/cpu/cpuN/online` via the privileged helper. This is a kernel hotplug operation — the CPU is fully taken offline at the scheduler level, not just de-prioritised. Games that enumerate CPU count at startup (common in multi-threaded engines) will therefore see only the online CPUs, preventing work from being spread across E-cores or the lower-L3 CCD.

CPU 0 is never parked (it is the bootstrap processor and many kernel subsystems assume it is always online).

### Nice elevation in Gaming Mode

When **Elevate game priority** is checked and Gaming Mode is active, every new process detected by the monitor gets `nice -1` applied via the helper. This raises its scheduling priority above normal user processes (`nice 0`) without requiring a kernel real-time policy. The original nice value is recorded and restored when Gaming Mode is disabled.

### Profile switching while active

If Gaming Mode is already active and you load a different profile, the tab:
1. Calls `disableGamingMode()` to unpark all CPUs.
2. Sets `m_pendingEnableAfterUnpark = true`.
3. When the unpark background thread completes, detects the flag and immediately calls `enableGamingMode()` with the new core selection, re-parking to the new configuration.

---

## CPU Topology Detection

Detection is cached in a module-level `std::optional` after the first call.

**AMD X3D detection** (`detectAmdX3D`):
- Reads `/sys/devices/system/cpu/cpuN/cache/index3/size` for each online CPU.
- Converts the size string (e.g. `"32M"` or `"96M"`) to bytes.
- Groups CPUs by L3 size; the group with the larger L3 is the V-Cache CCD (preferred).
- If all CCDs have the same L3 size (uniform), returns `std::nullopt`.

**Intel Hybrid detection** (`detectIntelHybrid`):
- Reads `cpuinfo_max_freq` for each online CPU from sysfs.
- Finds the global maximum frequency.
- CPUs with freq ≥ 80% of the maximum are P-cores (preferred); the rest are E-cores.
- If all CPUs have the same max frequency (uniform), returns `std::nullopt`.

**SMT sibling detection** (`getSmtSiblingsOf`):
- Reads `/sys/devices/system/cpu/cpuN/topology/core_id` for each CPU in the input set.
- Groups by `core_id`; the second (and later) logical CPUs per physical core are the SMT siblings.

---

## How CPU% is Measured

Each process's CPU% is computed from `/proc/[pid]/stat` without any kernel module or external library:

1. On each tick, read fields `utime` (field 14) and `stime` (field 15) from `/proc/[pid]/stat` (after the closing `)` of the comm field to handle process names with spaces).
2. Compute `deltaTicks = (utime + stime) - prevTicks`.
3. Compute `deltaWallNs` via `std::chrono::steady_clock`.
4. `cpuPercent = (deltaTicks / HZ) / (deltaWallNs / 1e9) * 100.0`, where `HZ = sysconf(_SC_CLK_TCK)`.

The per-CPU usage bars use the same principle against `/proc/stat` cpuN lines, tracking the `idle + iowait` delta against the total jiffies delta.

---

## Preset Rules

The *Templates…* button in the Rules tab offers 13 pre-built rules. These are designed for a typical AMD X3D system where CCD0 (CPUs 0-7, 16-23) is the V-Cache CCD and CCD1 (CPUs 8-15, 24-31) is the secondary CCD, but the affinity values can be edited before confirming.

| Preset | Pattern | Match | Affinity | Nice | I/O Class |
|---|---|---|---|---|---|
| Steam (CCD0) | steam | exact | 0-7,16-23 | — | — |
| steamwebhelper | steamwebhelper | exact | 0-7,16-23 | 5 | — |
| Wine / Proton | wine | contains | 0-7,16-23 | — | — |
| OBS Studio | obs | exact | 0-7,16-23 | -1 | — |
| Discord | discord | contains | 8-15,24-31 | 5 | — |
| Firefox | firefox | contains | 8-15,24-31 | — | — |
| Chromium / Chrome | chrom | contains | 8-15,24-31 | — | — |
| Brave | brave | contains | 8-15,24-31 | — | — |
| KWin | kwin | contains | — | — | — |
| Plasma Shell | plasmashell | exact | 8-15,24-31 | 5 | — |
| Compiler (gcc/clang) | gcc | contains | — | — | 2 (Best-effort) |
| Archive / compress | 7z | contains | 8-15,24-31 | 10 | 3 (Idle) |
| Background (nice 10) | *(empty)* | contains | — | 10 | — |

Each preset opens in the full RuleEditDialog so you can customise before saving.

---

## Keyboard Shortcuts

| Context | Key | Action |
|---|---|---|
| Processes table | `Delete` or `Backspace` | Kill selected process(es) (SIGTERM) |
| Any dialog | `Enter` | Accept |
| Any dialog | `Escape` | Cancel |

---

## Architecture Notes

```
main.cpp
  └─ MainWindow (QMainWindow)
       ├─ CpuHistoryWidget     — 120-sample rolling average area chart
       ├─ CpuBarsWidget        — Per-CPU usage bars with temperature tint + freq overlay
       ├─ ProcessTableWidget   — Live sortable/filterable process table
       ├─ RulesEditor          — Rule CRUD table + dialogs
       ├─ ProBalanceTab        — ProBalance config form
       ├─ GamingModeTab        — CPU parking + game launcher
       ├─ SettingsTab          — App settings
       └─ Log QTextEdit
            │
            ├─ ProcessMonitor (QThread) ── reads /proc every 100 ms
            │    ├─ RuleEngine           ── applied every 500 ms
            │    └─ ProBalance           ── ticked every 1 s
            │
            └─ Config namespace          ── load/save config.json
```

The `ProcessMonitor` thread never touches Qt GUI objects directly — it emits `processSnapshotReady` and `cpuSnapshotReady` signals which are dispatched to the GUI thread via `Qt::QueuedConnection`. Config updates from the GUI are passed to the monitor through a `QMutex`-protected copy.

CPU parking and helper operations run in transient `QThread` workers spun up on demand so they never block the GUI event loop.

---

## Known Limitations

- **Negative nice values** require the privileged helper to be installed. Without it, only nice values ≥ 0 can be set from the GUI. ProBalance uses positive nice values only and does not require root.
- **CPU parking** requires the helper. On systems without it, the Gaming Mode button is disabled for asymmetric topologies.
- **I/O Realtime class** (class 1) requires root; setting it via the GUI will silently fail without the helper providing elevated permissions.
- **Lutris game library** scanning requires the `sqlite3` CLI to be installed; there is no fallback.
- **Uniform CPU topology** (all cores identical): Gaming Mode CPU parking is disabled — there is no meaningful partition to create.
- **CPU% values** are approximate because Linux scheduler jiffies have limited resolution (typically 100 Hz). Very short-lived CPU spikes may be under-reported.
- Config is saved on every rule/settings change and on clean exit. An unclean shutdown (power loss, SIGKILL) after a gaming session may leave CPUs parked; run **Reset All Changes** or `echo 1 | sudo tee /sys/devices/system/cpu/cpuN/online` manually to recover.

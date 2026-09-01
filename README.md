# Process Lasso Qt

A native C++17/Qt6 process manager for Arch Linux and CachyOS, inspired by the Windows application *Process Lasso*. Originally forked from [franzjeger/process-lasso-linux](https://github.com/franzjeger/process-lasso-linux) and fully rewritten without Python — no psutil, no PyQt6, no subprocess calls. Everything runs as compiled native code using direct Linux kernel interfaces.

---

## What's New in 1.3.3

Rolls up 1.3.1 and 1.3.2, which were never published separately. Full history in
[CHANGELOG.md](CHANGELOG.md).

**Gaming Mode no longer strands your CPUs.** Parked CPUs were never brought back
when the app exited — `unParkAll()` was reachable only from the Gaming Mode tab,
so even a clean quit left them offline until you noticed. They are now restored
on every shutdown Process Lasso can observe, including `SIGTERM` / `SIGINT` /
`SIGHUP` (systemd stop, logout, Ctrl-C), which previously killed the process
outright without saving config.

**Crash detection with Safe Mode.** A boot-aware marker records whether the last
session shut down properly. After 3 unclean starts in a row, rules, default
affinity and ProBalance stop being applied and a banner offers **Resume Normal**
— your `config.json` is never modified, only its application is suppressed. The
marker knows the difference between "crashed just now, CPUs still parked" and
"crashed, but you have rebooted since and there is nothing left to fix".

**Parking and affinity no longer fight each other.** Original affinities are no
longer captured while CPUs are parked (which silently narrowed processes for
good), affinity failures are reported in the Log instead of failing silently,
and parked CPUs now show in **red** in the affinity picker — still selectable,
with a warning, since parking is temporary but a rule is permanent.

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
8. [System Tray & Companion Panel](#system-tray--companion-panel)
9. [CPU Usage Graphs](#cpu-usage-graphs)
10. [Temperature Monitoring](#temperature-monitoring)
11. [Crash Detection & Safe Mode](#crash-detection--safe-mode)
12. [Rules Engine Deep Dive](#rules-engine-deep-dive)
13. [ProBalance Deep Dive](#probalance-deep-dive)
14. [Gaming Mode Deep Dive](#gaming-mode-deep-dive)
15. [CPU Topology Detection](#cpu-topology-detection)
16. [How CPU% is Measured](#how-cpu-is-measured)
17. [Preset Rules](#preset-rules)
18. [Single-Instance Behaviour](#single-instance-behaviour)
19. [Keyboard Shortcuts](#keyboard-shortcuts)
20. [Architecture Notes](#architecture-notes)
21. [Troubleshooting](#troubleshooting)
22. [Known Limitations](#known-limitations)
23. [Changelog](CHANGELOG.md)

---

## Feature Overview

| Feature | Description |
|---|---|
| CPU usage graphs | "CPU History (avg)" rolling area chart + "Per-Core CPU" bars with GHz frequency overlay |
| Temperature monitoring | Per-core °C on the CPU bars plus a CPU/RAM status-bar summary; reads DDR5/DDR4 on-DIMM sensors. Toggleable |
| Live process table | PID, name, CPU%, RSS memory, nice, CPU affinity, I/O class, ProBalance status |
| Per-process CPU affinity | Topology-aware checkbox picker; `sched_setaffinity` on all TIDs |
| Per-process nice priority | Full -20 to 19 range |
| Per-process I/O priority | `ioprio_set` syscall; classes None / Realtime / Best-effort / Idle |
| Rules Engine | Pattern-based automation (contains / exact / regex); applies affinity + nice + I/O + ProBalance exempt on process start |
| ProBalance | Automatic CPU throttle; per-process exemption from the context menu or via rule flag |
| Gaming Mode | Parks non-preferred CPUs offline; AMD X3D and Intel Hybrid topology aware |
| Game Launcher | Integrated Steam and Lutris game picker; `/proc`-based game watcher; auto-restore on exit |
| Gaming Mode profiles | Named profiles saved to config; instant load/switch |
| Companion Panel | Small always-on-top floating widget: CPU%, show/hide, maximize, gaming toggle, quit |
| Settings | Default affinity, poll intervals, dark/system theme, window opacity, temperature toggle, systemd autostart |
| System tray | CPU load bar icon; show/hide; gaming mode toggle; companion panel toggle; quit |
| Single-instance | A second launch raises the existing window instead of opening a duplicate |
| Dark theme | Catppuccin Mocha colour scheme applied at startup |
| Crash detection | Boot-aware unclean-shutdown marker; repairs parked CPUs left by a crash, and stops applying config after repeated crashes |
| Config persistence | Atomic JSON write to `~/.config/process-lasso-qt/config.json` |

---

## Requirements

**Runtime:**

| Package | Purpose |
|---|---|
| `qt6-base` | Qt6 Widgets, Core, Gui |
| `qt6-base` (Network module) | Single-instance socket (`QLocalServer`) |
| `polkit` | `pkexec` for privileged helper installation |
| `sudo` | Passwordless helper execution at runtime |
| `sqlite` *(optional)* | Lutris game library scanning (CLI `sqlite3` binary) |

**Build:**

| Package | Purpose |
|---|---|
| `cmake >= 3.20` | Build system |
| `ninja` *(recommended)* | Fast parallel builds |
| `gcc >= 13` or `clang >= 16` | C++17 support |
| `qt6-base` | Qt6 development headers (Widgets + Network) |

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

These are handled by a small compiled C binary (`process-lasso-helper`) that validates all arguments strictly before acting.

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

All inputs are validated; the helper refuses unknown commands and rejects CPU 0 for the offline request (bootstrap CPU protection).

---

## Configuration File

Path: `~/.config/process-lasso-qt/config.json`

The file is written atomically (write to `.tmp`, then rename) to prevent corruption on crash. Missing keys are filled from built-in defaults at load time via a recursive deep-merge.

**Top-level structure:**

```json
{
  "show_temperatures": true,
  "cpu": {
    "default_affinity": "",
    "gaming_mode": false,
    "gaming_profiles": {}
  },
  "monitor": {
    "rule_enforce_interval_ms": 500,
    "display_refresh_interval_ms": 2000
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
  "rules": [],
  "ui": {
    "system_theme": false,
    "opacity": 100
  }
}
```

**Rule object structure:**

```json
{
  "rule_id": "uuid-without-braces",
  "name": "My Rule",
  "pattern": "firefox",
  "match_type": "contains",
  "affinity": "0-7",
  "nice": null,
  "ionice_class": null,
  "ionice_level": null,
  "pb_exempt": true,
  "enabled": true
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
| Status | Process state | `⏸ Throttled` (ProBalance active) or `⚡ PB Exempt` (ProBalance bypassed) |

**Sorting:** Click any column header to sort ascending; click again for descending. The active sort column shows a ▲ or ▼ indicator.

**Filtering:** Type in the *Filter* box above the table to show only rows whose name or PID contains the search text (case-insensitive).

**Row colour coding:**

| Colour | Meaning |
|---|---|
| Orange | ProBalance has throttled this process |
| Teal | Process is exempt from ProBalance |
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
| Exempt '*name*' from ProBalance | Prevents ProBalance from throttling this PID (session only) |
| Remove ProBalance Exemption for '*name*' | Re-enables ProBalance throttling for this PID |

**Keyboard shortcut:** `Delete` or `Backspace` on a selected row sends SIGTERM to the selected process(es).

**Manual affinity protection:** When you manually change a process's affinity from the context menu, the monitor suppresses rule re-enforcement for that PID for 30 seconds, so your manual change is not immediately overwritten.

**ProBalance exemption note:** Per-PID exemptions set from the context menu are session-only — they are not saved to `config.json` because PIDs change across process restarts. To make an exemption permanent, add a rule with the *Exempt from ProBalance* checkbox ticked (see [Rules Tab](#rules-tab)).

---

### Rules Tab

Create and manage persistent per-process rules that are automatically applied whenever a matching process is seen.

**Table columns:** Enabled, Name, Pattern, Match Type, Affinity, Nice, I/O Class, I/O Level, PB Exempt.

**Buttons:**

| Button | Effect |
|---|---|
| Add Rule | Opens empty RuleEditDialog |
| Templates… | Opens the preset template picker |
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
| ProBalance | Checkbox | When ticked, matching processes are permanently exempt from ProBalance throttling |
| Enabled | Bool | Disabled rules are stored but never applied |

**Process picker:** Click *Select from running processes…* in RuleEditDialog to choose a process from a live `/proc` snapshot, which pre-fills the pattern and affinity fields.

Rules are applied once when a new PID is detected and re-applied on the configured enforcement interval (default 500 ms). Rules saved to `config.json` persist across restarts.

**ProBalance exemption via rules** is the recommended way to permanently protect an application (e.g. your game client, compositor, or audio server) from being throttled, since it matches by process name rather than PID and survives restarts.

---

### ProBalance Tab

Automatic CPU throttling that prevents one runaway process from starving the rest of the system.

**How it works:**

1. Every 1 second the monitor calls `ProBalance::tick()` with the current process snapshot.
2. For each non-exempt process, if its CPU% stays above the **CPU threshold** for at least **consecutive seconds**, ProBalance raises the process's nice value by **nice adjustment** (capped at **nice floor**).
3. Once the process's CPU drops below **restore threshold** and stays there for **restore hysteresis seconds**, the original nice value is restored.

**Configuration fields:**

| Field | Default | Description |
|---|---|---|
| ProBalance Enabled | Yes | Master on/off switch |
| CPU threshold | 85% | Per-process CPU% that triggers throttling |
| Consecutive seconds above threshold | 3 s | How long the process must exceed the threshold |
| Nice adjustment (added on throttle) | 10 | Added to the process's current nice value |
| Nice floor (max nice applied) | 15 | The nice value is never raised above this |
| Restore when CPU below | 40% | CPU% below which restoration begins |
| Restore hysteresis (seconds below restore threshold) | 5 s | How long CPU must stay low before restoring |
| Exempt Processes (pattern contains) | kwin, plasmashell, systemd, kthreadd, Xorg, xwayland | Process names that are **never** throttled (name-pattern, always active) |

Click **Apply Settings** to save and propagate changes to the running monitor.

**Exemption summary:**

| Method | Scope | Persistent |
|---|---|---|
| Exempt Patterns list (this tab) | By name substring — matches any process whose name contains the pattern | Yes (saved in config) |
| Rules tab → ProBalance checkbox | By name — matches any process the rule matches | Yes (saved with rule) |
| Processes tab context menu | By PID — applies to the currently running instance only | No (session only) |

Throttled processes show orange in the Processes tab with `⏸ Throttled`.
Exempt processes show teal with `⚡ PB Exempt`.

---

### Gaming Mode Tab

Optimises the system for a single game by concentrating the OS scheduler on the highest-performance CPU cores and optionally elevating the game's scheduling priority.

#### CPU Topology

At startup the tab detects the CPU topology:

| Topology | Detection method | Preferred cores | Non-preferred cores |
|---|---|---|---|
| **AMD X3D** (e.g. 7950X3D) | Compares L3 cache size across CCDs via `/sys/devices/system/cpu/cpuN/cache/index3/size` | V-Cache CCD (higher L3) | Non-V-Cache CCD |
| **Intel Hybrid** (e.g. 12th–14th gen) | Compares `cpuinfo_max_freq` across cores; freq ≥ 80% of max = P-cores | P-cores | E-cores |
| **Uniform** | All cores identical | — | Parking disabled |

SMT siblings (hyperthreads) are detected via `/sys/devices/system/cpu/cpuN/topology/core_id`.

#### Enable/Disable Gaming Mode

Clicking **▶ Enable Gaming Mode** runs a background thread that:
1. Collects all non-preferred CPUs plus any manually unchecked preferred CPUs.
2. Calls `process-lasso-helper cpu-online N 0` for each (skipping CPU 0).
3. Emits `gamingModeChanged(true, elevateNice)` to the monitor thread.

If **Elevate game priority (nice -1)** is checked, the monitor applies `nice -1` to every new process while gaming mode is active.

Clicking **⏹ Disable Gaming Mode** calls `process-lasso-helper cpu-unpark-all` and restores any elevated nice values.

#### Reset All Changes

The **↩ Reset All Changes** button unparks all CPUs and restores all recorded original affinities.

#### Game Launcher

| Field | Description |
|---|---|
| Game (name) | Name used to identify the game process in `/proc` |
| Command | Full launch command, e.g. `steam -applaunch 238960` |
| Steam… | Opens SteamGamePickerDialog — scans `~/.steam/steam/steamapps/*.acf` manifests |
| Lutris… | Opens LutrisGamePickerDialog — queries `~/.local/share/lutris/pga.db` via `sqlite3` |
| ▶ Launch | Enables Gaming Mode, launches the command, polls `/proc` for the game PID |
| ⏹ Kill Game | Sends SIGTERM to the detected game PID |
| Auto-disable Gaming Mode when game exits | Disables Gaming Mode automatically after the game process disappears |

#### Profiles

A profile stores: game name, launch command, per-CPU checkbox states, and the elevate-nice setting.

| Button | Effect |
|---|---|
| Save | Prompts for a name and writes the current state to `config.json` |
| Delete | Removes the currently selected profile |
| Combo box change | Loads the selected profile immediately |

---

### Settings Tab

#### Default Process Affinity

When enabled, every new PID that does not match any rule receives the configured default affinity mask.

#### Monitor Intervals

| Control | Default | Description |
|---|---|---|
| Rule enforcement interval | 500 ms | How often rules are checked against running processes |
| Display refresh interval | 2000 ms | How often the process table is updated |

#### Appearance

| Control | Default | Description |
|---|---|---|
| Follow system theme | Off | When checked, clears the Catppuccin Mocha stylesheet |
| Show CPU and RAM temperatures | On | Per-core °C on the bars + CPU/RAM status-bar summary. Greyed out if no supported sensor exists. See [Temperature Monitoring](#temperature-monitoring) |
| Window opacity | 100% | Sets `QMainWindow::setWindowOpacity`; range 30–100% |

#### Autostart

When **Start with desktop session** is checked, the app writes a systemd user service to `~/.config/systemd/user/process-lasso.service` and runs `systemctl --user enable process-lasso.service`.

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
[14:22:01] [Rule:Firefox] affinity=0-7 → firefox(12345)
[14:22:04] [ProBalance] THROTTLE krita(67890) cpu=91.2% nice 0→10
[14:22:09] [ProBalance] RESTORE krita(67890) cpu=15.0% nice 10→0
[14:22:30] [ProBalance] PID 12345 manually exempted
[14:23:15] [Gaming Mode] Parking CPUs…
[14:25:40] [Launcher] Game process found: PID 77001
```

---

## System Tray & Companion Panel

### System Tray

The application minimises to the system tray icon instead of exiting when the window is closed.

**Icon:** A 22×22 vertical bar showing current average CPU usage (green < 40%, yellow ≤ 80%, red > 80%).

**Tooltip:** `Process Lasso Qt — CPU: X.X%` (updated every monitor tick).

**Left/double-click:** Toggle window visibility.

**Right-click menu:**

| Entry | Effect |
|---|---|
| Show / Hide | Toggle main window |
| Maximize / Restore | Show and maximize (or restore) the main window |
| Enable / Disable Gaming Mode | Toggle Gaming Mode without opening the window |
| Companion Panel | Toggle the floating companion panel |
| Quit | Save config, stop monitor thread, exit cleanly |

### Companion Panel

A small frameless always-on-top floating widget, draggable by its CPU label area.

| Element | Description |
|---|---|
| CPU % label | Live average CPU usage |
| Show/Hide button | Toggle the main window |
| Maximize button | Show and maximize the main window |
| Gaming mode button | Toggle Gaming Mode (highlighted when active) |
| Quit button | Exit the application |
| ✕ button | Hide the companion panel (does not quit) |

The panel's visibility is synced with the *Companion Panel* tray menu entry.

---

## CPU Usage Graphs

Two live graphs appear at the top of the main window:

### CPU History (avg)

A rolling 120-sample area chart showing the **average CPU usage** across all logical cores. The fill colour transitions green → yellow → orange → red as the average rises.

### Per-Core CPU

A grid of per-logical-CPU bars. Each bar shows:

| Element | Description |
|---|---|
| `Core N` label | Left label |
| Usage bar | Colour-coded fill |
| Percentage | Right-aligned inside the bar |
| Frequency | `X.XX GHz` sub-line from `scaling_cur_freq` |
| Temperature | `NN°C` sub-line under the `Core N` label — see [Temperature Monitoring](#temperature-monitoring) |
| "off" label | Shown for CPUs taken offline by Gaming Mode |

**Dynamic layout:** Column count adjusts as the window is resized; the widget height grows to fit all bars.

---

## Temperature Monitoring

Controlled by **Settings → Appearance → Show CPU and RAM temperatures** (config key `show_temperatures`, default `true`). When it is off, no hwmon files are read at all — the sweep is skipped entirely rather than just hidden.

### Where readings appear

| Location | Shows |
|---|---|
| Per-Core CPU bars | `NN°C` under each `Core N` label, colour-ramped |
| Status bar (right) | `CPU 93°C · RAM 52°C`; hover for a per-DIMM breakdown |
| System tray tooltip | CPU temperature appended to the CPU% line |

The orange heat *tint* applied to bar fills above 40 °C predates this feature and is always active, independent of the toggle.

### Supported sensors

Readings come from `/sys/class/hwmon`, matched by driver `name`:

| Driver | Provides | Notes |
|---|---|---|
| `coretemp` | Intel CPU package + per-core | `Package id N` and `Core N` labels |
| `k10temp`, `zenpower`, `zenpower3` | AMD CPU package | `Tdie` preferred, `Tctl` fallback |
| `spd5118` | DDR5 on-DIMM thermal sensor | One hwmon node per stick |
| `jc42` | DDR3 / DDR4 on-DIMM thermal sensor | Same handling as `spd5118` |

Both SMT siblings of a physical core report that core's temperature, keyed on `(physical_package_id, core_id)` so ids that repeat across sockets stay distinct.

**AMD note:** `k10temp` and `zenpower` expose no per-core sensor, so on those CPUs only the package temperature appears and the bars carry no `°C` line. This is a driver limitation, not a bug. `Tccd*` per-CCD sensors are not currently surfaced.

**No sensors?** If the machine exposes none of the drivers above, the Settings checkbox is greyed out with an explanatory tooltip and the status bar reads `no temperature sensors`. Missing DIMM sensors are common — many boards and most laptops do not wire them up. If your DDR5 sticks report nothing, check that the `spd5118` module is loaded (`modprobe spd5118`) and that SPD access is not disabled in firmware.

### Cost

Sensor file paths and the CPU topology map are discovered once and cached; rediscovery is triggered only when a cached path disappears (module unloaded, device removed). The sweep runs on the monitor thread at the display refresh interval, never on the GUI thread.

---

## Crash Detection & Safe Mode

A marker at `~/.local/state/process-lasso/runstate.json` records that a session
is in progress. It is armed at startup and cleared only after a **fully
completed** shutdown, so finding it still armed means the previous run died.

### Why the boot id matters

The marker also stores the kernel's boot id, which separates two cases a plain
clean/dirty flag conflates:

| Marker | Boot id | Meaning | Action |
|---|---|---|---|
| clean / absent | — | Shut down properly (or first ever run) | Resume normally |
| armed | **matches** | Crashed during **this** boot — parked CPUs are still stale | Unpark them |
| armed | **differs** | Crashed, but the machine rebooted since | **Nothing to repair** |

That last row is the power-loss case. CPU online state is kernel runtime state
that a reboot resets on its own, so a flag without the boot id would trigger a
"recovery" the reboot already performed.

### Safe Mode

Every unclean start increments a counter; a clean shutdown resets it. After
**3 in a row** the app starts in Safe Mode:

- Rules, default affinity and ProBalance are **not applied**. Monitoring, the
  process table and everything read-only still work normally.
- A banner explains why, with a **Resume Normal** button.
- **Your `config.json` is never modified.** Safe Mode suppresses *application*
  of the config, not the config itself.

Safe Mode is sticky — it clears only when you press Resume Normal, never on a
timer. Otherwise a config that crashes the app a minute in would keep resetting
the counter and never trip the protection. In normal mode the counter resets
after 60 seconds of uptime, for the same reason.

---

### Parked CPUs in the affinity picker

CPUs parked by Gaming Mode are shown in **red** in the CPU picker, with a tooltip
explaining they are offline. They remain **selectable** — parking is temporary,
while a rule or default affinity is saved config you may well be writing while
Gaming Mode is on.

Pressing OK with parked CPUs selected warns first:

- **All** selected CPUs parked → the affinity cannot take effect at all right
  now; you are asked to confirm before saving.
- **Some** selected parked → it applies to the online ones and picks up the rest
  once they are unparked.
- Selecting **every** CPU never warns — that is the same as no restriction.

---

## Rules Engine Deep Dive

Rules are stored as a JSON array in `config.json`. Each rule has a UUID (`rule_id`), so editing and deleting are stable across reorders.

**Match types:**

| Type | Behaviour |
|---|---|
| `contains` | `procName.contains(pattern, Qt::CaseInsensitive)` |
| `exact` | `procName == pattern` |
| `regex` | `QRegularExpression(pattern).match(procName).hasMatch()` |

**Application cadence:** Rules are applied to every new PID on first detection, then re-applied to all known PIDs on every `rule_enforce_interval_ms` tick (default 500 ms). Rules are suppressed for a PID during a 30-second manual override window after a manual affinity change.

**Affinity application:** `sched_setaffinity` is called on the main thread *and* all TIDs found in `/proc/[pid]/task/`.

**Wine/Proton name resolution:** If `/proc/[pid]/comm` is exactly 15 characters (kernel truncation), the cmdline is checked for a `.exe` path; the basename is used as the match name.

---

## ProBalance Deep Dive

ProBalance runs inside the monitor thread and is ticked every 1 second.

**State machine per process:**

```
Normal  ──[ CPU > threshold for N consecutive seconds ]──▶  Throttled
                                                            (nice raised)

Throttled ──[ CPU < restore_threshold for M seconds ]──▶  Normal
                                                          (nice restored)
```

**Exemption priority (highest to lowest):**
1. Name-pattern exemption (`exempt_patterns` in ProBalance config) — checked by ProBalance itself
2. Rule-based exemption (`pb_exempt: true` on a matching rule) — resolved by `RuleEngine::isPbExempt(name)` before tick
3. Manual per-PID exemption (context menu) — stored in `ProcessMonitor::m_pbManualExempt`

All three are honoured; a process matching any exemption path is never throttled.

---

## Gaming Mode Deep Dive

### CPU Parking

CPU parking writes `0` to `/sys/devices/system/cpu/cpuN/online` via the privileged helper. This is a kernel hotplug operation — the CPU is fully taken offline at the scheduler level. CPU 0 is never parked.

### Nice Elevation

When **Elevate game priority** is checked and Gaming Mode is active, every new process gets `nice -1` via the helper. Original nice values are recorded and restored when Gaming Mode is disabled.

### Profile Switching While Active

If Gaming Mode is already active and you load a different profile, the tab disables then immediately re-enables Gaming Mode with the new core selection.

---

## CPU Topology Detection

**AMD X3D:** Reads L3 cache size from `/sys/.../cache/index3/size`; larger L3 = V-Cache CCD (preferred).

**Intel Hybrid:** Reads `cpuinfo_max_freq`; cores with freq ≥ 80% of the maximum are P-cores (preferred).

**SMT detection:** Groups logical CPUs by `core_id`; the second logical CPU per physical core is the SMT sibling.

---

## How CPU% is Measured

Per-process CPU% from `/proc/[pid]/stat` jiffie deltas divided by wall-clock elapsed time × `sysconf(_SC_CLK_TCK)`. Per-CPU bars use the same principle against `/proc/stat` cpuN lines tracking `idle + iowait` delta.

---

## Preset Rules

The *Templates…* button in the Rules tab offers pre-built rules designed for a typical AMD X3D system where CCD0 (CPUs 0-7, 16-23) is the V-Cache CCD.

| Preset | Pattern | Match | Affinity | Nice | I/O |
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
| Compiler (gcc/clang) | gcc | contains | — | — | 2 (BE) |
| Archive / compress | 7z | contains | 8-15,24-31 | 10 | 3 (Idle) |
| Background (nice 10) | *(empty)* | contains | — | 10 | — |

Each preset opens in the full RuleEditDialog so you can customise before saving.

---

## Single-Instance Behaviour

Only one instance of Process Lasso Qt runs per user session. If you launch it a second time (e.g. from a desktop shortcut or autostart):

1. The new process tries to connect to a local socket named `process-lasso-qt-<username>`.
2. If the first instance is running, the connection succeeds: the new process sends a `raise` signal and exits immediately — no window flashes, no duplicate tray icon.
3. The first instance receives the signal and brings its window to the foreground.
4. If no instance is running, the process becomes the owner of the socket and starts normally.

Stale socket files left by a crash are cleaned up automatically on the next start.

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
main.cpp  (single-instance guard → QLocalServer → MainWindow)
  └─ MainWindow (QMainWindow)
       ├─ CompanionWidget      — frameless floating panel (always-on-top)
       ├─ CpuHistoryWidget     — 120-sample rolling average area chart
       ├─ CpuBarsWidget        — Per-CPU usage bars + GHz frequency
       ├─ ProcessTableWidget   — Live sortable/filterable process table
       ├─ RulesEditor          — Rule CRUD table + dialogs
       ├─ ProBalanceTab        — ProBalance config form
       ├─ GamingModeTab        — CPU parking + game launcher
       ├─ SettingsTab          — App settings
       └─ Log QTextEdit
            │
            ├─ ProcessMonitor (QThread) ── reads /proc every 100 ms
            │    ├─ RuleEngine           ── applied every 500 ms; isPbExempt() for PB
            │    └─ ProBalance           ── ticked every 1 s with merged exempt PID set
            │
            └─ Config namespace          ── load/save config.json
```

- The `ProcessMonitor` thread never touches Qt GUI objects directly — it emits signals dispatched to the GUI thread via `Qt::QueuedConnection`.
- Config updates from the GUI pass through a `QMutex`-protected copy (`m_configMux`).
- **`/proc` reading:** Always use `QFile::readAll()` + `split('\n')`. Never `while (!f.atEnd()) readLine()` — `/proc` files report `size() == 0` so `atEnd()` returns `true` immediately.
- CPU parking runs in transient `QThread` workers so it never blocks the GUI.

---

## Troubleshooting

### Verbose / debug mode

```bash
./build/process-lasso-qt --verbose 2>&1 | grep '\[V\]'
```

Instrumented paths: `ProcessMonitor` (percpu reads), `CpuBarsWidget` (geometry, paint), `CpuHistoryWidget` (average, depth).

### CPUs left parked after a crash

```bash
for cpu in /sys/devices/system/cpu/cpu*/online; do echo 1 | sudo tee "$cpu"; done
```

Or use **↩ Reset All Changes** from the Gaming Mode tab if the GUI can start.

### ProBalance throttling the wrong process

Use the Processes tab context menu to exempt a specific running PID instantly, or add a persistent rule with the **ProBalance** checkbox ticked (Rules tab → Add Rule).

---

## Known Limitations

- **Negative nice values** require the privileged helper. Without it, only nice ≥ 0 can be set from the GUI.
- **CPU parking** requires the helper. On systems without it, Gaming Mode parking is disabled for asymmetric topologies.
- **I/O Realtime class** (class 1) requires root.
- **Lutris game library** scanning requires the `sqlite3` CLI.
- **Uniform CPU topology:** Gaming Mode CPU parking is disabled — no meaningful partition exists.
- **CPU% values** are approximate (Linux scheduler jiffies, typically 100 Hz resolution).
- **ProBalance per-PID exemptions** are session-only. Use a rule for permanent exemption.
- Config is saved on every rule/settings change and on exit. Parked CPUs are now brought back online on **every** shutdown Process Lasso can observe — tray Quit, window close, and `SIGTERM` / `SIGINT` / `SIGHUP` (systemd stop, logout, Ctrl-C).
- **Parked CPUs and affinity interact.** A rule or default affinity whose CPUs are *all* parked cannot be applied at all — Process Lasso now says so in the Log instead of failing silently. If only some are parked, the affinity applies to the online remainder and widens automatically once they return.
- **`SIGKILL` and power loss cannot be caught.** A `SIGKILL` in the same boot is repaired at the next launch by the crash marker (see [Crash Detection](#crash-detection--safe-mode)); a power loss needs no repair, because the reboot brings every CPU back on its own. **Reset All Changes** still works at any time.
- Restoring on exit uses the app's own notion of ownership, which counts *any* CPU that was already offline at startup. A CPU you took offline by other means will therefore be brought back online when Process Lasso exits.

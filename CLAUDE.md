# CLAUDE.md — LLM Context for process-lasso-qt

C++17/Qt6 Linux process manager for CachyOS/Arch. Replaces a Python/PyQt6 upstream with
direct syscalls. No Python, no psutil, no subprocess (except the privileged helper).
Current version: **1.5.0**.

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
tests/
  probalance-harness.cpp— standalone ProBalance checks; not in the CMake build
  ruleengine-harness.cpp— asserts rule enforcement is SILENT once values match
  monitor-harness.cpp   — drives ProcessMonitor headless: rule apply + manual override
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

`main` is the released line (currently 1.5.0). One feature lives off it:

**`fan-control`** — hwmon PWM fan control (Fan Control tab, curve editor, six
new privileged-helper commands). ⚠️ That branch's own docs still call itself
**v1.4.0**, which `main` took on 2026-09-23 — renumber it to 1.5.0 when/if it
ships. Complete, builds clean, and verified
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

## Editing a rule from the Processes tab (v1.4.2)

Checkbox in the Processes tab filter row, **"Overwrite matching rules"**, config key
`ui.overwrite_matching_rules` (default **false** — it edits saved rules, so it is
opt-in). Persisted on toggle, not on Apply.

Every manual setter in `ProcessTableWidget` (affinity, nice **and** I/O priority) now
emits `manualChangeApplied(ManualChange)`. Before 1.4.2 only affinity emitted
anything, so a manual nice or ionice change got **no** protection and a matching rule
reverted it on the next pass, ~500 ms later.

`MainWindow::onManualChange()` decides:

| Situation | Behaviour |
|---|---|
| No enabled rule sets that attribute to a different value | 30 s override, nothing to ask about |
| Conflict, toggle **off** | 30 s override + a log line saying enforcement was suppressed and how to change the rule |
| Conflict, toggle **on** | Ask: **Change rule** (rewrite + save) or **Just this process** (indefinite override) |

The dialog names the rule and **how many running processes it affects**
(`ProcessTableWidget::countMatching`) — a rule covers every process matching its
pattern, so rewriting one from a single row can repoint ~150 of them. Never skip that
count.

After **Change rule**: `RuleEngine::updateRule()`, `RulesEditor::refresh()`,
`saveConfig()`, and **no** manual override — the rule now agrees with the user, so the
next pass is a no-op for that pid and applies the new value to its siblings.

`setManualAffinityOverride` is now **`setManualOverride`**: it always suppressed the
whole `applyToProcess` call, not just affinity, and the old name hid that. A duration
of `0` means indefinite (stored as `infinity`), used by "Just this process" — a
30 s grace there would be absurd after the user was explicitly asked. `run()` drops
override entries whose pid is no longer in the snapshot, so an indefinite one cannot
outlive its process and catch a recycled pid.

### The "manual change gets rolled back" report — resolved, it was never a rollback

Chased across several rounds in Sept 2026 and worth not re-chasing. A manual affinity
change *appeared* to be reverted instantly. It never was: the value reached the kernel
every time. The **display** was stale, for two compounding reasons.

1. Until 1.4.1 the rule-enforcement log flood starved the GUI thread, so the Processes
   table stopped repainting **entirely**. The old mask stayed on screen indefinitely.
2. Even with a healthy GUI, the table lags by `display_refresh_interval_ms` (2 s), and
   all three context-menu dialogs were seeded from the **table cell** rather than from
   `/proc` — so reopening one showed the pre-change value and confirmed the illusion.

Fixed in 1.4.2: dialogs read live values, and `ProcessTableWidget::refreshRow()`
re-reads that one process and repaints immediately after a successful change.

**The lesson:** when this app "ignores" an action, suspect the *display* before the
mechanism. `tests/monitor-harness.cpp` exercises the real override path headlessly and
passed throughout — that disagreement between a passing test and a user's screen was
the clue, and it was read as "cannot reproduce" for too long.

---

## Editing a rule must never silently discard the edit (v1.4.9)

Three separate silent failures made *"editing rules does nothing"* a true statement
from the user's side, while the engine was working correctly the whole time. The
engine layer is covered by `tests/` and passed throughout — **the GUI layer was the
bug every time**. Check it first when a change "does not apply".

1. **The duplicate guard discarded edits.** `RulesEditor::editSelected()` ran
   `findDuplicate()` and, on a hit, offered *Edit existing rule* / *Add anyway* /
   *Cancel* — two of which `return`ed without calling `updateRule()`. With two
   `firefox` rules present, editing either one threw the change away, and *Edit
   existing rule* redirected the user to a **different** rule than the one they
   opened. Introduced in 1.4.3 by reusing the add-path guard.
   → The edit path now warns but only ever offers **Save change / Discard change**,
   defaulting to Save, and never redirects. Reuse of the add-path dialog here is the
   mistake; keep them separate.

2. **`RuleEditDialog::getRule()` rebuilt the Rule from the form widgets**, so every
   field *not on the form* was silently dropped — which silently revoked
   `allowHelper` (added 1.4.4) on every edit. ⚠️ **Any new `Rule` field that is not
   exposed in the dialog must be copied from `m_rule` in `getRule()`**, or editing a
   rule quietly erases it.

3. **nice and ionice failures were silent.** Only affinity had a failure branch;
   `setNice()`/`setIoNice()` returning false produced nothing at all, so a rule that
   could not apply its priority looked exactly like a rule doing nothing. Now
   `warnOnce(rule, pid, procName, what, errno)` — deduped per rule+pid+attribute,
   because these repeat twice a second forever.

### "Refresh & Reapply Rules" button

Rules are **already** enforced every `rule_enforce_interval_ms` (500 ms default), so
this does not change *when* they apply. It exists for feedback:
`ProcessMonitor::reapplyRulesNow()` sets a flag, `run()` forces one pass and logs
`[Rules] Re-applied: N changes across M processes, K skipped (manual override)` —
including explicitly reporting **"nothing needed changing"**, which is the answer to
"is my rule working?", not evidence that it is not.

It also resyncs the tables first (`RulesEditor::refresh()`, `refreshPbExemptPatterns()`,
`updateThrottled()`). That is an escape hatch, **not** a licence for stale views: if
pressing it visibly changes what is displayed, a refresh path is missing and that is
the bug to fix.

---

## Two rules matching the same process (v1.4.3)

Nothing stops two enabled rules matching the same process — the Rules tab happily
holds two `firefox` rules, and the Processes tab's *Add Rule for '<name>'…* makes it
easy to create the second one by accident.

If both set the **same attribute to different values** they overwrite each other on
every enforcement pass: rule A writes its value and logs, rule B writes its value and
logs, twice a second, forever. ⚠️ The v1.4.1 no-op skip **cannot** damp this — it
suppresses writes when the value already matches, and here the value genuinely changes
every time. It is a second, independent route to the same GUI-starvation flood.

**`applyToProcess()` is first-match-wins, PER ATTRIBUTE.** The first enabled matching
rule that defines an attribute claims it; later rules are ignored for that attribute
only. Per attribute, not per rule, so "rule A sets affinity, rule B sets nice" still
combines correctly — that is a legitimate setup and must keep working.

The claim is staked **even when the write fails**, otherwise a losing rule steps in on
the next pass and restarts the fight.

`RuleEngine::shadowedAttributes()` reports `ruleId → field names` for attributes that
can never apply because an earlier enabled rule with the **same pattern and match
type** already claims them. The Rules tab strikes those cells through in grey with a
tooltip — a setting that is in the config but inert must not look live. Only the
exactly-duplicated case is reported: general pattern overlap is undecidable (regex),
and first-match-wins handles it safely regardless.

`RulesEditor::findDuplicate()` / `confirmDuplicate()` guard **add, add-from-Processes-tab
and edit**, offering *Edit existing rule* / *Add anyway* / *Cancel*.

Covered by `tests/ruleengine-harness.cpp`, which fails 3/10 against the 1.4.2 engine.

---

## Rule enforcement must be a no-op when nothing changed (v1.4.1)

`RuleEngine::applyToProcess()` runs for **every matching pid, twice a second**.
Until 1.4.1 it wrote and logged unconditionally, which meant a steady state that
was already correct still produced a write and a log line every pass.

Cesar's config has 9 rules matching ~150 brave/chromium/firefox/"Isolated Web Co"
processes. That is ~300 log lines/second into the Log tab's `QTextEdit` **on the
GUI thread** (each one a queued `logMessage` signal), plus — because
`Utils::setAffinity()` loops over every **thread** of the process — thousands of
`sched_setaffinity` calls a second. The GUI thread saturates and the window stops
servicing input, which presents as *"setting affinity does nothing"* even though
the affinity code is fine. It also fits an unclean shutdown.

**Rule: read before you write.** Each of the three actions compares first
(`Utils::getAffinityStr` / `getNice` / `getIoNice`) and skips both the syscall and
the log line when the value already matches. A rule that genuinely needs applying
still applies and logs exactly once.

Affinity compares **sets**, not strings — `cpulistToSet()` on both sides — so
`"16-31"` and `"16-17,18-31"` are the same mask. An empty read (process gone)
falls through to the write rather than being treated as a match.

⚠️ Do not "simplify" this back to an unconditional write. The logging volume is
the load-bearing part: this is a GUI-thread starvation bug, not a syscall-cost
bug. `getAffinityStr()` reads only the main thread while `setAffinity()` writes
all of them; that is deliberate and fine, because threads inherit the mask.

**Diagnostic tell:** if the app is sluggish or "ignores" the UI, look at the Log
tab first. Identical lines repeating every second for the same pids is this bug.

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

## Helper security model — read before touching helper/ (v1.4.4)

### The helper CANNOT authenticate its caller. Do not try.

Cesar asked, reasonably, whether the helper could be made to "only respond to the
application". **It cannot**, and attempting it is actively harmful because it buys
false confidence. Every available check is defeated by a caller that controls its own
process image:

| Check | How it fails |
|---|---|
| Parent's `/proc/<ppid>/exe` or name | The parent `exec()`s something else after `fork()` — a TOCTOU race — or simply copies the app binary and execs the helper from that |
| A secret compiled into the app | The app binary is world-readable; `strings` it |
| `argv[0]` | Caller-controlled |

So the helper does **no** caller-identity checking. What contains the risk instead:

1. **Scope the sudoers rule to one user.** Until 1.4.4 `install-helper.sh` wrote
   `ALL ALL=(root) NOPASSWD: …` — *every local account* could run the helper as root
   with no password. Nothing needed that. It is now scoped to the installing user,
   taken from `PKEXEC_UID`/`SUDO_USER` (set by pkexec/sudo themselves, not by the
   caller), validated with `visudo -cqf` before installation. **Do not widen it.**
2. **A tiny fixed command set.** No shell, no `exec`, no caller-supplied paths. The
   sysfs path is built with `%d` from a validated integer, never from caller text.
3. **No command can grant code execution or change credentials.** The worst a hostile
   caller gets is local denial of service — park CPUs, renice, repin. Genuinely bad,
   not privilege escalation. **Any new command must preserve this property**; the
   moment one can write an arbitrary path or run an arbitrary binary, the helper
   becomes a root exploit and the sudoers rule becomes the vulnerability.
4. **Validate every argument before use.** `set-affinity` rejects cpulists containing
   anything but digits/comma/dash, longer than 255, or naming a CPU ≥ `_SC_NPROCESSORS_CONF`;
   and refuses pid ≤ 1 and kernel threads (no `/proc/<pid>/exe`).

### Escalation flow (affinity on another user's process)

`sched_setaffinity` on a process you do not own fails with `EPERM` — common with
Docker containers, which run as root. `Utils::setAffinity` now reports errno out, and
`Utils::describeAffinityError()` turns it into a sentence naming the owner.

`RuleEngine` then, per rule:
- `rule.allowHelper` true (JSON `allow_helper`) or allowed for the session →
  `CpuPark::setAffinityViaHelper()`.
- otherwise → fire `EscalationCb` **once per rule per session** and wait. Once per
  *rule*, never once per pid: a rule matching a browser would open 150 dialogs.
- otherwise → explain once per rule+pid via `m_permWarned`.

⚠️ Every branch is deduped. A rule that can never succeed runs twice a second forever,
so an undeduped log line there is the 1.4.1 flood again. `tests/ruleengine-harness.cpp`
asserts this using pid 1, which is root-owned and so yields EPERM without any
privilege to set up.

`ProcessMonitor` forwards the callback as a **queued** signal; the prompt must be built
on the GUI thread. Answering "remember" writes `allowHelper` onto the rule, so the
grant is visible and revocable in the Rules tab rather than hidden in a side list.

---

## Privileged helper

Binary: `/usr/local/bin/process-lasso-helper`  
Installed by: `packaging/install-helper.sh` (run as root)  
Invoked by: `CpuPark::parkCpus()`, `CpuPark::unParkAll()`, `CpuPark::setProcessNiceViaHelper()`

```
process-lasso-helper cpu-online <N> <0|1>    # park/unpark single CPU
process-lasso-helper set-affinity <cpulist> <pid>  # sched_setaffinity on every thread
process-lasso-helper cpu-unpark-all          # reads /sys/.../offline, brings all back
process-lasso-helper renice-pid <nice> <pid> # setpriority(PRIO_PROCESS, pid, nice)
process-lasso-helper --check-only            # exit 0, used to verify sudo access
```

Sudoers entry written by install-helper.sh (**scoped to the installing user** since
1.4.4 — it used to be `ALL ALL=`, see the security model above):
```
<installing-user> ALL=(root) NOPASSWD: /usr/local/bin/process-lasso-helper
```
Existing installs keep the old permissive rule until the helper is reinstalled.

### Uninstalling

`packaging/uninstall-helper.sh` (bash, run via `pkexec`, counterpart to
install-helper.sh; both are installed to `usr/share/process-lasso-qt/`).

Order is load-bearing: the **sudoers rule goes first**, then `visudo -cqf /etc/sudoers`
runs and the script **aborts before touching the binary** if the configuration no
longer parses. A broken file under `/etc/sudoers.d` locks sudo out for everyone, so
the passwordless grant is removed first and proven safe before anything else happens.

Default is to **move aside** into `/var/backups/process-lasso-uninstall-<timestamp>/`,
not delete; `--purge` deletes. Idempotent — safe to run when nothing is installed.
The sudoers file is moved **out of** `/etc/sudoers.d/`, never renamed in place: sudo
ignores names containing a dot, so a `.bak` left there would be inert but still look
like live configuration to the next person reading the directory.

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
The script resolves the helper binary **relative to its own location**, searching a
fixed candidate list.

⚠️ **Fixed 1.4.5 — the original was off by one directory and never worked.** It used
`$(dirname "$0")/..` + `/bin`. The script installs to `<prefix>/share/process-lasso-qt/`,
so one level up is `<prefix>/share` and that resolved to `<prefix>/share/bin/…` — a
directory that exists in no layout. Installing the helper from an AppImage or a
packaged install therefore failed **every time**; this file previously claimed the
opposite, which is why it went unnoticed. The binary is **two** levels up:

```bash
DIR="$(cd "$(dirname "$0")" && pwd)"      # <prefix>/share/process-lasso-qt
PREFIX="$(cd "$DIR/../.." && pwd)"        # <prefix>
"$PREFIX/bin/process-lasso-helper"        # correct
```

The remaining candidates cover `DESTDIR` trees and a source checkout
(`build/`, `build-appimage/`), so `./packaging/install-helper.sh` works from a clone
after a build. The script also refuses to run as non-root with a clear message
instead of failing later inside `install(1)`.

**Do not** restore the old `${1:-...}` form. The C++ caller used to pass a username
as `$1`; the script incorrectly used it as the helper source path, causing a silent
install failure (`[[ ! -f "cesarin" ]]`).

### ⚠️ Two facts that broke installHelper() twice — do not rediscover them

**1. `applicationDirPath()` is the AppImage MOUNT ROOT, not `$APPDIR/usr/bin`.**
`packaging/build-appimage.sh` puts a **copy of the application binary at
`$APPDIR/AppRun`** (verified: identical md5 to `usr/bin/process-lasso-qt`), and that
copy is what executes. So `/proc/self/exe` — and therefore `applicationDirPath()` —
is `/tmp/.mount_XXXXXX`, and `"../share/…"` resolves to `/tmp/share/…`. Anything
computed relative to the binary must use `"/usr/share/…"` from that root.

**2. `QStandardPaths::AppDataLocation` can never find our data.** It appends
`<organizationName>/<applicationName>` → `share/AcornInteractive/process-lasso-qt`,
but CMake installs to `${CMAKE_INSTALL_DATADIR}/process-lasso-qt` → `share/process-lasso-qt`.
No organisation directory, so the two never meet — in *any* layout, installed or
AppImage. Use **`GenericDataLocation`** with an explicit `"process-lasso-qt/"` prefix.

Either change `setOrganizationName()` **and** the install path together, or leave
both alone. Changing one silently breaks every data lookup.

### ⚠️ root CANNOT read anything inside the AppImage (v1.4.8)

An AppImage is a **FUSE mount owned by the invoking user**, and FUSE refuses access
to every other uid — *including root* — unless `/etc/fuse.conf` sets
`user_allow_other`, which is off by default and no application may assume.

So `pkexec bash /tmp/.mount_XXXX/usr/share/process-lasso-qt/install-helper.sh` fails
with a bare **"Permission denied"** even when the user authenticates correctly. Root
genuinely cannot open that path. Nothing about the polkit side is wrong, which is why
the message is so misleading.

`installHelper()` therefore **stages** `install-helper.sh` *and* the helper binary
into a `QTemporaryDir` before invoking pkexec, laid out like an install prefix:

```
<tmp>/share/process-lasso-qt/install-helper.sh
<tmp>/bin/process-lasso-helper
```

so the script's own `$PREFIX/bin` lookup resolves without it knowing it is a copy.
The temp dir is 0700 and owned by the user — fine, because root bypasses ordinary
DAC on a normal filesystem; it is only FUSE that blocks it.

**Anything handed to pkexec, sudo or a root helper must be copied out of the mount
first.** This applies to any future file the privileged side needs to read.

### CpuPark::installHelper() — where it looks for the script

1. `QStandardPaths::locate(GenericDataLocation, "process-lasso-qt/install-helper.sh")`
   — system and user installs, **and** AppImages, because AppRun prepends
   `$APPDIR/usr/share` to `XDG_DATA_DIRS`. This is the one that does the work.
2. `applicationDirPath() + "/usr/share/process-lasso-qt/…"` — AppImage, from the
   mount root. Covers the case where `XDG_DATA_DIRS` was *not* patched, so the two
   mechanisms are independent.
3. `applicationDirPath() + "/../share/process-lasso-qt/…"` — `<prefix>/bin` layouts.
4. `applicationDirPath() + "/../packaging/…"` and `"/packaging/…"` — source checkout.

Verified against a real extracted AppImage with and without the `XDG_DATA_DIRS`
patch, from a build directory, and from a bogus path.

**All** must remain. The failure message now lists every path searched; keep that —
the bare version sent the reader looking for a missing file rather than a wrong
assumption.

### Window opacity does not work on Wayland (v1.4.6)

`QWidget::setWindowOpacity()` is a **silent no-op** under Wayland. Verified on this
machine: `libQt6XcbQpa.so.6` contains `_NET_WM_WINDOW_OPACITY` (X11 sets that window
property), while `libQt6WaylandClient.so.6` contains no opacity protocol at all and
only the inherited `QPlatformWindow::setOpacity` base symbol. There is no
window-opacity request in xdg-shell, so a client cannot do this.

The slider is therefore **disabled with an explanation** when
`QGuiApplication::platformName()` starts with `wayland`, and `applyTheme()` skips the
call rather than making it and having it ignored. It still works under X11 and
XWayland. Do not "fix" this by re-enabling the control.

Config key is **top-level `window_opacity`**, not `ui.opacity`. A dead
`ui.opacity` default sat in `config.cpp` that nothing ever read; removed in 1.4.6.

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
    "overwrite_matching_rules": false
  },
  "window_opacity": 100            // TOP-LEVEL, not under "ui". Ignored on
                                   // Wayland — see below.
}
```

`Config::deepMerge(base, override)` recursively merges — override wins on leaf keys,
both objects merged for nested objects.

---

## Runtime failures must be visible in the Rules tab (v1.5.0)

A rule that cannot apply a setting used to show a plain "Yes" in Enabled. The Log
said it was failing; the table said it was fine. Same silence problem as 1.4.1,
1.4.9 and the stale-display work — **when something does not happen, say so where
the user is looking**, not only in a log they may never open.

`RuleEngine` keeps `ruleId → { "affinity"|"nice"|"ionice" → reason }`:
- `noteFailure()` on every failure path, `clearFailure()` on every success path
  **and on the already-correct path** — otherwise a rule that starts working again
  stays marked failing forever.
- Guarded by its own `m_failMux`: written on the monitor thread, read on the GUI
  thread. ⚠️ `RuleEngine` otherwise has no mutex; do not read the rest of it from
  the GUI thread on the strength of this one.
- `failureGeneration()` bumps only when the map actually changes.
  `MainWindow::onSnapshot()` repaints the Rules tab only when it moves — rebuilding
  every snapshot flickers and drops the selection; never rebuilding means a rule
  that has begun failing still looks healthy.

## Matching by command line (v1.5.0)

`Rule::matchTarget` — `"name"` (default) or `"cmdline"`, JSON key `match_target`.
`Rule::matches(procName, cmdline)` picks the subject; everything else (contains /
exact / regex) is unchanged.

Why: several processes routinely share one name — a dozen `python3.13`
interpreters, every Electron app called `node` — and a name rule cannot tell them
apart. The command line can: `python3.13 ./ComfyUI/main.py --listen --port 8188`.

- **Every caller must pass the cmdline.** `applyToProcess()` and `isPbExempt()`
  take it as a defaulted argument, so a missed call site compiles fine and
  silently stops cmdline rules working. Call sites: `ProcessMonitor::run()`,
  `applyNewPid()`, `reapplyAllDefaults()`, `ProcessTableWidget::countMatching()`.
- A cmdline rule with an empty cmdline **does not match** (kernel threads have no
  cmdline) rather than falling back to the name — falling back would silently
  widen the rule to everything sharing that name, which is what it exists to avoid.
- `shadowedAttributes()` keys on pattern + matchType **+ matchTarget**: a name rule
  and a cmdline rule with the same pattern target different processes.
- Rules saved before 1.5.0 have no `match_target`, default `"name"` — unchanged.

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

## ProBalance exemptions (v1.2.0, reworked 2026-09-23)

**Everything matches by NAME, never by PID**, using one shared rule: a pattern is a
case-insensitive *substring* of the process name. That rule lives in exactly one
place — `ProBalance::nameMatches()` / `nameMatchesAny()` (public statics). It was
reimplemented in four files once; do not do that again.

`ProBalance::toggleExemptPattern(QStringList&, name, exempt, removed)` is the shared
add/remove: **adding** is a no-op when an existing pattern already covers the name;
**removing** drops *every* pattern matching the name, because the pattern covering a
process need not be its exact name — dropping only an exact-name entry would leave a
broader pattern in place, keeping the process exempt while the UI unticked itself.

⚠️ **Substring matching is the sharp edge.** A short pattern (`sh`, or worse `e`)
exempts most of the machine, and the failure is silent — ProBalance throttles nothing
and logs nothing. The ProBalance tab carries a warning label above the list saying so;
keep it there. If a bug report says "ProBalance does nothing", check `exempt_patterns`
before anything else.

### The three exemption lists

| # | List | Lives in | Lifetime | Written by |
|---|------|----------|----------|-----------|
| 1 | Permanent name patterns | `config.json` → `probalance.exempt_patterns` | Saved | ProBalance tab list, **and** the Processes context menu |
| 2 | Session name patterns | `MainWindow::m_pbSessionExempt` → `ProcessMonitor::m_pbSessionExempt` | This run only | Processes context menu |
| 3 | Rule-based | `Rule::pbExempt` (JSON `"pb_exempt"`) | Saved with the rule | Rules tab |

List 1 is evaluated **inside** `ProBalance::isExempt()` from its own config copy.
Lists 2 and 3 are resolved to PIDs in `ProcessMonitor::run()` and passed as
`tick()`'s `exemptPids`. All three are honoured; matching any one is enough.

```cpp
QStringList sessionPats;
{ QMutexLocker lk(&m_configMux); sessionPats = m_pbSessionExempt; }
QSet<int> pbExempt;
for (const auto &proc : snapshot)
    if (m_ruleEngine->isPbExempt(proc.name)
     || ProBalance::nameMatchesAny(proc.name, sessionPats))
        pbExempt.insert(proc.pid);
m_proBalance->tick(snapshot, tickSec, pbExempt);
```

⚠️ **There used to be a fourth: a session-only `QSet<int>` of PIDs**
(`ProcessMonitor::setPbExempt`, `m_pbManualExempt`) behind the context menu. Nothing
persisted it, it never appeared in the ProBalance tab, and it covered one PID — so
exempting `firefox` left its `Isolated Web Co` children throttleable. That is how it
got reported as broken. The API is **gone**; do not reintroduce per-PID exemption.
Session scope is now a *name* list, list 2, which is what people actually want.

### Context menu (Processes tab)

A submenu, `ProBalance exemption for 'X'`, with two **checkable** actions —
"This session only" and "Permanent (saved to config)" — so current state is visible
rather than inferred from which verb the menu happens to be offering. They are
independent (both can be ticked) and emit two separate signals,
`pbExemptSessionToggled` / `pbExemptPermanentToggled`, landing on
`MainWindow::onPbExemptSessionToggle` / `onPbExemptPermanentToggle`.

`MainWindow::m_pbSessionExempt` is deliberately **not** in `m_config`, so
`saveConfig()` cannot leak session exemptions into `config.json`.
`refreshPbExemptPatterns()` is the one place that pushes both lists to the table and
the session list to the monitor — call it after anything that changes either.

### Becoming exempt while throttled — do not regress

`tick()` does **not** simply `continue` past an exempt process. A process can be
exempted *while it is throttled*; skipping it would strand it at the throttled nice
value for the life of the process, which looks exactly like "exempting it did
nothing". `tick()` restores `originalNice` and erases the state entry first.

### Table display

`ProcessTableWidget` holds both pattern lists (`setPbExemptPatterns(permanent,
session)`) and matches them itself with `ProBalance::nameMatchesAny`, so the row
colour, the Status column and the menu ticks agree by construction. Status is
"⚡ PB Exempt" for permanent, "⚡ PB Exempt (session)" for session-only; both teal.

`updateThrottled()` must be called **before** `updateSnapshot()` in
`MainWindow::onSnapshot()` — the latter is what repaints the rows, so setting it
after left the Status column one refresh stale. The exempt patterns are pushed on
change instead of per frame.

`MainWindow` reads the permanent patterns from **`m_config`**, never from
`m_proBalance`, which belongs to the monitor thread.

### Config handover is a monitor-thread job

`MainWindow` never calls `m_proBalance->updateConfig()` — ProBalance has no mutex and
`tick()` reads its config from the monitor thread. `ProcessMonitor::updateConfig()`
only sets `m_pbConfigDirty` under `m_configMux`; `run()` picks the pending
`probalance` object up and applies it itself. The right-click exemption writes config
on every click, so this race was no longer theoretical.

### Testing exemptions without launching the app

`tests/` holds three standalone harnesses, none of them in the CMake build; each
carries its own `g++` line in its header comment. `monitor-harness.cpp` needs a
`moc` pass on `processmonitor.h` (it is a QObject) — the header says how.

`tests/probalance-harness.cpp` — 29 checks over the state machine and the exempt-list
rules. Not in the CMake build; the `g++` line is in its header comment. It stubs
`Utils::setNice()`, so it touches nothing on the live desktop. **Extend this rather
than starting the app**, which enforces saved rules against every PID on Cesar's
running session. See also the memory `process-lasso-dont-launch-to-test`.

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
# Outputs: process-lasso-qt-1.5.0-x86_64.AppImage  (~68 MB)
#          process-lasso-qt-1.5.0-x86_64.AppImage.zsync  (~238 KB)
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
ProcessTableWidget::manualChangeApplied → MainWindow::onManualChange
ProcessTableWidget::pbExemptSessionToggled   → MainWindow::onPbExemptSessionToggle
ProcessTableWidget::pbExemptPermanentToggled → MainWindow::onPbExemptPermanentToggle
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
If you need GUI → engine communication, go through `ProcessMonitor::updateConfig()`,
which flags the change and lets `run()` hand it to ProBalance on its own thread.
(`MainWindow::onSnapshot()` still reads `m_proBalance->throttledPids()` from the GUI
thread — a pre-existing race, not one to copy.)

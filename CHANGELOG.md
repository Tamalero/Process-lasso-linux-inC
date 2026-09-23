# Changelog

All notable changes to Process Lasso Qt.

## [1.4.1] — 2026-09-23

### Fixed — the app became unresponsive, and rules looked like they did nothing

`RuleEngine::applyToProcess()` re-applied **and re-logged** every rule on every
enforcement pass, whether or not the value needed changing. With nine rules
matching ~150 browser processes at a 500 ms interval that is roughly **300 log
lines per second** pushed into the Log tab's text view on the GUI thread, plus a
`sched_setaffinity` for *every thread* of every one of those processes —
thousands of syscalls a second.

The GUI thread never caught up. Setting affinity from the context menu appeared
to do nothing, the process list stopped refreshing, and the Log tab was
unreadable. The affinity machinery was fine throughout; the UI simply could not
service input.

Rules now read the current affinity, nice and I/O priority first and skip the
write **and** the log line when the value is already correct. A rule that
genuinely needs applying still applies and still logs, exactly once. Steady
state is silent, so the Log tab shows only real changes.

Present since 1.3.3; it scaled with the number of matching processes, which is
why it surfaced now.

### Fixed — a manual affinity change could be reverted immediately

`m_manualOverrides` — the map that protects a manual affinity change from rule
re-enforcement for 30 seconds — was written from the GUI thread with no mutex
while the monitor thread iterated and erased from it twice a second. A dropped
insert means the next pass reverts the change; a `QHash` insert racing an
iteration can also corrupt the container outright. It is now guarded.

## [1.4.0] — 2026-09-23

### Added — session-only vs. permanent ProBalance exemptions

The Processes tab context menu now has a **ProBalance exemption for '*name*'**
submenu with two independent, checkable scopes:

- **This session only** — exempt until the app exits, never written to
  `config.json`. For "stop throttling this while I render / compile / play"
  without leaving anything behind.
- **Permanent (saved to config)** — adds the name to the ProBalance tab's
  **Exempt Processes** list and saves it.

Both tick boxes show the current state, so whether a process is exempt — and for
how long — is visible rather than inferred. Both exempt by **name**, so
exempting `firefox` covers its child processes too. Session-exempt processes show
`⚡ PB Exempt (session)` in the Status column.

The ProBalance tab now warns that exempt patterns match **anywhere** in a
process name: a short or generic entry (`sh`, or worse `e`) exempts most of the
machine, and the failure is silent — ProBalance just stops throttling anything.

### Fixed — "Exempt from ProBalance" did nothing visible

Right-clicking a process and choosing **Exempt 'X' from ProBalance** used to set a
session-only, per-PID flag. Nothing persisted it, it never appeared in the ProBalance
tab's exempt list, and it covered only that one PID — so a browser's other processes
stayed throttleable and the whole thing was gone on the next launch.

- The menu now writes the process name into the ProBalance tab's **Exempt Processes**
  list (`probalance.exempt_patterns`). It shows up in the tab immediately, is saved to
  `config.json`, covers every process of that name, and survives a restart.
- **Remove ProBalance Exemption** drops every pattern that matches the process, not
  just an exact-name entry — the list matches on "contains", so removing only the
  exact name could leave the process exempt while the menu said it was not.
- A process exempted **while it was throttled** is now un-niced. Previously ProBalance
  just stopped looking at it, stranding it at the throttled nice value for the life of
  the process — the most direct reason exempting appeared to do nothing.
- The **Status** column no longer lags a refresh behind; the teal "⚡ PB Exempt" marker
  now also reflects the tab's pattern list, not just the vanished per-PID set.
- ProBalance config changes are handed to the monitor thread instead of being written
  into ProBalance from the GUI thread while it ticks.

### Internal

- The exempt-list matching rule (case-insensitive substring) had been
  reimplemented in four files. It is now `ProBalance::nameMatches()` /
  `nameMatchesAny()` / `toggleExemptPattern()`, used by all of them.
- New `tests/probalance-harness.cpp`: 29 standalone checks over the ProBalance
  state machine and the exempt-list rules, with `Utils::setNice()` stubbed.
  Not part of the CMake build; the `g++` line is in its header comment.

## [1.3.3] — 2026-09-01

Rolls up 1.3.1 and 1.3.2, which were never published separately. Everything
below lands in one release on top of v1.3.0.

### Fixed — CPU parking vs affinity assignment

Three ways Gaming Mode's CPU parking conflicted with affinity assignment. The
kernel itself is well-behaved — `sched_setaffinity` stores the *requested* mask
and restores it when CPUs come back online — but the app defeated that.

- **Original affinities are no longer captured while CPUs are parked.**
  `sched_getaffinity` returns an already-truncated mask when CPUs are offline,
  and *Reset All Changes* wrote captured masks back with an explicit
  `sched_setaffinity` — which the kernel treats as a **new** request, pinning
  the process off those cores permanently. A process whose real affinity was
  all 32 CPUs was being recorded as 28.
- **Affinity failures are no longer silent.** The log line lived inside the
  success branch, so a rule whose CPUs were all parked did nothing and said
  nothing. It is now reported in the Log with the reason. Transient failures
  (a short-lived process exiting mid-scan) stay quiet — they are not actionable.
- **Parked CPUs are shown in red in the affinity picker**, and are now
  *selectable* rather than disabled: parking is temporary, while a rule or
  default affinity is saved config you may well be writing during Gaming Mode.
  Confirming with parked CPUs selected warns first, and warns more firmly when
  *every* selected CPU is parked.

### Added — crash detection and Safe Mode

- A marker at `~/.local/state/process-lasso/runstate.json` records that a
  session is in progress, and is cleared only after a fully completed shutdown.
- The marker carries the kernel's **boot id**, which distinguishes "crashed
  during this boot, parked CPUs are still stale" from "crashed, but the machine
  has rebooted since and there is nothing left to repair". A power loss needs no
  recovery — the reboot already brought every CPU back.
- **Safe Mode** after 3 consecutive unclean starts: rules, default affinity and
  ProBalance stop being *applied*, with a banner and a **Resume Normal** button.
  Your `config.json` is never modified — only its application is suppressed.
- Writes are `fsync`-ed and renamed atomically, so the marker survives a power
  cut rather than sitting in the page cache.

### Fixed — parked CPUs were never restored at exit

- **Gaming Mode used to leave CPUs offline after the app quit.** `unParkAll()`
  was reachable only from the Gaming Mode tab, so even a *clean* quit stranded
  them until you noticed and ran *Reset All Changes*. They are now restored on
  every shutdown the app can observe.
- **`SIGTERM`, `SIGINT` and `SIGHUP` now shut down cleanly** (systemd stop,
  logout, Ctrl-C) instead of killing the process outright. Config is saved and
  CPUs are unparked on those paths too.

### Known limitations

`SIGKILL` and power loss still cannot be caught. A `SIGKILL` in the same boot is
repaired at the next launch by the crash marker; a power loss needs no repair,
because the reboot restores every CPU on its own.

## [1.3.0] — 2026-08-06
- CPU and RAM temperature monitoring (per-core °C, status bar, tray tooltip).

## [1.2.0] — 2026-05-30
- ProBalance per-process exemptions; single-instance enforcement.

## [1.1.0] — 2026-05-04
- CPU graph UI polish; AppImage auto-update support.

## [1.0.0] — 2026-05-04
- Initial stable release.

[1.3.3]: https://github.com/Tamalero/Process-lasso-linux-inC/releases/tag/v1.3.3
[1.3.0]: https://github.com/Tamalero/Process-lasso-linux-inC/releases/tag/v1.3.0
[1.2.0]: https://github.com/Tamalero/Process-lasso-linux-inC/releases/tag/v1.2.0
[1.1.0]: https://github.com/Tamalero/Process-lasso-linux-inC/releases/tag/v1.1.0
[1.0.0]: https://github.com/Tamalero/Process-lasso-linux-inC/releases/tag/v1.0.0

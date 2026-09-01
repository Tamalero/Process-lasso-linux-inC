# Changelog

All notable changes to Process Lasso Qt.

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

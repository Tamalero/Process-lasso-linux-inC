# Changelog

All notable changes to Process Lasso Qt.

## [1.4.6] — 2026-09-23

### Fixed — "install-helper.sh not found in data directory"

Pressing **Install / Update Helper** while running the app from a source build
directory reported that, because the search only knew the installed layouts:
XDG data directories, and `<binary>/../share/process-lasso-qt/`. From
`<repo>/build` neither exists.

It now also looks in `<binary>/../packaging/` and `<binary>/packaging/`, so the
button works when running straight out of a checkout. When it still finds
nothing, the message lists **every path it searched** instead of a bare "not
found", which sent you looking for a missing file rather than a wrong
assumption.

### Fixed — the window opacity slider silently did nothing on Wayland

`setWindowOpacity()` is a no-op under Wayland. There is no window-opacity request
in the protocol, and Qt's Wayland plugin does not implement it — X11 does this by
setting the `_NET_WM_WINDOW_OPACITY` window property, which is why the same
slider works under X11 and XWayland.

The slider is now **disabled on Wayland with an explanation**, pointing at the
compositor instead (on KDE: Alt+scroll over the window). Dragging a control that
cannot do anything, with nothing to say why, is worse than not offering it.

### Internal

`config.cpp` wrote a `ui.opacity` default that nothing ever read — the real key
is top-level `window_opacity`. Removed, and the schema in `CLAUDE.md` corrected;
it had documented the dead key.

## [1.4.5] — 2026-09-23

### Fixed — installing the privileged helper never worked from an AppImage

`install-helper.sh` looked for the helper binary one directory too high. The
script is installed to `<prefix>/share/process-lasso-qt/`, and it resolved
`<script dir>/../bin/` — that is `<prefix>/share/bin/`, which exists in no
layout. The binary is at `<prefix>/bin/`, two levels up.

So **Gaming Mode → install helper failed every time** from an AppImage or a
packaged install, and had done since the path was introduced. Anyone whose
helper is installed got it there some other way.

The search now covers, in order: `<prefix>/bin` (installed and AppImage), the
parent's `bin/` (DESTDIR-style trees), and `build/` plus `build-appimage/` so the
script also works from a source checkout after a build — which it previously
could not, since `<repo>/bin` never exists.

It also now refuses to run as non-root up front, naming `pkexec`, instead of
failing further down with "cannot change owner"; and when no binary is found it
lists every path it searched.

### Added — packaging/uninstall-helper.sh

There was no supported way to remove the privileged helper and its sudoers rule
other than doing it by hand.

The sudoers rule is removed **first**, then `visudo -cqf /etc/sudoers` runs and
the script aborts before touching the binary if the configuration no longer
parses — a broken file under `/etc/sudoers.d` locks sudo out for everyone, so the
passwordless grant goes first and is proven safe before anything else happens.

It moves things aside into `/var/backups/process-lasso-uninstall-<timestamp>/`
rather than deleting them; pass `--purge` to delete. Safe to run when nothing is
installed.

Run it with `pkexec bash /usr/share/process-lasso-qt/uninstall-helper.sh`.

### Fixed — install-helper.sh was not marked executable

It was the only script in the repository tracked as non-executable, so
`./packaging/install-helper.sh` did not run despite having a shebang. The shipped
copies were unaffected — CMake and the PKGBUILD both force mode 755 at install
time.

## [1.4.4] — 2026-09-23

### Security — the sudoers rule was far too broad

`install-helper.sh` wrote `ALL ALL=(root) NOPASSWD: /usr/local/bin/process-lasso-helper`.
That let **every local account on the machine** run the privileged helper as root
without a password. Nothing about the application needed that.

The rule is now scoped to the single user who installs it, taken from
`PKEXEC_UID`/`SUDO_USER` (set by pkexec and sudo themselves), and validated with
`visudo -cqf` before installation so a malformed file can never lock out sudo.

**Existing installations keep the old permissive rule until you reinstall the
helper.** If you have other user accounts on this machine, reinstalling is worth
doing even if you do not need the new feature.

To be explicit about what is *not* claimed: the helper does not try to verify
that its caller is Process Lasso, because that is not possible — a caller
controls its own process image, so checking a parent's name or path is defeated
by `exec()` after `fork()`, or by copying the app binary. Limiting *who* may
invoke it is the control that actually works. The helper has no shell, no
`exec`, and takes no caller-supplied paths, so the worst a hostile caller
achieves is local denial of service, not privilege escalation.

### Added — rules can now apply affinity to processes you do not own

Setting CPU affinity on another user's process needs root, so a rule targeting
anything running as root — a Docker container, for instance — failed silently.

When a rule hits this, you are now asked once whether to apply it through the
privileged helper, with a **Remember this choice for this rule** option. Saying
yes stores `allow_helper` on the rule, so the grant is visible and revocable in
the Rules tab rather than hidden away. You are asked once per rule, never once
per process.

The new `set-affinity` helper command rejects cpulists containing anything but
digits, commas and dashes, cpulists naming a CPU that does not exist, pid 1, and
kernel threads.

### Fixed — "Failed to set affinity" now says why

The message was identical whether the process belonged to another user, every
requested CPU was parked, or the process had just exited. It now names the
cause, and the owner when the cause is ownership. Rule-driven failures are
reported once per rule and process rather than on every enforcement pass.

## [1.4.3] — 2026-09-23

### Fixed — two rules for the same process fought each other, flooding the log

Nothing stopped you adding a second rule for an application that already had
one, and *Add Rule for '<name>'…* in the Processes tab made it easy to do by
accident. When both rules set the same attribute to different values they
overwrote each other on every enforcement pass — twice a second, forever, each
flip writing a log line. That is the same GUI-starvation flood fixed in 1.4.1,
reached by a different route, and the 1.4.1 fix could not damp it: that one
skips writes when the value already matches, and here the value really did
change every time.

- **The first matching rule now wins, per attribute.** The first enabled rule
  that defines affinity, priority or I/O priority claims it; later rules are
  ignored for that attribute only. "One rule sets affinity, another sets
  priority" still combines exactly as before.
- **Shadowed settings are shown struck through** in the Rules tab, in grey, with
  a tooltip explaining which rule claimed them. A setting that sits in your
  config but can never apply should not look live.
- **Adding a duplicate now warns first**, naming the existing rule and which
  settings clash, and offers to edit that rule instead. This covers adding from
  the Rules tab, adding from the Processes tab, and editing a rule's pattern
  into a duplicate.

Existing configurations with duplicate rules are fixed by the first change
alone — no clean-up needed.

## [1.4.2] — 2026-09-23

### Added — edit a rule without leaving the Processes tab

New **Overwrite matching rules** checkbox beside the Processes tab filter box
(off by default, `ui.overwrite_matching_rules`).

Setting affinity, priority or I/O priority on a process that an existing rule
already covers used to be futile: the rule reclaimed it 30 seconds later and the
only real fix was a trip to the Rules tab. With the toggle on, the app offers to
change the rule itself instead.

You are always asked first, and the prompt names the rule and **how many running
processes it affects** — a rule covers every process matching its pattern, so
editing one from a single row can repoint a browser's whole process tree.
Choosing *Just this process* keeps your change on that one PID for as long as it
lives and leaves the rule untouched.

### Fixed — manual changes that looked like they were being reverted

They were not. The value reached the kernel every time; the screen was stale.

- All three context-menu dialogs were seeded from the **table cell** rather than
  from `/proc`, so reopening one showed the value from before your change — up
  to two seconds old, or indefinitely old while the GUI thread was starved by
  the log flood fixed in 1.4.1. They now read live values.
- The **I/O priority dialog was hardcoded** to "class 2, level 4". It never
  showed what the process was actually set to, and silently proposed changing it
  to that. It now shows the real value.
- The table waited up to two seconds to show a manual change. The affected row
  is now re-read and repainted immediately.

### Fixed — nice and I/O priority had no protection at all

Only affinity reported a manual change to the monitor, so a hand-set **nice** or
**I/O priority** was reverted by a matching rule on the very next enforcement
pass, about half a second later, with none of the 30-second grace affinity got.
All three now report.

### Changed

`setManualAffinityOverride` is now `setManualOverride` — it always suppressed the
whole rule application, not just affinity, and the old name hid that. A duration
of `0` means indefinite, used by *Just this process*. Overrides are dropped when
their process exits, so an indefinite one cannot outlive it and catch a recycled
PID.

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

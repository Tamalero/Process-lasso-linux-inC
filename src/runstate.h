#pragma once
#include <QString>

// Crash / unclean-shutdown detection.
//
// A marker file records "a session is in progress". It is armed at startup and
// cleared only on a fully completed shutdown, so finding it still armed on the
// next start means the previous run died without cleaning up.
//
// The marker also carries the kernel's boot id, which separates the two cases
// that need very different handling:
//
//   armed + same boot id  → we died during THIS boot; parked CPUs and any other
//                           kernel-side state are still stale and real.
//   armed + different id  → we died, but the machine has rebooted since. The
//                           reboot already restored every CPU, so there is
//                           nothing to repair — just clear the marker.
//
// That second case is what a plain clean/dirty flag gets wrong: after a power
// loss it reports "dirty" and triggers a recovery the reboot already performed.

struct RunStateInfo {
    bool hadPreviousRun   = false;  // a marker file existed at all
    bool previousWasClean = true;   // it recorded a completed shutdown
    bool sameBoot         = false;  // its boot id matches the running kernel
    int  crashCount       = 0;      // consecutive unclean starts, post-increment
    bool safeMode         = false;  // crashCount >= SAFE_MODE_THRESHOLD
};

namespace RunState {

// Consecutive unclean starts before config stops being applied.
inline constexpr int SAFE_MODE_THRESHOLD = 3;
// How long a session must survive before its config is considered trustworthy.
// Without a time gate, a loop that dies after ten seconds would reset the
// counter every time and never reach safe mode.
inline constexpr int HEALTHY_UPTIME_MS = 60000;

QString path();

// Read the previous marker and immediately re-arm it for this session.
//
// **Must be called before any config is applied.** Everything between reading
// the marker and re-arming it is an unprotected window: a crash in that gap
// would leave the old value on disk and the next run would draw the wrong
// conclusion — which is exactly the stale-marker loop this guards against.
RunStateInfo beginSession();

// Zero the crash counter once the session has proven stable. Leaves the marker
// armed. Not called automatically while safe mode is active — safe mode stays
// sticky until the user acknowledges it.
void markHealthy();

// Record a completed shutdown. Call LAST, after remediation has actually
// succeeded: marking clean before releasing hardware records a lie that the
// next run will trust.
void markClean();

} // namespace RunState

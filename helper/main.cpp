// Privileged sysfs helper for Process Lasso.
// Installed to /usr/local/bin/process-lasso-helper with SUID root
// (or via sudoers NOPASSWD rule).
//
// Commands:
//   cpu-online   <n> <0|1>           – bring CPU n online or offline
//   cpu-unpark-all                   – bring all offline CPUs online
//   renice-pid   <nice_val> <pid>    – set process nice (supports negative values)
//   set-affinity <cpulist> <pid>     – sched_setaffinity on every thread of pid
//   --check-only                     – exit 0 (used to verify sudo access)
//
// SECURITY NOTE. This binary runs as root via a NOPASSWD sudoers rule, so it
// must be treated as reachable by any process of any user that rule allows.
//
// It deliberately does NOT try to verify that its caller is Process Lasso.
// That cannot be done: a caller controls its own process image, so checking
// the parent's name or path is defeated by exec()ing after fork (a TOCTOU
// race), by copying the application binary, or by setting argv[0]. A check
// like that would buy nothing but false confidence.
//
// What contains the risk instead:
//   * a fixed, tiny command set — no shell, no exec, no caller-supplied paths;
//   * every argument validated before use, and sysfs paths built from a
//     validated integer with %d, never from caller text;
//   * none of these operations can grant code execution or change credentials.
//     The worst a hostile caller achieves is local denial of service (parking
//     CPUs, renicing, or repinning processes) — bad, but not privilege
//     escalation;
//   * the sudoers rule written by install-helper.sh is scoped to the single
//     user who installed it, not to ALL.
// Keep every one of those properties when adding a command.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <sys/resource.h>
#include <sys/types.h>
#include <sched.h>
#include <dirent.h>
#include <unistd.h>

static bool isUnsignedInt(const char *s)
{
    if (!s || !*s) return false;
    for (; *s; ++s) if (!isdigit((unsigned char)*s)) return false;
    return true;
}

static bool isSignedInt(const char *s)
{
    if (!s || !*s) return false;
    if (*s == '-') ++s;
    if (!*s) return false;
    for (; *s; ++s) if (!isdigit((unsigned char)*s)) return false;
    return true;
}

static int writeSysfsOnline(int cpuNum, int value)
{
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/online", cpuNum);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 1; }
    fprintf(f, "%d\n", value);
    fclose(f);
    return 0;
}

// Parse a cpulist string "0-3,5,8-11" and bring each CPU online.
static int unParkAll()
{
    FILE *f = fopen("/sys/devices/system/cpu/offline", "r");
    if (!f) return 0; // Nothing offline
    char buf[4096] = {};
    fgets(buf, sizeof(buf), f);
    fclose(f);

    // Trim trailing newline
    const size_t len = strlen(buf);
    if (len && buf[len-1] == '\n') buf[len-1] = '\0';
    if (buf[0] == '\0') return 0;

    // Parse cpulist
    char *tok = strtok(buf, ",");
    while (tok) {
        char *dash = strchr(tok, '-');
        if (dash) {
            *dash = '\0';
            int lo = atoi(tok);
            int hi = atoi(dash + 1);
            for (int c = lo; c <= hi; ++c) writeSysfsOnline(c, 1);
        } else {
            writeSysfsOnline(atoi(tok), 1);
        }
        tok = strtok(nullptr, ",");
    }
    return 0;
}

// Parse "0-3,5,8-11" into a cpu_set_t. Returns the number of CPUs set, or -1.
static int parseCpuList(const char *text, cpu_set_t *out)
{
    if (!text || !*text || strlen(text) > 255) return -1;
    for (const char *p = text; *p; ++p)
        if (!isdigit((unsigned char)*p) && *p != ',' && *p != '-') return -1;

    const long confd = sysconf(_SC_NPROCESSORS_CONF);
    const int maxCpu = (confd > 0 && confd < CPU_SETSIZE) ? (int)confd : CPU_SETSIZE;

    char buf[256];
    snprintf(buf, sizeof(buf), "%s", text);
    CPU_ZERO(out);
    int count = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        char *dash = strchr(tok, '-');
        int lo, hi;
        if (dash) { *dash = '\0'; lo = atoi(tok); hi = atoi(dash + 1); }
        else      { lo = hi = atoi(tok); }
        if (lo < 0 || hi < lo || hi >= maxCpu) return -1;
        for (int c = lo; c <= hi; ++c) { CPU_SET(c, out); ++count; }
    }
    return count ? count : -1;
}

// Refuse pid 1 and kernel threads. Neither is a thing a process manager should
// be repinning on someone's behalf, and both are easy to name by accident with
// a "contains" rule.
static int pidIsAcceptableTarget(int pid)
{
    if (pid <= 1) return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    char target[16];
    // Kernel threads have no exe link; readlink fails with ENOENT for them
    // even as root.
    return readlink(path, target, sizeof(target)) >= 0 || errno != ENOENT;
}

static int setAffinity(const char *cpulist, int pid)
{
    cpu_set_t mask;
    const int n = parseCpuList(cpulist, &mask);
    if (n < 0) { fprintf(stderr, "Invalid cpulist\n"); return 1; }
    if (!pidIsAcceptableTarget(pid)) {
        fprintf(stderr, "Refusing pid %d (init or kernel thread)\n", pid);
        return 1;
    }

    char taskDir[64];
    snprintf(taskDir, sizeof(taskDir), "/proc/%d/task", pid);
    DIR *d = opendir(taskDir);
    if (!d) { perror(taskDir); return 1; }

    int anyOk = 0, firstErr = 0;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        if (!isUnsignedInt(e->d_name)) continue;
        const int tid = atoi(e->d_name);
        errno = 0;
        if (sched_setaffinity(tid, sizeof(mask), &mask) == 0) anyOk = 1;
        else if (!firstErr) firstErr = errno;
    }
    closedir(d);

    if (!anyOk) {
        fprintf(stderr, "set-affinity failed: %s\n",
                strerror(firstErr ? firstErr : ESRCH));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: process-lasso-helper <command> [args...]\n");
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "--check-only") == 0) return 0;

    if (strcmp(cmd, "cpu-online") == 0) {
        if (argc != 4) { fprintf(stderr, "cpu-online: expected <cpu> <0|1>\n"); return 1; }
        if (!isUnsignedInt(argv[2])) { fprintf(stderr, "Invalid CPU number\n"); return 1; }
        if (strcmp(argv[3], "0") != 0 && strcmp(argv[3], "1") != 0) {
            fprintf(stderr, "Value must be 0 or 1\n"); return 1;
        }
        const int cpuNum = atoi(argv[2]);
        const int value  = atoi(argv[3]);
        return writeSysfsOnline(cpuNum, value);
    }

    if (strcmp(cmd, "cpu-unpark-all") == 0) return unParkAll();

    if (strcmp(cmd, "set-affinity") == 0) {
        if (argc != 4) { fprintf(stderr, "set-affinity: expected <cpulist> <pid>\n"); return 1; }
        if (!isUnsignedInt(argv[3])) { fprintf(stderr, "Invalid PID\n"); return 1; }
        return setAffinity(argv[2], atoi(argv[3]));
    }

    if (strcmp(cmd, "renice-pid") == 0) {
        if (argc != 4) { fprintf(stderr, "renice-pid: expected <nice> <pid>\n"); return 1; }
        if (!isSignedInt(argv[2])) { fprintf(stderr, "Invalid nice value\n"); return 1; }
        if (!isUnsignedInt(argv[3])) { fprintf(stderr, "Invalid PID\n"); return 1; }
        const int niceVal = atoi(argv[2]);
        const int pid     = atoi(argv[3]);
        if (niceVal < -20 || niceVal > 19) {
            fprintf(stderr, "Nice value out of range [-20, 19]\n"); return 1;
        }
        if (setpriority(PRIO_PROCESS, (unsigned)pid, niceVal) != 0) {
            perror("setpriority"); return 1;
        }
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    return 1;
}

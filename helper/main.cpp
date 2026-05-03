// Privileged sysfs helper for Process Lasso.
// Installed to /usr/local/bin/process-lasso-helper with SUID root
// (or via sudoers NOPASSWD rule).
//
// Commands:
//   cpu-online   <n> <0|1>           – bring CPU n online or offline
//   cpu-unpark-all                   – bring all offline CPUs online
//   renice-pid   <nice_val> <pid>    – set process nice (supports negative values)
//   --check-only                     – exit 0 (used to verify sudo access)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <sys/resource.h>
#include <sys/types.h>
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

#pragma once
#include <cstdio>

// Enabled with --verbose on the command line.
extern bool gVerbose;

#define VLOG(fmt, ...) \
    do { if (gVerbose) fprintf(stderr, "[V] " fmt "\n", ##__VA_ARGS__); } while (0)

#!/bin/bash
# run.sh — launch process-lasso-qt from the build directory
# Run this once with --install-helper (needs sudo) to enable privileged features,
# then run without arguments for normal use.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY="$SCRIPT_DIR/build/process-lasso-qt"
HELPER_SRC="$SCRIPT_DIR/build/process-lasso-helper"
HELPER_DST="/usr/local/bin/process-lasso-helper"
SUDOERS_FILE="/etc/sudoers.d/process-lasso"

if [[ ! -x "$BINARY" ]]; then
    echo "ERROR: $BINARY not found. Run: cmake --build build -j\$(nproc)" >&2
    exit 1
fi

if [[ "${1:-}" == "--install-helper" ]]; then
    if [[ $EUID -ne 0 ]]; then
        echo "Re-running with sudo to install helper..."
        exec sudo bash "$0" --install-helper
    fi
    install -o root -g root -m 0755 "$HELPER_SRC" "$HELPER_DST"
    cat > "$SUDOERS_FILE" <<'SUDOERS'
# process-lasso-qt: allow privileged helper without password
ALL ALL=(root) NOPASSWD: /usr/local/bin/process-lasso-helper
SUDOERS
    chmod 0440 "$SUDOERS_FILE"
    echo "Helper installed to $HELPER_DST"
    echo "Sudoers rule written to $SUDOERS_FILE"
    echo "You can now run ./run.sh without --install-helper"
    exit 0
fi

# Check helper status
if [[ ! -x "$HELPER_DST" ]]; then
    echo "NOTE: Privileged helper not installed."
    echo "      CPU parking and negative nice values will not work."
    echo "      Run:  sudo ./run.sh --install-helper"
    echo ""
fi

exec "$BINARY" "$@"

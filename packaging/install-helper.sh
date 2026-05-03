#!/bin/bash
# install-helper.sh — run via pkexec to install the privileged helper
# Usage: pkexec bash /path/to/install-helper.sh /path/to/process-lasso-helper
set -euo pipefail

HELPER_SRC="${1:-$(dirname "$0")/../bin/process-lasso-helper}"
HELPER_DST="/usr/local/bin/process-lasso-helper"
SUDOERS_FILE="/etc/sudoers.d/process-lasso"

if [[ ! -f "$HELPER_SRC" ]]; then
    echo "ERROR: helper binary not found at $HELPER_SRC" >&2
    exit 1
fi

install -o root -g root -m 0755 "$HELPER_SRC" "$HELPER_DST"

# Write sudoers rule allowing the current user (or any user) to run the helper
# without a password prompt.  Restrict to specific allowed commands only.
cat > "$SUDOERS_FILE" <<'SUDOERS'
# process-lasso-qt: allow privileged helper without password
ALL ALL=(root) NOPASSWD: /usr/local/bin/process-lasso-helper
SUDOERS

chmod 0440 "$SUDOERS_FILE"

echo "process-lasso-helper installed successfully."

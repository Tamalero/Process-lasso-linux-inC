#!/bin/bash
# install-helper.sh — run via pkexec to install the privileged helper
# Usage: pkexec bash /path/to/install-helper.sh
# The helper binary is expected at ../bin/process-lasso-helper relative to this script.
set -euo pipefail

HELPER_SRC="$(cd "$(dirname "$0")/.." && pwd)/bin/process-lasso-helper"
HELPER_DST="/usr/local/bin/process-lasso-helper"
SUDOERS_FILE="/etc/sudoers.d/process-lasso"

if [[ ! -f "$HELPER_SRC" ]]; then
    echo "ERROR: helper binary not found at $HELPER_SRC" >&2
    exit 1
fi

install -o root -g root -m 0755 "$HELPER_SRC" "$HELPER_DST"

# Who asked for this install? We run as root under pkexec (or sudo), so the
# real user has to come from the environment those set themselves -- it is not
# caller-supplied data.
TARGET_USER=""
if [[ -n "${PKEXEC_UID:-}" ]]; then
    TARGET_USER="$(getent passwd "$PKEXEC_UID" | cut -d: -f1 || true)"
fi
if [[ -z "$TARGET_USER" && -n "${SUDO_USER:-}" ]]; then
    TARGET_USER="$SUDO_USER"
fi

# The sudoers rule grants PASSWORDLESS ROOT execution of the helper. Scope it to
# the one account that installed it.
#
# It used to read "ALL ALL=(root) NOPASSWD: ..." -- every local account on the
# machine could run it as root with no password. Nothing about the app needed
# that; it was simply too broad. Do not widen it again.
#
# The helper cannot tell whether its caller is really Process Lasso -- see the
# security note at the top of helper/main.cpp -- so limiting WHO may invoke it
# is the control that actually does work.
if [[ -n "$TARGET_USER" ]]; then
    SUDOERS_WHO="$TARGET_USER"
else
    echo "WARNING: could not determine the installing user; falling back to ALL." >&2
    echo "         Edit $SUDOERS_FILE and replace ALL with your username." >&2
    SUDOERS_WHO="ALL"
fi

TMP_SUDOERS="$(mktemp)"
cat > "$TMP_SUDOERS" <<SUDOERS
# process-lasso-qt: allow the privileged helper without a password.
# Scoped to one user on purpose -- see packaging/install-helper.sh.
$SUDOERS_WHO ALL=(root) NOPASSWD: /usr/local/bin/process-lasso-helper
SUDOERS

# Never install a sudoers file that does not parse: a malformed one can lock
# sudo out for everybody.
if ! visudo -cqf "$TMP_SUDOERS"; then
    echo "ERROR: generated sudoers file is invalid; not installing it." >&2
    rm -f "$TMP_SUDOERS"
    exit 1
fi

install -o root -g root -m 0440 "$TMP_SUDOERS" "$SUDOERS_FILE"
rm -f "$TMP_SUDOERS"

echo "process-lasso-helper installed successfully."
echo "sudoers rule scoped to: $SUDOERS_WHO"

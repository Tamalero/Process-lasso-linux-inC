#!/bin/bash
# uninstall-helper.sh — remove the privileged helper and its sudoers rule.
# Usage: pkexec bash /path/to/uninstall-helper.sh [--purge]
#
# Counterpart to install-helper.sh. Safe to run when nothing is installed.
#
# By default this MOVES things aside into a timestamped directory under
# /var/backups rather than deleting them, and prints where they went. Pass
# --purge to delete outright. The default is reversible on purpose: a sudoers
# file is a thing you want back if removing it turns out to have been a mistake.
set -euo pipefail

HELPER="/usr/local/bin/process-lasso-helper"
SUDOERS_FILE="/etc/sudoers.d/process-lasso"
PURGE=0
[[ "${1:-}" == "--purge" ]] && PURGE=1

if [[ $EUID -ne 0 ]]; then
    echo "ERROR: must run as root (use pkexec or sudo)." >&2
    exit 1
fi

BACKUP_DIR="/var/backups/process-lasso-uninstall-$(date +%Y%m%d-%H%M%S)"
removed_any=0

take() {  # take <path> <description>
    local path="$1" what="$2"
    [[ -e "$path" ]] || { echo "  $what: already absent"; return; }
    if [[ $PURGE -eq 1 ]]; then
        rm -f -- "$path"
        echo "  $what: deleted ($path)"
    else
        mkdir -p "$BACKUP_DIR"
        # Move OUT of the directory, never rename in place: sudo ignores files
        # in sudoers.d whose name contains a dot, so a ".bak" left there would
        # look inert while still being a file someone has to reason about later.
        mv -- "$path" "$BACKUP_DIR/"
        echo "  $what: moved to $BACKUP_DIR/$(basename "$path")"
    fi
    removed_any=1
}

echo "Removing the Process Lasso privileged helper…"

# Sudoers first. If anything goes wrong afterwards, the important thing — the
# passwordless root grant — is already gone.
take "$SUDOERS_FILE" "sudoers rule"

# A broken file anywhere under /etc/sudoers.d can lock sudo out for everyone, so
# prove the remaining configuration still parses before touching anything else.
if ! visudo -cqf /etc/sudoers; then
    echo "ERROR: /etc/sudoers no longer parses cleanly. NOT continuing." >&2
    echo "       Restore from $BACKUP_DIR and check with 'visudo -c'." >&2
    exit 1
fi

take "$HELPER" "helper binary"

if [[ $removed_any -eq 0 ]]; then
    echo "Nothing to do — the helper was not installed."
    exit 0
fi

echo
echo "Done. CPU parking, renicing other users' processes, and setting affinity"
echo "on processes you do not own will no longer work until the helper is"
echo "reinstalled. Everything else is unaffected."
if [[ $PURGE -eq 0 ]]; then
    echo
    echo "Nothing was deleted. To restore:  sudo mv $BACKUP_DIR/* /etc/sudoers.d/ …"
    echo "To remove permanently, re-run with --purge."
fi

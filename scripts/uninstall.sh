#!/usr/bin/env bash
# Removes DittoSR from a folder install.sh (or install.ps1) installed it into: AMD's files back from
# <folder>/dittosr-backup/ (after a hash check), DittoSR's own files removed.
#   uninstall.sh <folder>
set -euo pipefail

dir="${1:?usage: uninstall.sh <the folder install.sh installed into>}"
[ -d "$dir" ] || { echo "not found: $dir" >&2; exit 1; }
backup="$dir/dittosr-backup"
for n in amd_fidelityfx_dx12.dll amd_fidelityfx_loader_dx12.dll amd_fidelityfx_vk.dll amd_fidelityfx_upscaler_dx12.dll; do
    target="$dir/$n"
    saved="$backup/$n"
    orig="$dir/${n%.dll}.orig.dll"
    if [ -f "$saved" ]; then
        want="$(tr -d '[:space:]' < "$saved.sha256" | tr 'A-F' 'a-f')"
        if [ "$(sha256sum "$saved" | cut -d ' ' -f 1)" != "$want" ]; then
            echo "the backup of $n does not match its hash: verify the game's files in Steam instead" >&2
            exit 1
        fi
        cp -f "$saved" "$target"
        rm -f "$orig"
        echo "restored AMD's $n"
    elif [ -f "$target" ] && grep -aq "DittoSR" "$target"; then
        # (installed alone - no AMD file was there)
        rm -f "$target"
        echo "removed DittoSR's $n"
    fi
done
rm -f "$dir/dittosr.ini" "$dir/dittosr.log"
echo "done (the backups, if any, stay in $backup)"

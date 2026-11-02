#!/usr/bin/env bash
# Removes DittoSR from a folder install.sh (or install.ps1, or the loader) installed it into: the game's files back from
# <folder>/dittosr-backup/ (after a hash check) - AMD's, and Intel's libxess.dll - DittoSR's own files removed.
#   uninstall.sh <folder>
set -euo pipefail

dir="${1:?usage: uninstall.sh <the folder install.sh installed into>}"
[ -d "$dir" ] || { echo "not found: $dir" >&2; exit 1; }
backup="$dir/dittosr-backup"
for n in amd_fidelityfx_dx12.dll amd_fidelityfx_loader_dx12.dll amd_fidelityfx_vk.dll amd_fidelityfx_upscaler_dx12.dll libxess.dll; do
    target="$dir/$n"
    saved="$backup/$n"
    orig="$dir/${n%.dll}.orig.dll"
    [ "$n" = libxess.dll ] && orig="$dir/libxess_orig.dll"
    if [ -f "$saved" ]; then
        want="$(tr -d '[:space:]' < "$saved.sha256" | tr 'A-F' 'a-f')"
        if [ "$(sha256sum "$saved" | cut -d ' ' -f 1)" != "$want" ]; then
            echo "the backup of $n does not match its hash: verify the game's files in Steam instead" >&2
            exit 1
        fi
        cp -f "$saved" "$target"
        rm -f "$orig"
        echo "restored the game's $n"
    elif [ -f "$target" ] && grep -aq "DittoSR" "$target"; then
        if [ -f "$orig" ]; then
            # (no backup: the game's copy)
            mv -f "$orig" "$target"
            echo "restored the game's $n from its copy"
        else
            # (installed alone - no game file was there)
            rm -f "$target"
            echo "removed DittoSR's $n"
        fi
    fi
done
rm -f "$dir/dittosr.ini" "$dir/dittosr.log" "$dir/dittosr-xess.log"
echo "done (the backups, if any, stay in $backup)"

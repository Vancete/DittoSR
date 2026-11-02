#!/usr/bin/env bash
# Installs DittoSR into a Windows game run through Proton (SteamOS, Linux) - install.ps1's counterpart.
#   install.sh <folder | Steam app id> [--name amd_fidelityfx_dx12.dll | amd_fidelityfx_loader_dx12.dll | amd_fidelityfx_vk.dll]
#              [--build <build folder>]
# The folder: the one with the game's FidelityFX DLL, or any folder above it (the game's install folder): the DLLs are
# looked for below it. A Steam app id (the number in the store page's address): the game's install folder, found in
# Steam's libraries. For each of amd_fidelityfx_dx12.dll (FidelityFX SDK 1.1, FSR 3.1), amd_fidelityfx_loader_dx12.dll
# (SDK 2.x), amd_fidelityfx_vk.dll (FSR 3.1 on Vulkan) and amd_fidelityfx_upscaler_dx12.dll where the folder has neither
# of the first two (an SDK 2.x game with the loader built into its executable) found, AMD's file is backed up to
# <folder>/dittosr-backup/ (with its SHA-256), copied to <name>.orig.dll (what DittoSR hands frame generation and the
# FSR toggle to) and replaced by DittoSR - the same DLL serves D3D12 and Vulkan. With none of them anywhere below the folder, DittoSR goes in as
# --name alone (OptiScaler, a game that loads it optionally). dittosr.ini goes next to it (kept if already there). Run
# it again after a rebuild: the backups and the originals' copies are left as they are.
# Proton loads a game folder's DLL by itself (Wine has none of these names); leave PROTON_FSR4_UPGRADE off, it puts
# its own amd_fidelityfx_dx12.dll there.
set -euo pipefail

names=(amd_fidelityfx_dx12.dll amd_fidelityfx_loader_dx12.dll amd_fidelityfx_vk.dll)
upscaler=amd_fidelityfx_upscaler_dx12.dll
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$here/../build"
name="amd_fidelityfx_dx12.dll"
dir=""
while [ $# -gt 0 ]; do
    case "$1" in
        --name) name="$2"; shift 2 ;;
        --build) build="$2"; shift 2 ;;
        -h|--help) sed -n '2,4p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) [ -z "$dir" ] || { echo "one folder or app id only: $1" >&2; exit 1; }; dir="$1"; shift ;;
    esac
done
[ -n "$dir" ] || { echo "usage: install.sh <folder | Steam app id> [--name <dll>] [--build <folder>]" >&2; exit 1; }
dll="$build/amd_fidelityfx_dx12.dll"
[ -f "$dll" ] || { echo "DittoSR's DLL not found: $dll (run the script from the extracted DittoSR.zip)" >&2; exit 1; }

# a Steam app id: its install folder, in whichever library holds it
if [[ "$dir" =~ ^[0-9]+$ ]] && [ ! -d "$dir" ]; then
    id="$dir"; dir=""
    for steam in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        vdf="$steam/steamapps/libraryfolders.vdf"
        [ -f "$vdf" ] || continue
        while IFS= read -r lib; do
            acf="$lib/steamapps/appmanifest_$id.acf"
            [ -f "$acf" ] || continue
            sub="$(sed -n 's/^[[:space:]]*"installdir"[[:space:]]*"\(.*\)".*/\1/p' "$acf" | head -n 1)"
            [ -n "$sub" ] && [ -d "$lib/steamapps/common/$sub" ] && { dir="$lib/steamapps/common/$sub"; break 2; }
        done < <(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)".*/\1/p' "$vdf"; echo "$steam")
    done
    [ -n "$dir" ] || { echo "app $id is not installed in any Steam library found" >&2; exit 1; }
    echo "app $id: $dir"
fi
[ -d "$dir" ] || { echo "not found: $dir" >&2; exit 1; }

# the folder(s) holding a FidelityFX DLL, at or below the one given
found=()
while IFS= read -r d; do found+=("$d"); done < <(
    find "$dir" \( -iname "${names[0]}" -o -iname "${names[1]}" -o -iname "${names[2]}" -o -iname "$upscaler" \) -type f \
        -printf '%h\n' 2>/dev/null | grep -v '/dittosr-backup$' | sort -u)
if [ ${#found[@]} -gt 1 ]; then
    echo "FidelityFX DLLs in more than one folder - run it again with the one the game loads:" >&2
    printf '  %s\n' "${found[@]}" >&2
    exit 1
fi
[ ${#found[@]} -eq 1 ] && dir="${found[0]}"

backup="$dir/dittosr-backup"
targets=()
for n in "${names[@]}"; do [ -f "$dir/$n" ] && targets+=("$n"); done
# (the game loads the upscaler itself)
if [ ! -f "$dir/${names[0]}" ] && [ ! -f "$dir/${names[1]}" ] && [ -f "$dir/$upscaler" ]; then targets+=("$upscaler"); fi
if [ ${#targets[@]} -eq 0 ]; then targets=("$name"); echo "no FidelityFX DLL in $dir: DittoSR alone as $name"; fi
for n in "${targets[@]}"; do
    target="$dir/$n"
    orig="$dir/${n%.dll}.orig.dll"
    if [ -f "$target" ] && [ ! -f "$orig" ]; then
        # the first install: AMD's file, before anything replaces it (no Authenticode check here: AMD's name in the
        # file, and not DittoSR's)
        if grep -aq "DittoSR" "$target" || ! grep -aq "Advanced Micro Devices" "$target"; then
            echo "$target is not AMD's: restore the game's file first (Steam: Properties > Installed Files > Verify)" >&2
            exit 1
        fi
        mkdir -p "$backup"
        cp "$target" "$backup/$n"
        # (upper case, as install.ps1 writes it: either uninstall script reads it)
        sha256sum "$target" | cut -d ' ' -f 1 | tr 'a-f' 'A-F' > "$backup/$n.sha256"
        cp "$target" "$orig"
        echo "backed up $n to $backup, AMD's copy as $orig"
    fi
    cp -f "$dll" "$target"
    echo "installed DittoSR as $target"
done
ini="$dir/dittosr.ini"
[ -f "$ini" ] || cp "$here/../dittosr.ini" "$ini"
echo "settings: $ini; log: $dir/dittosr.log"

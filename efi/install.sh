#!/bin/sh
# Install BroadTek into the OpenCore EFI: the kext next to Lilu, its
# config.plist entry after Lilu, and the boot-arg -broadtekdbg. Run it again
# to update. Nothing is loaded now; it takes effect at the next restart.
#
#   sudo efi/install.sh [--stage default|off|events|full|classdry|class|advaddr|fulladv] [EFI volume]
#
# The volume is a mounted path, a partition UUID or diskNsM. Without one it
# finds the volume with OpenCore on it; if there is more than one it asks
# which. The config as it was before the first install is kept as
# EFI/OC/config.plist.pre-broadtek. efi/uninstall.sh undoes it.
#
# --stage says how much the plugin may change, and defaults to the standard advertising fixes:
#   default apply the standard advertising fixes
#   off     observe only, nothing is rewritten
#   events  rewrite events only: the features rewrite and nothing else
#   full    rewrite commands as well
#   advaddr observe, and send each advertising set its private address (0x2035)
#   fulladv rewrite commands and address the advertising sets: both at once
set -eu

stage=default
while [ $# -gt 0 ]; do
    case "$1" in
        --stage) [ $# -ge 2 ] || { echo "--stage needs a value" >&2; exit 1; }
                 stage="$2"; shift 2 ;;
        --stage=*) stage="${1#--stage=}"; shift ;;
        *) break ;;
    esac
done
case "$stage" in
    default|off|events|full|classdry|class|advaddr|fulladv) ;;
    *) echo "unknown stage '$stage': expected default, off, events, full, classdry, class, advaddr or fulladv" >&2; exit 1 ;;
esac

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo $0" >&2; exit 1; }
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

ours=
for d in "$root" "$root/build/out"; do
    if [ -f "$d/BroadTek.kext/Contents/MacOS/BroadTek" ]; then ours="$d/BroadTek.kext"; break; fi
done
[ -n "$ours" ] || { echo "BroadTek.kext not found: build it first (make kext)" >&2; exit 1; }

. "$here/find_efi.sh"
find_efi "${1:-}"
oc="$EFI_MOUNT/EFI/OC"
echo "OpenCore: $oc ($EFI_PART)"
[ -d "$oc/Kexts/Lilu.kext" ] || { echo "Lilu.kext is not in $oc/Kexts; BroadTek is a Lilu plugin. Nothing was changed." >&2; exit 1; }

tmp="$(mktemp -t config.plist)"
trap 'rm -f "$tmp"' EXIT
python3 "$here/configure.py" install "$oc/config.plist" "$tmp" "$stage"
plutil -lint "$tmp" >/dev/null || { echo "the new config does not parse; nothing was changed" >&2; exit 1; }

efi_room "$ours"

backup="$oc/config.plist.pre-broadtek"
[ -e "$backup" ] || { cp "$oc/config.plist" "$backup"; echo "saved the config as it was: $backup"; }

export COPYFILE_DISABLE=1
rm -rf "$oc/Kexts/BroadTek.kext"
cp -R -X "$ours" "$oc/Kexts/"
build="$(git -C "$root" describe --always --dirty --abbrev=7 2>/dev/null || echo unknown)"
echo "  copied BroadTek.kext ($(shasum "$oc/Kexts/BroadTek.kext/Contents/MacOS/BroadTek" | cut -c1-12), build $build)"
echo "  after the restart, ioreg's broadtek-build must read $build; anything"
echo "  else means this is not the kext that booted."
# no AppleDouble files on the FAT volume: OpenCore would try to read them
find "$oc/Kexts" -name '._*' -delete 2>/dev/null || true

cp "$tmp" "$oc/config.plist"
sync
echo "done, stage $stage. Restart for it to take effect."
# not log show: it carries no kernel lines on this machine (docs/STATE.md §3)
echo "after the restart:  ioreg -l | grep -i broadtek"
echo "  and, within a minute or two of booting, before the buffer wraps:"
echo "                    sudo dmesg | grep -i broadtek"
echo "to switch it off:   sudo efi/uninstall.sh"

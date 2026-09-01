#!/bin/sh
#
# Exercises the 752x560 panel detection against every device this fork lands on.
#
# Three functions decide whether a device renders natively at 752x560 or at
# 640x480: read_panel_mode() reads the panel, read_fb_mode() reads the current
# framebuffer mode, and panel_is_560p() judges the pair. They are duplicated in
# runtime.sh and install.sh - the installer runs before the card's script
# directory exists, so the two cannot share code - and they must agree, or the
# installer would pin a mode the session then changes away from. Both copies
# are extracted from source here rather than pasted, and both are run through
# the same matrix.
#
# The fixtures in tools/fixtures/ are real /proc/mi_modules/fb/mi_fb0 dumps.
# mi_fb0-mini-v4.txt came off a tester's Mini v4 and is the reason any of this
# exists: that firmware has no "Current TimingWidth=" line at all, so Onion's
# original probe found nothing, retried for five seconds, and locked a 752x560
# panel to 640x480.
#
# Usage: tools/test_panel_is_560p.sh
#
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
FIXTURES="$ROOT/tools/fixtures"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

extract() { # $1=source file $2=output
    awk '/^(read_panel_mode|read_fb_mode|panel_is_560p)\(\) \{/, /^\}/' "$1" > "$2"
    for fn in read_panel_mode read_fb_mode panel_is_560p; do
        grep -q "^$fn() {" "$2" || { echo "ERROR: $fn missing from $1" >&2; exit 1; }
    done
    sed -i \
        -e 's#/proc/mi_modules/fb/mi_fb0#$SB/mi_fb0#g' \
        -e 's#/etc/fw_printenv miyoo_version#$SB/fw_printenv miyoo_version#g' \
        -e 's#/tmp/pin_560p_failed#$SB/pin_560p_failed#g' \
        "$2"
}

extract "$ROOT/static/build/.tmp_update/runtime.sh"          "$WORK/runtime_fn.sh"
extract "$ROOT/static/dist/miyoo/app/.tmp_update/install.sh" "$WORK/install_fn.sh"

SB="$WORK/sb"
pass=0; fail=0

setup() { # $1=fixture basename (or "none") $2=firmware ("MISSING" for none)
    rm -rf "$SB"; mkdir -p "$SB"
    [ "$1" = "none" ] || cp "$FIXTURES/$1" "$SB/mi_fb0"
    if [ "$2" != "MISSING" ]; then
        printf '#!/bin/sh\necho "%s"\n' "$2" > "$SB/fw_printenv"
        chmod +x "$SB/fw_printenv"
    fi
}

probe()   { ( . "$WORK/runtime_fn.sh"; read_panel_mode ); }
fbmode()  { ( . "$WORK/runtime_fn.sh"; read_fb_mode ); }
verdict() { ( . "$1"; if panel_is_560p "$(read_panel_mode)"; then echo 560p; else echo 480p; fi ); }

cmp_out() { # $1=desc $2=got $3=want
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1)); printf '  ok     %-42s -> %s\n' "$1" "$2"
    else
        fail=$((fail + 1)); printf '  FAIL   %-42s -> %s (wanted %s)\n' "$1" "$2" "$3"
    fi
}

check() { # $1=desc $2=expected verdict - also asserts both copies agree
    got=$(verdict "$WORK/runtime_fn.sh")
    ins=$(verdict "$WORK/install_fn.sh")
    if [ "$got" != "$ins" ]; then
        fail=$((fail + 1))
        printf '  DESYNC %-42s runtime=%s install=%s\n' "$1" "$got" "$ins"
        return 0
    fi
    cmp_out "$1" "$got" "$2"
}

echo "Panel probe - which mode each firmware reports:"
setup mi_fb0-flip.txt        "miyoo_version=202510011046"
cmp_out "Flip, unpinned (timing line wins)"  "$(probe)"  752x560
cmp_out "  its framebuffer is still"         "$(fbmode)" 640x480
setup mi_fb0-flip-pinned.txt "miyoo_version=202510011046"
cmp_out "Flip, already pinned"               "$(probe)"  752x560
setup mi_fb0-mini-v4.txt     "miyoo_version=202407211632"
cmp_out "Mini v4 (no timing line at all)"    "$(probe)"  752x560
cmp_out "  its framebuffer already is"       "$(fbmode)" 752x560
setup mi_fb0-mini-480p.txt   "miyoo_version=202407211632"
cmp_out "Mini v1-v3 / Mini+"                 "$(probe)"  640x480
setup mi_fb0-mini-480p-timing.txt "miyoo_version=202510011046"
cmp_out "480p panel that DOES print a timing line" "$(probe)" 640x480
setup mi_fb0-flip-no-trailing-comma.txt "miyoo_version=202510011046"
cmp_out "timing line with no trailing comma"  "$(probe)"  752x560
setup none                   "miyoo_version=202510011046"
cmp_out "mi_fb0 unreadable"                  "$(probe)"  ""

echo
echo "Verdicts:"
setup mi_fb0-flip.txt      "miyoo_version=202510011046"; check "Flip"                          560p
setup mi_fb0-mini-v4.txt   "miyoo_version=202407211632"; check "Mini v4"                       560p
setup mi_fb0-mini-v4.txt   "miyoo_version=202310271401"; check "Mini v4, earliest v4 firmware" 560p
setup mi_fb0-mini-480p.txt "miyoo_version=202407211632"; check "Mini v1-v3 on late firmware"   480p
setup mi_fb0-mini-480p.txt "miyoo_version=202111201656"; check "original Mini, 2021 firmware"  480p
setup mi_fb0-mini-480p-timing.txt "miyoo_version=202510011046"; check "480p panel with a timing line" 480p
setup mi_fb0-flip-no-trailing-comma.txt "miyoo_version=202510011046"; check "Flip, timing line unterminated" 560p
setup none                 "miyoo_version=202510011046"; check "mi_fb0 unreadable"             480p

echo
echo "A 752x560 panel with firmware too old, or unreadable, stays 480p:"
setup mi_fb0-mini-v4.txt "miyoo_version=202310271400"; check "one minute below the gate"  480p
setup mi_fb0-mini-v4.txt "MISSING"                   ; check "no /etc/fw_printenv"        480p
setup mi_fb0-mini-v4.txt "miyoo_version="            ; check "empty firmware value"       480p
setup mi_fb0-mini-v4.txt "miyoo_version=abc"         ; check "non-numeric firmware"       480p
setup mi_fb0-mini-v4.txt "## Error: not defined"     ; check "u-boot error text"          480p

echo
echo "A failed pin stays failed for the rest of the session:"
setup mi_fb0-mini-v4.txt "miyoo_version=202407211632"; touch "$SB/pin_560p_failed"
check "Mini v4 after a failed pin"  480p
setup mi_fb0-flip.txt    "miyoo_version=202510011046"; touch "$SB/pin_560p_failed"
check "Flip after a failed pin"     480p

echo
echo "passed=$pass failed=$fail"
[ "$fail" = "0" ] || exit 1

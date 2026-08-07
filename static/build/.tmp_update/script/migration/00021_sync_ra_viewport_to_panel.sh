#!/bin/sh

# Point RetroArch's custom viewport at the panel's real resolution.
#
# Onion now pins the framebuffer to the panel's native mode for the whole
# session, so games render at 752x560 on a Mini Flip / Mini v4 rather than
# 640x480 upscaled by the GOP. The shipped config carries a 640x480 custom
# viewport, which only takes effect when the user selects the "Custom" aspect
# ratio - but when they do, it would letterbox the picture into the old size.
#
# Derived from the panel rather than hardcoded, so this is correct on 640x480
# devices too, where it is a no-op.

RA_CFG=/mnt/SDCARD/RetroArch/.retroarch/retroarch.cfg

[ -f "$RA_CFG" ] || exit 0

resolution=$(head -n 1 /tmp/screen_resolution 2> /dev/null)
case "$resolution" in
[0-9]*x[0-9]*) ;;
*) exit 0 ;;
esac

res_x=$(echo "$resolution" | cut -d 'x' -f 1)
res_y=$(echo "$resolution" | cut -d 'x' -f 2)

set_cfg() {
    key="$1"
    value="$2"
    if grep -q "^$key = " "$RA_CFG"; then
        sed -i "s/^$key = .*/$key = \"$value\"/" "$RA_CFG"
    else
        echo "$key = \"$value\"" >> "$RA_CFG"
    fi
}

set_cfg custom_viewport_width "$res_x"
set_cfg custom_viewport_height "$res_y"
set_cfg custom_viewport_x "0"
set_cfg custom_viewport_y "0"

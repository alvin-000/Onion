#!/bin/sh
#
# fbwatch - undo unintended framebuffer mode changes on 752x560 panels
#
# Problem (Miyoo Mini Flip / Miyoo Mini v4, both 752x560 panels):
#   Onion's UI renders at 640x480 and the SigmaStar GOP hardware-upscales it to
#   fill the 752x560 panel  ->  StretchWindow Src[640,480] Dst[752,560].
#   On startup some processes set the framebuffer to the panel's native
#   752x560. The GOP stretch source follows xres/yres, so the scaler reads
#   640-pixel-stride content as 752-pixel-wide rows: every row shifts 112px,
#   the pattern repeats every ~6.7 rows, and the screen shows fine horizontal
#   banding in the previous screen's colours until the app sets 640x480.
#
#   Measured on a Mini Flip: the bad mode persists ~2.1s per transition.
#
#   Mini / Mini+ are unaffected: their panel is natively 640x480, so the
#   transient mode never differs. This script exits immediately there.
#
# SCOPE - this only ever corrects *back to* the UI mode. It deliberately does
# NOT enforce any other resolution.
#
#   Onion legitimately switches to 752x560 for apps and ports marked
#   FullResolution=1. An earlier revision treated /tmp/fb_target_res as an
#   authority to enforce in both directions, and drove the framebuffer to
#   752x560 itself - racing Onion's own change_resolution() and RetroArch's
#   init, and forcing the wide mode while the buffer still held 640-stride
#   content. That made game launches visibly worse.
#
#   So: when the target is the UI mode, defend it. When Onion wants anything
#   else, stand down completely and let Onion and RetroArch own the mode. This
#   also keeps the daemon off the CPU during gameplay.
#
# NOTE - this is a mitigation, not a cure. It reacts to a mode change that has
# already happened, so the bad mode is briefly live (up to one poll interval)
# and a thin band can still scan out. Eliminating it entirely requires stopping
# the FBIOPUT_VSCREENINFO that sets 752x560 in the first place.

FB_PROC=/proc/mi_modules/fb/mi_fb0
TARGET_FILE=/tmp/fb_target_res
FBSET=/usr/sbin/fbset
LOG=/tmp/fbwatch.log

POLL_US=2000        # while defending the UI mode
IDLE_US=200000      # while standing down (Onion owns the mode)

# Detection reads /sys/class/graphics/fb0/stride (xres * bytes-per-pixel, so
# 640x480 -> 2560 and 752x560 -> 3008). It is a tiny file read with the shell
# built-in `read`, so the hot loop forks only for usleep - not for cat/sed over
# the large mi_fb0 dump, keeping the daemon cheap while it is defending.
#
# Polling faster than this was tried (0.5ms) and made no visible difference,
# which indicates the residual artifact is the cost of the corrective fbset
# itself - a GOP scaler reconfiguration is visible however quickly it is
# issued - rather than the width of the window being raced. Removing it
# entirely means preventing the 752x560 FBIOPUT_VSCREENINFO, not reacting
# faster to it.
FB_STRIDE=/sys/class/graphics/fb0/stride
FB_BPP=/sys/class/graphics/fb0/bits_per_pixel

# Panel timing is not readable immediately at boot - runtime.sh's own
# get_screen_resolution() retries for the same reason. Wait for it rather than
# exiting, so this can start before init_system() and cover the LCD init.
panel=""
tries=0
while [ $tries -lt 40 ]; do
    if [ -r "$FB_PROC" ]; then
        panel="$(sed -n 's/.*Current TimingWidth=\([0-9]*\),TimingWidth=\([0-9]*\).*/\1x\2/p' "$FB_PROC")"
        [ -n "$panel" ] && break
    fi
    tries=$((tries + 1))
    usleep 250000
done

# Give up quietly if the panel never reported (nothing safe to enforce).
[ -n "$panel" ] || exit 0

# Only devices whose panel timing differs from 640x480 can hit this bug.
[ "$panel" = "640x480" ] && exit 0

# The UI mode is whatever runtime.sh set before init_system. Everything Onion
# draws outside a FullResolution app runs at this resolution.
UI_MODE="$(cat "$TARGET_FILE" 2>/dev/null)"
case "$UI_MODE" in
    [0-9]*x[0-9]*) ;;
    *) UI_MODE="640x480" ;;
esac

bpp=32
read -r bpp < "$FB_BPP" 2>/dev/null
[ -n "$bpp" ] || bpp=32
bytes=$((bpp / 8))
[ "$bytes" -gt 0 ] || bytes=4

ui_w="${UI_MODE%x*}"
ui_h="${UI_MODE#*x}"
ui_stride=$((ui_w * bytes))

while true; do
    target=""
    read -r target < "$TARGET_FILE" 2>/dev/null

    # Onion wants something other than the UI mode - hands off entirely.
    if [ "$target" != "$UI_MODE" ]; then
        usleep $IDLE_US
        continue
    fi

    current=""
    read -r current < "$FB_STRIDE" 2>/dev/null

    if [ -n "$current" ] && [ "$current" != "$ui_stride" ]; then
        # Set FBWATCH_DRYRUN=1, or touch /mnt/SDCARD/.fbwatch_dryrun, to log
        # what would be corrected without doing it.
        if [ "$FBWATCH_DRYRUN" = "1" ] || [ -f /mnt/SDCARD/.fbwatch_dryrun ]; then
            read -r up _ < /proc/uptime
            echo "$up WOULD stride $current -> $ui_stride" >> "$LOG"
            usleep $IDLE_US
            continue
        fi
        # The mode change is a GOP scaler reconfiguration and is visible in
        # itself, so hide it behind a backlight blank the way
        # change_resolution() does. Whether that is possible here depends on
        # timing: the correction that matters runs ~7.5s into boot, and Onion
        # only exports the PWM in init_system(), which comes later - so the
        # node may not exist yet. Record which case we are in.
        #
        # (Hiding the GOP layer via "GUI_SHOW 0 off" was tried instead and is a
        # no-op on this firmware: the write is accepted but Visible State stays
        # 1 and the panel does not blank.)
        _bl=/sys/class/pwm/pwmchip0/pwm0/duty_cycle
        _bl_prev=""
        if [ -w "$_bl" ]; then
            _bl_prev=$(cat "$_bl" 2> /dev/null)
            echo 0 > "$_bl" 2> /dev/null
        fi

        "$FBSET" -g "$ui_w" "$ui_h" "$ui_w" "$((ui_h * 2))" 32

        usleep 400000

        if [ -n "$_bl_prev" ]; then
            echo "$_bl_prev" > "$_bl" 2> /dev/null
        fi

        # Corrections are rare; log them so they can be correlated with any
        # visible artifact. blanked= says whether the backlight was available.
        read -r up _ < /proc/uptime
        echo "$up stride $current -> $ui_stride ($UI_MODE) blanked=${_bl_prev:-NO}" >> "$LOG"
    fi

    usleep $POLL_US
done

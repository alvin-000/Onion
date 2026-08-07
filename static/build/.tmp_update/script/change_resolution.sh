#!/bin/sh

sysdir=/mnt/SDCARD/.tmp_update
logfile=$(basename "$0" .sh)
. $sysdir/script/log.sh

res_x=""
res_y=""

if [ -n "$1" ]; then
    res_x=$(echo "$1" | cut -d 'x' -f 1)
    res_y=$(echo "$1" | cut -d 'x' -f 2)
else
    # No argument means "back to the session's mode". That is the panel's
    # native resolution wherever Onion could pin it, not a hardcoded 640x480 -
    # third-party ports call this to undo their own switch, and handing them
    # 480p would leave the framebuffer in the wrong mode for everything else.
    default_res=$(cat /tmp/fb_target_res 2> /dev/null)
    [ -n "$default_res" ] || default_res="640x480"
    res_x=$(echo "$default_res" | cut -d 'x' -f 1)
    res_y=$(echo "$default_res" | cut -d 'x' -f 2)
fi
log "Changing resolution to $res_x x $res_y"

if [ "${res_x}x${res_y}" = "$(cat /tmp/fb_target_res 2> /dev/null)" ]; then
    # Already in this mode. Every mode change is a visible GOP scaler
    # reconfiguration, so do not do one for nothing.
    log "Already at ${res_x}x${res_y}, nothing to do"
    exit 0
fi

# Publish the new target before switching so libfbpin lets this through.
echo -n "${res_x}x${res_y}" > /tmp/fb_target_res

# Hide the switch behind the backlight, the same way pin_ui_resolution and
# pin_installer_resolution do. The switch itself is a GOP scaler
# reconfiguration and is visible as roughly two seconds of garbled panel;
# blanking first turns that into a short black gap instead. Every caller of
# this script pays that cost, so it belongs here rather than in any one of them.
_bl=/sys/class/pwm/pwmchip0/pwm0/duty_cycle
_bl_prev=""
if [ -w "$_bl" ]; then
    _bl_prev=$(cat "$_bl" 2> /dev/null)
    echo 0 > "$_bl" 2> /dev/null
    echo 0 > /sys/class/pwm/pwmchip0/pwm0/enable 2> /dev/null
fi

fbset -g "$res_x" "$res_y" "$res_x" "$((res_y * 2))" 32

# Zero the aperture so whatever becomes visible at the new stride is black
# rather than the previous mode's content read back wrongly.
cat /dev/zero > /dev/fb0 2> /dev/null

# Let the scaler settle before the panel is lit again.
usleep 400000

if [ -n "$_bl_prev" ]; then
    echo "$_bl_prev" > "$_bl" 2> /dev/null
    echo 1 > /sys/class/pwm/pwmchip0/pwm0/enable 2> /dev/null
fi

# inform batmon and keymon of resolution change
killall -SIGUSR1 batmon
killall -SIGUSR1 keymon

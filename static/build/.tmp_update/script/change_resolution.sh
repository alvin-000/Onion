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
    res_x=640
    res_y=480
fi
log "Changing resolution to $res_x x $res_y"

# Zero the whole aperture (it spans both modes) so the region that becomes
# visible when the mode grows is already black rather than stale content.
cat /dev/zero > /dev/fb0 2> /dev/null

# The GOP scaler reconfiguration is itself visibly garbled on 752x560 panels,
# independent of framebuffer contents. Blank the backlight across the switch.
# See runtime.sh change_resolution() for detail.
_bl=/sys/class/pwm/pwmchip0/pwm0/duty_cycle
_bl_prev=""
if [ -w "$_bl" ]; then
    _bl_prev=$(cat "$_bl" 2> /dev/null)
    echo 0 > "$_bl" 2> /dev/null
fi

# Publish the new target only once the panel is dark - libfbclamp rewrites any
# mode set that disagrees with it, so writing it earlier makes the next process
# to touch the framebuffer perform the switch while lit. See runtime.sh.
echo -n "${res_x}x${res_y}" > /tmp/fb_target_res

fbset -g "$res_x" "$res_y" "$res_x" "$((res_y * 2))" 32

usleep 400000

if [ -n "$_bl_prev" ]; then
    echo "$_bl_prev" > "$_bl" 2> /dev/null
fi

# inform batmon and keymon of resolution change
killall -SIGUSR1 batmon
killall -SIGUSR1 keymon

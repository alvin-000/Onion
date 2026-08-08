#!/bin/sh
echo $0 $*
cd $(dirname "$0")
HOME=/mnt/SDCARD
# DinguxCommander blits into a surface but never calls SDL_Flip or
# SDL_UpdateRect - it writes /dev/fb0 itself - so it needs the geometry it was
# built for. See script/run_at_480p.sh. No-op on 640x480 panels.
/mnt/SDCARD/.tmp_update/script/run_at_480p.sh ./DinguxCommander
sync

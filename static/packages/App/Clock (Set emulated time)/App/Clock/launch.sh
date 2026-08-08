#!/bin/sh
sysdir=/mnt/SDCARD/.tmp_update
savedir=/mnt/SDCARD/Saves/CurrentProfile/saves

cd $sysdir
HOME=/mnt/SDCARD
# clock writes /dev/fb0 directly - it imports SDL_SetVideoMode but no SDL_Flip
# or SDL_UpdateRect - so it needs the geometry it was built for. See
# script/run_at_480p.sh. No-op on 640x480 panels.
./script/run_at_480p.sh ./bin/clock

date +%s > $savedir/currentTime.txt

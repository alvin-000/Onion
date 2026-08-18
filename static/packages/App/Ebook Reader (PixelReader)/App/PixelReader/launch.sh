#!/bin/sh
cd $(dirname "$0")

export LD_LIBRARY_PATH=$(dirname "$0")/lib:$LD_LIBRARY_PATH
# reader presents through SDL (SDL_Flip), so libuiscale scales its 640x480
# surface to the panel - see script/run_scaled.sh. No-op on 640x480 devices.
/mnt/SDCARD/.tmp_update/script/run_scaled.sh ./reader 2>log.txt

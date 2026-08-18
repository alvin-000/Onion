#!/bin/sh
scriptdir=/mnt/SDCARD/.tmp_update/script

touch /tmp/stay_awake

cd $scriptdir
# st presents through SDL, so libuiscale can scale its 640x480 surface to the
# panel - see script/run_scaled.sh. Missed in the first sweep because this
# launcher calls st through PATH rather than ./bin/st.
$scriptdir/run_scaled.sh st -q -e sh $scriptdir/ota_update.sh 

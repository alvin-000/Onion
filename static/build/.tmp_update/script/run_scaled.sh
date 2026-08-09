#!/bin/sh
#
#   Run an SDL app with libuiscale preloaded, so a program hardcoded to 640x480
#   fills a larger panel instead of sitting in it.
#
#   This is the companion to run_at_480p.sh, and which one an app needs depends
#   on how it puts pixels on screen:
#
#     presents through SDL          -> run_scaled.sh  (this one)
#       calls SDL_Flip or SDL_UpdateRect. libuiscale hooks those, hands the app
#       the 640x480 surface it expects, and scales the result to the panel. No
#       mode change, so no visible blank.
#
#     writes /dev/fb0 itself        -> run_at_480p.sh
#       imports SDL_SetVideoMode but neither SDL_Flip nor SDL_UpdateRect, and
#       references /dev/fb0. There is no present to hook, so the framebuffer is
#       switched to the geometry the app was built for instead.
#
#   Telling them apart:
#
#     arm-linux-gnueabihf-readelf --dyn-syms <binary> | grep SDL_
#
#   Self-disabling: libuiscale falls through to the real SDL call when
#   /tmp/fb_target_res is absent, already matches the requested mode, or the
#   app asks for something other than 32bpp. Safe to wrap unconditionally.
#
#   Usage:  run_scaled.sh <command> [args...]
#

UiScaleLib="/mnt/SDCARD/miyoo/lib/libuiscale.so"

if [ -f "$UiScaleLib" ]; then
    export LD_PRELOAD="$UiScaleLib${LD_PRELOAD:+:$LD_PRELOAD}"
fi

exec "$@"

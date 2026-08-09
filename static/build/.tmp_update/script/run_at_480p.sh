#!/bin/sh
#
#   Run a command with the framebuffer at 640x480, restoring the panel's mode
#   afterwards.
#
#   For apps that mmap /dev/fb0 and write it directly at a hardcoded 640x480
#   geometry rather than presenting through SDL. On a 752x560 panel libfbpin
#   rewrites their mode request to match the panel - which is right for anything
#   rendering through SDL and wrong for a direct writer, because they then
#   believe they were given the 640x480 they asked for and write rows of 640
#   into a 752-wide framebuffer. Every row lands 112 pixels further off than the
#   last, and the screen becomes unreadable noise.
#
#   Preloading a scaler does not help: nothing in these apps reaches SDL_Flip or
#   SDL_UpdateRect, so there is no present to hook. Giving them the geometry they
#   were built for is the fix.
#
#   Telling one of these apart is quick - if a binary imports SDL_SetVideoMode
#   but neither SDL_Flip nor SDL_UpdateRect, and references /dev/fb0, it writes
#   the framebuffer itself:
#
#     arm-linux-gnueabihf-readelf --dyn-syms <binary> | grep SDL_
#
#   No-op on 640x480 panels and wherever change_resolution.sh is unavailable, so
#   it is safe to wrap a launcher unconditionally.
#
#   Usage:  run_at_480p.sh <command> [args...]
#

ChangeRes="/mnt/SDCARD/.tmp_update/script/change_resolution.sh"
OriginalRes=""

restore_resolution() {
    if [ -n "$OriginalRes" ] && [ -x "$ChangeRes" ]; then
        "$ChangeRes" "$OriginalRes"
    fi
}

if [ -x "$ChangeRes" ]; then
    OriginalRes=$(cat /tmp/fb_target_res 2> /dev/null)
    if [ -n "$OriginalRes" ] && [ "$OriginalRes" != "640x480" ]; then
        # Restore on any exit, including a kill from the menu.
        trap restore_resolution EXIT INT TERM
        "$ChangeRes" 640x480
    else
        OriginalRes=""
    fi
fi

"$@"

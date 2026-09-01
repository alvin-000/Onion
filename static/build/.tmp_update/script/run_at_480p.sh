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
#   That test only sees SDL 1.2. An SDL2 app imports none of those three
#   symbols and still reads as "not a direct writer" while being one - the
#   PICO-8 wrapper (XK9274/pico-8-wrapper-miyoo) ships its own SDL2 with the
#   mmiyoo backend, which mmaps /dev/fb0 and presents with MI_GFX_BitBlit at a
#   stride of its own. Confirmed on a Mini Flip: pico8_dyn has zero
#   SDL_SetVideoMode and zero SDL_Flip references. For SDL2, look for the
#   window API and the absence of any SDL present path:
#
#     strings <binary> | grep -c SDL_CreateWindow    # non-zero: SDL2
#
#   and check the backend it loads for an open("/dev/fb0") plus MI_GFX/MI_SYS
#   rather than an SDL present call.
#
#   Onion applies this automatically to the apps in
#   script/direct_writer_apps.list; wrapping a launcher by hand is still
#   supported and is what covers anything the list does not name.
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

# Exit explicitly with the command's status. runtime.sh reads it to decide
# whether to raise "Fatal error occurred", and whether an EXIT trap's own last
# command replaces the script's status is shell-dependent - so do not leave it
# to the trap.
"$@"
exit $?

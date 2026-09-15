#!/bin/sh
#
# Exercises the two ways an app is routed to run_at_480p.sh.
#
# On a 752x560 panel most apps are carried across by libfbpin and libuiscale,
# but some cannot be - a direct framebuffer writer has no present to hook, and
# pinning it to the panel mode shears every row. Those run at 640x480 instead.
# Two things put an app on that path, and app_wants_480p() in runtime.sh is
# where they meet:
#
#   run_as_480p in the app's own folder   a user's manual override
#   a line in script/direct_writer_apps.list
#
# Both matter to get right in opposite directions. A false positive drops an app
# that was rendering correctly down to 480p; a false negative leaves a user with
# an app that will not start and a fallback that does nothing. The flag also has
# to work when the list is missing, because the whole point of it is to need
# nothing but the app's own folder.
#
# The functions are extracted from runtime.sh rather than pasted, so this tests
# what ships. The sandbox becomes $sysdir, which is all the redirection needed -
# both functions address everything through it.
#
# Usage: tools/test_app_wants_480p.sh
#
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUNTIME="$ROOT/static/build/.tmp_update/runtime.sh"
REAL_LIST="$ROOT/static/build/.tmp_update/script/direct_writer_apps.list"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

FN="$WORK/fn.sh"
awk '/^(app_launch_dir|app_root_dir|app_is_direct_writer|app_wants_480p|get_full_resolution_path)\(\) \{/, /^\}/' "$RUNTIME" > "$FN"
for fn in app_launch_dir app_root_dir app_is_direct_writer app_wants_480p get_full_resolution_path; do
    grep -q "^$fn() {" "$FN" || { echo "ERROR: $fn missing from $RUNTIME" >&2; exit 1; }
done

# get_full_resolution_path() gates on a file that only exists on a 752x560
# device. Point it at the sandbox so this runs anywhere - the function itself is
# still the one from runtime.sh, never a copy.
sed -i 's#/tmp/new_res_available#$SB/new_res_available#g' "$FN"
grep -q 'new_res_available' "$FN" || { echo "ERROR: new_res_available gate vanished" >&2; exit 1; }

SB="$WORK/sb"
pass=0; fail=0

reset() { # start from an install with the shipped list and no apps
    rm -rf "$SB"; mkdir -p "$SB/script"
    cp "$REAL_LIST" "$SB/script/direct_writer_apps.list"
}

app() { # $1=app dir under the sandbox; echoes its absolute path
    # config.json is what makes a folder an app, and app_root_dir() finds the
    # app by looking for it.
    mkdir -p "$SB/$1"
    printf '{"label":"x","launch":"%s"}\n' "${2:-launch.sh}" > "$SB/$1/config.json"
    echo "$SB/$1"
}

# An app whose config.json puts the launch script in a subfolder, as the PICO-8
# wrapper does. MainUI cd's to the subfolder, not to the app.
sub_app() { # $1=app dir under the sandbox, $2=subdir
    d=$(app "$1" "$2/launch.sh")
    mkdir -p "$d/$2"
    echo "$d"
}

cmd() { # $1=the launch line, written as cmd_to_run.sh
    printf '%s\n' "$1" > "$SB/cmd_to_run.sh"
}

# Two launch shapes reach cmd_to_run.sh, and both open with "cd <dir>;".
#
#   MainUI          cd D; chmod a+x D/launch.sh; LD_PRELOAD=... D/launch.sh
#   set_cmd_app()   cd D; chmod a+x ./launch.sh; LD_PRELOAD=... ./launch.sh
#
# The second is src/common/utils/apps.h, used by keymon's X/Y shortcut and by
# Tweaks. MainUI's is the one nearly every app arrives through, so it is the
# default here.
app_cmd() { # $1=app dir (absolute)
    cmd "cd $1; chmod a+x $1/launch.sh; LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so $1/launch.sh  "
}

shortcut_cmd() { # $1=app dir (absolute)
    cmd "cd $1; chmod a+x ./launch.sh; LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so ./launch.sh"
}

# log() is runtime.sh's, sourced from script/log.sh there and irrelevant here.
verdict() { ( . "$FN"; log() { :; }; sysdir="$SB"
              if app_wants_480p; then echo 480p; else echo normal; fi ); }
launchdir() { ( . "$FN"; sysdir="$SB"; app_launch_dir ); }

fullres() { ( . "$FN"; sysdir="$SB"; touch "$SB/new_res_available"
              get_full_resolution_path ); }

cmp_out() { # $1=desc $2=got $3=want
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1)); printf '  ok     %-46s -> %s\n' "$1" "$2"
    else
        fail=$((fail + 1)); printf '  FAIL   %-46s -> %s (wanted %s)\n' "$1" "$2" "$3"
    fi
}

check() { cmp_out "$1" "$(verdict)" "$2"; }

echo "The run_as_480p flag:"
reset; d=$(app "App/Terminal"); app_cmd "$d"
check "app with no flag"                                normal
touch "$d/run_as_480p"
check "  same app, flag dropped in"                     480p
rm "$d/run_as_480p"
check "  flag removed again"                            normal

reset; d=$(app "App/My Nice App"); app_cmd "$d"; touch "$d/run_as_480p"
check "app folder with a space in the name"             480p

reset; d=$(app "RApp/somerapp"); app_cmd "$d"; touch "$d/run_as_480p"
check "RApp folder"                                     480p

reset; d=$(app "Emu/PORTS/thing"); app_cmd "$d"; touch "$d/run_as_480p"
check "a folder that is neither App nor RApp"           480p

reset; d=$(app "App/Terminal"); touch "$d/run_as_480p"
cmd "cd $d; chmod a+x ./launch.sh; LD_PRELOAD=/mnt/SDCARD/miyoo/lib/libfbpin.so:/mnt/SDCARD/miyoo/lib/libuiscale.so:/mnt/SDCARD/miyoo/app/../lib/libpadsp.so ./launch.sh"
check "after launch_game()'s LD_PRELOAD sed"            480p

reset; d=$(app "App/Terminal"); shortcut_cmd "$d"; touch "$d/run_as_480p"
check "launched from a keymon shortcut instead"         480p

reset; d=$(app "App/Terminal"); app_cmd "$d"; mkdir "$d/run_as_480p"
check "flag is a directory, not a file"                 normal

echo
echo "An app whose launch script lives in a subfolder (the PICO-8 shape):"
reset; d=$(sub_app "App/pico" "script")
cmd "cd $d/script; chmod a+x ./launch.sh; LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so ./launch.sh"
check "no flag"                                         normal
touch "$d/run_as_480p"
check "  flag in the app folder, where a user puts it"  480p
rm "$d/run_as_480p"; touch "$d/script/run_as_480p"
check "  flag beside launch.sh instead"                 480p
rm "$d/script/run_as_480p"
check "  both removed"                                  normal

reset; d=$(sub_app "App/deep" "a/b")
cmd "cd $d/a/b; chmod a+x ./launch.sh; LD_PRELOAD=... ./launch.sh"
touch "$d/run_as_480p"
check "two subfolders deep"                             480p

# A file loose in App/ must not turn on every app underneath it.
reset; d=$(sub_app "App/pico" "script")
cmd "cd $d/script; chmod a+x ./launch.sh; LD_PRELOAD=... ./launch.sh"
touch "$SB/App/run_as_480p"
check "a stray flag in App/ affects nothing"            normal

echo
echo "The flag does not depend on the list:"
reset; rm "$SB/script/direct_writer_apps.list"
d=$(app "App/Terminal"); app_cmd "$d"; touch "$d/run_as_480p"
check "list missing, flag present"                      480p
rm "$d/run_as_480p"
check "list missing, no flag"                           normal

echo
echo "The shipped list, unchanged:"
reset
app_cmd "/mnt/SDCARD/App/pico"
check "the PICO-8 wrapper is listed"                    480p
app_cmd "/mnt/SDCARD/RApp/PICO-8"
check "Onion's own PICO-8 is not"                       normal
app_cmd "/mnt/SDCARD/App/Tweaks"
check "an unrelated app is not"                         normal

# The list entries carry a trailing slash to stay unambiguous, so they match
# the absolute launch path MainUI writes and not the "./launch.sh" a keymon
# shortcut writes. Pinned here so the asymmetry is visible rather than
# surprising: the flag file has no such blind spot, which is what the case
# above it shows.
shortcut_cmd "/mnt/SDCARD/App/pico"
check "  ...but not via a keymon shortcut"              normal
d=$(app "App/pico"); shortcut_cmd "$d"; touch "$d/run_as_480p"
check "  which the flag file covers regardless"         480p

echo
echo "Commands that are not an app launch:"
reset
cmd 'LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so ./retroarch -v -L "/mnt/SDCARD/RetroArch/.retroarch/cores/fceumm_libretro.so" "/mnt/SDCARD/Roms/FC/game.nes"'
check "a game"                                          normal
cmd "cd /mnt/SDCARD/App/Terminal"
check "a cd with no semicolon"                          normal
cmd ""
check "an empty command"                                normal
rm -f "$SB/cmd_to_run.sh"
check "no cmd_to_run.sh at all"                         normal

echo
echo "app_launch_dir() reads the folder out of the launch line:"
reset; d=$(app "App/My Nice App"); app_cmd "$d"
cmp_out "a name with a space survives whole"  "$(launchdir)"  "$d"
reset; d=$(app "App/Terminal"); app_cmd "$d"
cmp_out "an ordinary name"                    "$(launchdir)"  "$d"
reset; app_cmd "$SB/App/DoesNotExist"
cmp_out "a folder that is not there"          "$(launchdir)"  ""

echo
echo "get_full_resolution_path() resolves the app folder, not the launch folder:"
reset; d=$(app "App/Terminal"); app_cmd "$d"
cmp_out "a bare launch.sh"            "$(fullres)"  "$d/full_resolution"
reset; d=$(sub_app "App/pico" "script")
cmd "cd $d/script; chmod a+x ./launch.sh; LD_PRELOAD=... ./launch.sh"
cmp_out "launch.sh in a subfolder"    "$(fullres)"  "$d/full_resolution"
reset; d=$(app "App/My Nice App"); app_cmd "$d"
cmp_out "a name with a space"         "$(fullres)"  "$d/full_resolution"
reset; d=$(app "RApp/gngeo"); app_cmd "$d"
cmp_out "a cd-style RApp launch"      "$(fullres)"  "$d/full_resolution"
reset
cmd 'LD_PRELOAD=/lib/libpadsp.so ./retroarch -v -L "/mnt/SDCARD/RetroArch/.retroarch/cores/x.so" "/mnt/SDCARD/Roms/FC/g.nes"'
cmp_out "a game falls through"        "$(fullres)"  ""

echo
echo "passed=$pass failed=$fail"
[ "$fail" = "0" ] || exit 1

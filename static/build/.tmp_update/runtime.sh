#!/bin/sh
sysdir=/mnt/SDCARD/.tmp_update
miyoodir=/mnt/SDCARD/miyoo
export LD_LIBRARY_PATH="/lib:/config/lib:$miyoodir/lib:$sysdir/lib:$sysdir/lib/parasyte"
export PATH="$sysdir/bin:$PATH"

# Hold the framebuffer in the session's mode (see pin_ui_resolution). SDL's
# fbcon backend probes ~15 candidate modes on every video init and sets each
# one in turn; on a 752x560 panel every one of those is a visible scaler
# reconfiguration. This swallows them.
#
# Exported rather than added per launch site because the binaries that do it
# are spawned from several places - keymon's system() calls reach
# gameSwitcher --overlay and infoPanel, which no runtime.sh launch line covers.
# Launches that set LD_PRELOAD explicitly (RetroArch, MainUI) override this,
# which is intended.
#
# No-op on 640x480 panels - see src/libfbpin/fbpin.c.
#
# libuiscale rides along for the same reason. An SDL app built for a 640x480
# Miyoo still runs on a 752x560 panel without it - SDL hands it the real
# 3008-byte stride, so its rows land correctly and it simply occupies the top
# left corner - but the shim scales it to fill the panel instead. Exporting it
# here means any app gets that, including ones a user installs later, with no
# per-app wrapper to remember.
#
# Safe to export globally because the shim disables itself in every other case:
#
#   640x480 panel            /tmp/fb_target_res is absent, passes through
#   app asks for the panel   request already matches the target, passes through
#   run_at_480p.sh apps      that wrapper sets the target to 640x480 first, so
#                            direct /dev/fb0 writers get the mode they expect
#   gameSwitcher, infoPanel  never call SDL_SetVideoMode on this platform
#   bootScreen, the OSD      (sdl_direct_fb.h builds a plain RGB surface), and
#                            they read finfo.line_length, so they are already
#                            correct at the panel's own resolution
#   an SDL2 app              nothing named SDL_SetVideoMode to capture, so the
#                            shim never engages - see below
#
# That last case is why libuiscale is not linked against SDL 1.2 (see its
# Makefile). An LD_PRELOADed object's dependencies join the global symbol scope
# of every process, ahead of a dlopen'd library's own dependencies, so linking
# SDL 1.2 here dragged it into SDL2 processes and hijacked the 142 symbol names
# the two versions share - SDL_Init among them. RAOfflineProxy, a pygame app,
# died on launch with "No available video device" until that link was dropped.
# Anything added to this export has to be checked the same way.
#
# What it does not fix is a third-party app that mmaps /dev/fb0 and assumes a
# 640-pixel stride. That garbles with or without this - there is no present to
# hook - and needs run_at_480p.sh. Measured: the screen is unreadable while such
# an app runs, MainUI survives, and the display repaints itself once it exits.
export LD_PRELOAD="$miyoodir/lib/libfbpin.so:$miyoodir/lib/libuiscale.so"

logfile=$(basename "$0" .sh)
. $sysdir/script/log.sh

MODEL_MM=283
MODEL_MMF=285
MODEL_MMP=354

# Physical panel resolution, detected at boot.
screen_resolution="640x480"

# The framebuffer mode the whole session runs in. Every mode change is a
# visible GOP scaler reconfiguration on 752x560 panels, so this is set once,
# before anything draws, and never changed again.
ui_resolution="640x480"

main() {
    # Set model ID based on hardware detection
    if [ -e /sys/devices/soc0/soc/soc:hall-mh248/hallvalue ] || [ -e /dev/input/event1 ]; then
        export DEVICE_ID=$MODEL_MMF
    elif axp 0 > /dev/null 2>&1; then
        export DEVICE_ID=$MODEL_MMP
    else
        export DEVICE_ID=$MODEL_MM
    fi
    echo -n "$DEVICE_ID" > /tmp/deviceModel

    # HW capability flags
    HAS_AXP=0
    if [ $DEVICE_ID -eq $MODEL_MMF ] || [ $DEVICE_ID -eq $MODEL_MMP ]; then
        HAS_AXP=1
    fi

    SERIAL_NUMBER=$(read_uuid)
    echo -n "$SERIAL_NUMBER" > /tmp/deviceSN

    touch /tmp/is_booting
    check_installer
    clear_logs

    # Establish the session's framebuffer mode before anything draws. Must come
    # before init_system: the LCD init there is when boot-time garbling shows.
    get_screen_resolution
    pin_ui_resolution
    write_560p_report

    init_system
    update_time

    # Remount passwd/group to add our own users
    mount -o bind $sysdir/config/passwd /etc/passwd
    mount -o bind $sysdir/config/group /etc/group

    # Start the battery monitor
    batmon &

    # Reapply theme
    system_theme="$(/customer/app/jsonval theme)"
    active_theme="$(cat $sysdir/config/active_theme)"

    if [ "$system_theme" == "./" ] || [ "$system_theme" != "$active_theme" ] || [ ! -d "$system_theme" ]; then
        themeSwitcher --reapply_icons
    fi

    # Check is charging
    if [ $DEVICE_ID -eq $MODEL_MM ]; then
        is_charging=$(cat /sys/devices/gpiochip0/gpio/gpio59/value)
    elif [ $HAS_AXP -eq 1 ]; then
        axp_status="0x$(axp 0 | cut -d':' -f2)"
        is_charging=$([ $(($axp_status & 0x4)) -eq 4 ] && echo 1 || echo 0)
    fi

    # Show charging animation
    if [ $is_charging -eq 1 ]; then
        cd $sysdir
        chargingState
    fi

    # Make sure MainUI doesn't show charging animation
    touch /tmp/no_charging_ui

    # Check if blf needs enabling
    if [ -f $sysdir/config/.blf ]; then
        /mnt/SDCARD/.tmp_update/script/blue_light.sh check &
    elif [ -f $sysdir/config/.blfOn ]; then
        /mnt/SDCARD/.tmp_update/script/blue_light.sh enable &
    fi

    cd $sysdir
    bootScreen "Boot"

    # Set filebrowser branding to "Onion" and apply custom theme
    if [ -f "$sysdir/config/filebrowser/first.run" ]; then
        $sysdir/bin/filebrowser config set --branding.name "Onion" -d $sysdir/config/filebrowser/filebrowser.db
        $sysdir/bin/filebrowser config set --branding.files "$sysdir/config/filebrowser/theme" -d $sysdir/config/filebrowser/filebrowser.db

        rm "$sysdir/config/filebrowser/first.run"
    fi

    # Start networking (Checks networking, checks timezone)
    start_networking

    # Start the key monitor
    keymon &

    # Init
    rm /tmp/.offOrder 2> /dev/null
    HOME=/mnt/SDCARD/RetroArch/

    # Disable VNC server flag at boot
    if [ -f "$sysdir/config/.vncServer" ]; then
        rm "$sysdir/config/.vncServer"
    fi

    # Detect if MENU button is held
    detectKey 1
    menu_pressed=$?

    if [ $menu_pressed -eq 0 ]; then
        rm -f "$sysdir/cmd_to_run.sh" 2> /dev/null
    fi

    # RetroArch ships as two builds that differ in exactly one thing:
    # retroarch_miyoo354 has RetroAchievements compiled in, plain retroarch
    # does not. Everything else matches - same NEEDED libraries, same video,
    # audio and input drivers, same 560p handling.
    #
    # Upstream mounted the achievements build on the Mini+ only, and #1860
    # added the Flip, both being the WiFi-capable models. A Mini v4 reports
    # DEVICE_ID 283, the same as a 480p Mini v1-v3, so no model test can single
    # it out - and it does not need to. Selecting by model means one SD card
    # gains and loses achievements as it is moved between devices, which is
    # what prompted this: a card set up on a Flip, carried to a v4, silently
    # stopped having them.
    #
    # A build without achievements ignores a configured account rather than
    # failing - verified on hardware with username, password, token, hardcore
    # and leaderboards all set: no login attempt, no crash, core loaded
    # normally. So mounting the achievements build everywhere is safe, and
    # costs a device that cannot use it nothing but ~185KB of inert code.
    if [ -f /mnt/SDCARD/RetroArch/retroarch_miyoo354 ]; then
        mount -o bind /mnt/SDCARD/RetroArch/retroarch_miyoo354 /mnt/SDCARD/RetroArch/retroarch
    fi

    # Bind arcade name library to customer path
    mount -o bind $miyoodir/lib/libgamename.so /customer/lib/libgamename.so

    rm -rf /tmp/is_booting

    #EmuDeck - Startup scripts
    mkdir -p "$sysdir/startup"
    mkdir -p "$sysdir/checkoff"
    startup_scripts=$(find "$sysdir/startup" -type f -name "*.sh")

    for startup_script in $startup_scripts; do
        sh "$startup_script"
    done

    # Auto launch
    if [ ! -f $sysdir/config/.noAutoStart ]; then
        state_change check_game
    else
        rm -f "$sysdir/cmd_to_run.sh" 2> /dev/null
    fi

    # Only launch startup app if not quick switching
    if [ ! -f /tmp/quick_switch ]; then
        startup_app=$(cat $sysdir/config/startup/app)

        if [ $startup_app -eq 1 ]; then
            log "\n\n:: STARTUP APP: GameSwitcher\n\n"
            touch $sysdir/.runGameSwitcher
        elif [ $startup_app -eq 2 ]; then
            log "\n\n:: STARTUP APP: RetroArch\n\n"
            echo "LD_PRELOAD=$miyoodir/lib/libpadsp.so ./retroarch -v" > $sysdir/cmd_to_run.sh
            touch /tmp/quick_switch
        elif [ $startup_app -eq 3 ]; then
            log "\n\n:: STARTUP APP: AdvanceMENU\n\n"
            touch /tmp/run_advmenu
        fi
    fi

    state_change check_switcher
    set_startup_tab
    # Main runtime loop
    while true; do
        state_change check_main_ui
        state_change check_game_menu
        state_change check_game
        state_change check_switcher
    done
}

state_change() {
    log "state change: $1"
    runifnecessary "keymon" keymon
    check_networking
    touch /tmp/state_changed
    sync
    eval "$1"
}

set_prev_state() {
    echo "$1" > /tmp/prev_state
}

clear_logs() {
    mkdir -p $sysdir/logs

    cd $sysdir/logs
    rm -f \
        ./MainUI.log \
        ./gameSwitcher.log \
        ./keymon.log \
        ./game_list_options.log \
        ./dnsmasq.log \
        ./ftp.log \
        ./runtime.log \
        ./update_networking.log \
        ./easy_netplay.log \
        2> /dev/null
}

check_main_ui() {
    if [ ! -f $sysdir/cmd_to_run.sh ]; then
        if [ -f /tmp/run_advmenu ]; then
            rm /tmp/run_advmenu
            $sysdir/bin/adv/run_advmenu.sh
        else
            launch_main_ui
        fi

        check_off_order "End"
    fi
}

launch_main_ui() {
    log "\n:: Launch MainUI"

    cd $sysdir

    # Generate battery percentage image
    mainUiBatPerc

    # Hide any new recents if applicable
    check_hide_recents

    # Ensure we've mounted the correct MainUI binary
    mount_main_ui

    # Wifi state before
    wifi_setting=$(/customer/app/jsonval wifi)

    start_audioserver

    mute_theme_bgm

    # MainUI launch
    cd $miyoodir/app
    # MainUI is a closed-source binary hardcoded to 640x480, and its SDL
    # fbcon backend would otherwise drag the framebuffer back to that mode on
    # startup. libuiscale gives it the 640x480 surface it expects and upscales
    # the result into the native framebuffer, so the mode never changes. It is
    # a no-op when the framebuffer is already 640x480.
    #
    # libfbpin is here too, despite the note in the handoff plan that the two
    # must never both apply to one process. That warning's reason was that
    # MainUI legitimately needs a non-native mode - and libuiscale is exactly
    # what removed that need: it rewrites MainUI's 640x480 request into a
    # request for the pinned mode before it ever reaches the framebuffer, so
    # libfbpin only ever sees conforming mode sets and passes them through.
    #
    # Without it, mode sets MainUI makes outside SDL_SetVideoMode - notably on
    # the way out - are unabsorbed, and leave the framebuffer at 640x480 for
    # whatever runs next. Measured: apps launched from the menu read the
    # framebuffer as 640x480 and correctly size themselves to it, landing as a
    # 640x480 image centred in a 752x560 panel.
    PATH="$miyoodir/app:$PATH" \
        LD_LIBRARY_PATH="$miyoodir/lib:/config/lib:/lib" \
        LD_PRELOAD="$miyoodir/lib/libfbpin.so:$miyoodir/lib/libuiscale.so:$miyoodir/lib/libpadsp.so" \
        ./MainUI 2>&1 > /dev/null

    # MainUI has exited, so the UI has been used - snapshot what libfbpin saw.
    copy_fbpin_log

    # Merge the last game launched into the recent list
    check_hide_recents

    # Check if wifi setting changed
    if [ $(/customer/app/jsonval wifi) -ne $wifi_setting ]; then
        touch /tmp/network_changed
        rm /tmp/ntp_synced 2> /dev/null
        sync
    fi

    $sysdir/bin/freemma
    mv -f /tmp/cmd_to_run.sh $sysdir/cmd_to_run.sh

    set_prev_state "mainui"
}

check_game_menu() {
    if [ ! -f /tmp/launch_alt ]; then
        return
    fi

    rm -f /tmp/launch_alt

    if [ ! -f $sysdir/cmd_to_run.sh ]; then
        return
    fi

    launch_game_menu
}

launch_game_menu() {
    log "\n\n:: GLO\n\n"

    cd $sysdir
    ./script/game_list_options.sh

    if [ $? -ne 0 ]; then
        log "\n\n< Back to MainUI\n\n"
        rm -f $sysdir/cmd_to_run.sh 2> /dev/null
        check_off_order "End"
    fi
}

check_game() {
    # Game launch
    if [ -f $sysdir/cmd_to_run.sh ]; then
        launch_game
    fi
}

check_is_game() {
    echo "$1" | grep -q "retroarch/cores" || echo "$1" | grep -q "/../../Roms/" || echo "$1" | grep -q "/mnt/SDCARD/Roms/"
}

change_resolution() {
    res_x=""
    res_y=""

    if [ -n "$1" ]; then
        res_x=$(echo "$1" | cut -d 'x' -f 1)
        res_y=$(echo "$1" | cut -d 'x' -f 2)
    else
        res_x=$(echo "$ui_resolution" | cut -d 'x' -f 1)
        res_y=$(echo "$ui_resolution" | cut -d 'x' -f 2)
    fi
    log "Changing resolution to $res_x x $res_y"

    if [ "${res_x}x${res_y}" = "$(cat /tmp/fb_target_res 2> /dev/null)" ]; then
        # Already in this mode - do not reconfigure the scaler for nothing.
        return
    fi

    bootScreen clear

    # Publish the new target before switching so libfbpin stops defending the
    # old one and lets this fbset through.
    echo -n "${res_x}x${res_y}" > /tmp/fb_target_res
    fbset -g "$res_x" "$res_y" "$res_x" "$((res_y * 2))" 32
    # inform batmon and keymon of resolution change
    killall -SIGUSR1 batmon
    killall -SIGUSR1 keymon
}

launch_game() {
    log "\n:: Launch game"
    cmd=$(cat $sysdir/cmd_to_run.sh)
    TZ_VALUE=$(cat "$sysdir/config/.tz")

    is_game=0
    rompath=""
    romext=""
    romcfgpath=""
    retroarch_core=""
    full_resolution_path=""
    launch_script=""

    start_audioserver
    save_settings

    if check_is_game "$cmd"; then
        # Extract rom path
        rompath=$(echo "$cmd" | awk '{ st = index($0,"\" \""); print substr($0,st+3,length($0)-st-3)}')

        # Check for custom launch script
        if echo "$rompath" | grep -q ":"; then
            launch_script=$(echo "$rompath" | awk '{split($0,a,":"); print a[1]}')
            rompath=$(echo "$rompath" | awk '{split($0,a,":"); print a[2]}')
            echo "LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so \"$launch_script\" \"$rompath\"" > $sysdir/cmd_to_run.sh
        fi

        orig_path="$rompath"
        romext=$(echo "$(basename "$rompath")" | awk -F. '{print tolower($NF)}')

        if [ "$romext" != "miyoocmd" ]; then
            # Resolve real path
            if [ -f "$rompath" ]; then
                rompath=$(realpath "$rompath")
            fi

            # Update cmd_to_run with resolved path
            if [ "$rompath" != "$orig_path" ]; then
                temp=$(cat $sysdir/cmd_to_run.sh)
                cmd_replaced=$(echo "$temp" | rev | sed 's/^"[^"]*"//g' | rev)"\"$rompath\""
                echo "$cmd_replaced" > $sysdir/cmd_to_run.sh
            fi

            # Game config path
            romcfgpath="$(dirname "$rompath")/.game_config/$(basename "$rompath" ".$romext").cfg"
            log "rompath: $rompath (ext: $romext)"
            log "romcfgpath: $romcfgpath"
            is_game=1
        fi
    fi

    full_resolution_path="$(get_full_resolution_path)"

    if [ -z "$launch_script" ]; then
        launch_script=$(echo "$cmd" | awk -F'"' '{print $2}')
    fi

    if [ $is_game -eq 1 ]; then
        if [ -f "$launch_script" ] && cat "$launch_script" | grep -q '.retroarch/cores'; then
            # Override core if needed
            override_game_core "$romcfgpath" "$launch_script"
        fi

        # Handle dollar sign
        if echo "$rompath" | grep -q "\$"; then
            temp=$(cat $sysdir/cmd_to_run.sh)
            echo "$temp" | sed 's/\$/\\\$/g' > $sysdir/cmd_to_run.sh
        fi

        # Kill services for maximum performance
        if [ ! -f $sysdir/config/.keepServicesAlive ]; then
            for process in dropbear bftpd filebrowser telnetd smbd; do
                if is_running $process; then
                    killall -9 $process
                fi
            done
        fi

        playActivity start "$rompath"
    fi

    # Prevent quick switch loop
    rm -f /tmp/quick_switch 2> /dev/null
    rm -f /tmp/force_auto_load_state 2> /dev/null

    log "----- COMMAND:"
    log "$(cat $sysdir/cmd_to_run.sh)"

    if [ $is_game -eq 0 ] || [ -f "$rompath" ]; then
        if [ "$romext" == "miyoocmd" ]; then
            /mnt/SDCARD/.tmp_update/script/remove_last_recent_entry.sh
            emupath=$(dirname $(echo "$cmd" | awk '{ gsub(/"/, "", $2); st = index($2,".."); if (st) { print substr($2,0,st) } else { print $2 } }'))
            cd "$emupath"
            chmod a+x "$rompath"
            "$rompath" "$rompath" "$emupath"
            retval=$?
        else
            # The framebuffer is already pinned to $ui_resolution, which is
            # the panel's native mode wherever that is supported - so games run
            # at full resolution without a mode change. change_resolution() is
            # a no-op when the requested mode is the one already set; it is
            # still called so anything that genuinely needs a different mode
            # keeps working.
            if [ -f /tmp/new_res_available ] && [ -f "$full_resolution_path" ]; then
                change_resolution
            fi

            # Previously skipped on 560p-capable devices because the loading
            # screen was drawn either side of a mode switch. There is no switch
            # any more, so it can be shown everywhere.
            if [ $is_game -eq 1 ]; then
                infoPanel --message "LOADING" --persistent --romscreen &
                touch /tmp/dismiss_info_panel
                sync
            fi

            # GAME LAUNCH
            cd /mnt/SDCARD/RetroArch
            force_retroarch_cfg

            # MainUI writes cmd_to_run.sh with a hardcoded
            # LD_PRELOAD=.../libpadsp.so, and that assignment discards the
            # libfbpin.so exported at the top of this script. Without it the
            # app's SDL fbcon backend probes ~15 modes on video init and every
            # probe is a real mode change. Measured on a Mini Flip around a
            # Package Manager launch: 752x560 -> 640x480 -> 752x560 -> 640x480.
            # That is the garbling on entering and leaving apps.
            #
            # Apps only. Games are deliberately left alone: libfbpin rewrites
            # any mode set that disagrees with the target, so a core that
            # genuinely needs a different mode would have its video init
            # rejected outright. Pinning the game path is worth doing, but it
            # needs testing against real cores first - change_resolution below
            # already restores the session mode if a game changes it.
            #
            # libuiscale is restored alongside it, for the same reason and with
            # the same caveat. Without it an app built for 640x480 still runs -
            # SDL hands it the real stride and it occupies the top left corner -
            # but it is not scaled, so the global export in this script would
            # reach keymon and its children while missing the one path apps are
            # actually launched through.
            if [ $is_game -eq 0 ] && ! grep -q "libfbpin.so" $sysdir/cmd_to_run.sh; then
                sed -i "s|LD_PRELOAD=|LD_PRELOAD=$miyoodir/lib/libfbpin.so:$miyoodir/lib/libuiscale.so:|" $sysdir/cmd_to_run.sh
                log "app launch: restored libfbpin and libuiscale to LD_PRELOAD"
            fi

            # make the cmd_to_run shell env aware of the new timezone
            TZ="$TZ_VALUE" $sysdir/cmd_to_run.sh
            retval=$?

            # Restore the session mode in case the game changed it behind
            # our back. No-op when it did not.
            change_resolution "$ui_resolution"

            if [ $is_game -eq 1 ] && [ ! -f /tmp/.offOrder ] && [ -f /tmp/.displaySavingMessage ]; then
                rm /tmp/.displaySavingMessage
                infoPanel --message "SAVING" --persistent --romscreen &
                touch /tmp/dismiss_info_panel
                sync
            fi
        fi
    else
        retval=404
    fi

    log "cmd retval: $retval"

    if [ $retval -eq 404 ]; then
        infoPanel --title "File not found" --message "The requested file was not found." --auto
    elif [ $retval -ge 128 ] && [ $retval -ne 143 ] && [ $retval -ne 255 ] && [ ! -f /tmp/.forceKillRetroarch ]; then
        infoPanel --title "Fatal error occurred" --message "The program exited unexpectedly.\n(Error code: $retval)" --auto
    fi

    launch_game_postprocess $is_game "$launch_script" "$rompath"
}

force_retroarch_cfg() {
    # Enable network commands in RetroArch
    cat > /tmp/onion_ra_patch.cfg <<- EOM
network_cmd_enable = "true"
EOM

    $sysdir/script/patch_ra_cfg.sh /tmp/onion_ra_patch.cfg

    rm /tmp/onion_ra_patch.cfg
}

override_game_core() {
    romcfgpath="$1"
    launch_path="$2"

    retroarch_core=""

    # Determine the appropriate appendconfig
    if [ -f /tmp/reset_game ]; then
        echo -e "savestate_auto_load = \"false\"\nconfig_save_on_exit = \"false\"\n" > /tmp/reset.cfg
        appendconfig="/tmp/reset.cfg"
        rm /tmp/reset_game
    elif [ -f /tmp/force_auto_load_state ]; then
        echo -e "savestate_auto_load = \"true\"\nconfig_save_on_exit = \"false\"\n" > /tmp/auto_load_state.cfg
        appendconfig="/tmp/auto_load_state.cfg"
        rm /tmp/force_auto_load_state
    else
        appendconfig=""
    fi

    # Extract the core name from the ROM's config file
    if [ -f "$romcfgpath" ]; then
        romcfg=$(cat "$romcfgpath")
        retroarch_core=$(get_info_value "$romcfg" core)
    fi

    # Check if core override is specified in the ROM's config
    if [ ! -z "$retroarch_core" ]; then
        corepath=".retroarch/cores/$retroarch_core.so"
        log "Overriding core to: $retroarch_core"

        if [ -f "/mnt/SDCARD/RetroArch/$corepath" ]; then
            if [ -z "$appendconfig" ]; then
                # No appendconfig needed
                echo "LD_PRELOAD=$miyoodir/lib/libpadsp.so ./retroarch -v -L \"$corepath\" \"$rompath\"" > $sysdir/cmd_to_run.sh
            else
                # Inject appendconfig directly into the final runtime command
                echo "LD_PRELOAD=$miyoodir/lib/libpadsp.so ./retroarch -v --appendconfig \"$appendconfig\" -L \"$corepath\" \"$rompath\"" > $sysdir/cmd_to_run.sh
            fi
            return 0 # Success
        else
            log "Specified core not found: $corepath"
        fi
    fi

    # Add appendconfig to the launch script if necessary
    if [ ! -z "$appendconfig" ]; then
        if grep -q -- "--appendconfig" "$launch_path"; then
            # Update existing appendconfig in the script
            sed -i "s|--appendconfig \".*\"|--appendconfig \"$appendconfig\"|g" "$launch_path"
            log "Updated existing appendconfig in launch script: $appendconfig (path: $launch_path)"
        else
            # Inject appendconfig argument into the command
            sed -i "s|./retroarch -v|& --appendconfig \"$appendconfig\"|g" "$launch_path"
            log "Injected appendconfig into launch script: $appendconfig (path: $launch_path)"
        fi
    fi
}

cleanup_appendconfig() {
    launch_path="$1"

    if [ ! -f /tmp/quick_switch ]; then
        # Cleanup `cmd_to_run.sh` by removing any existing appendconfig
        if [ -f "$sysdir/cmd_to_run.sh" ]; then
            cmd=$(cat $sysdir/cmd_to_run.sh)
            if echo "$cmd" | grep -q "/tmp/reset.cfg"; then
                echo "$cmd" | sed 's/ --appendconfig \"\/tmp\/reset.cfg\"//g' > $sysdir/cmd_to_run.sh
            elif echo "$cmd" | grep -q "/tmp/auto_load_state.cfg"; then
                echo "$cmd" | sed 's/ --appendconfig \"\/tmp\/auto_load_state.cfg\"//g' > $sysdir/cmd_to_run.sh
            fi
        fi
    fi

    # Clean up launch_path: Remove explicit appendconfig paths
    if [ -w "$launch_path" ]; then
        if grep -q '/tmp/reset.cfg' "$launch_path"; then
            sed -i 's| --appendconfig "/tmp/reset.cfg"||g' "$launch_path"
            log "Removed /tmp/reset.cfg from $launch_path"
        elif grep -q '/tmp/auto_load_state.cfg' "$launch_path"; then
            sed -i 's| --appendconfig "/tmp/auto_load_state.cfg"||g' "$launch_path"
            log "Removed /tmp/auto_load_state.cfg from $launch_path"
        fi
    fi
}

launch_game_postprocess() {
    is_game=$1
    launch_path="$2"
    rompath="$3"

    # Reset CPU frequency
    echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

    # Reset flags
    rm /tmp/stay_awake 2> /dev/null

    # TIMER END + SHUTDOWN CHECK
    if [ $is_game -eq 1 ]; then
        cd $sysdir
        playActivity stop "$rompath"

        # Remove appended configs
        cleanup_appendconfig "$launch_path"

        if [ -f /tmp/.lowBat ]; then
            bootScreen lowBat
            sleep 3
            touch /tmp/.offOrder
        fi

        # Reset networking if needed
        if [ ! -f "$sysdir/config/.keepServicesAlive" ]; then
            for service in smbd http ssh ftp telnet; do
                if [ -f "$sysdir/config/.${service}State" ]; then
                    touch /tmp/network_changed
                    break
                fi
            done
        fi

        set_prev_state "game"
        check_off_order "End_Save"
    else
        set_prev_state "app"
        check_off_order "End"
    fi
}

get_full_resolution_path() {
    if [ -f /tmp/new_res_available ]; then
        # Check if the program to be launched supports 560p
        # Different programs need different checks (Apps vs Ports vs the rest)
        if grep -qF "/mnt/SDCARD/App/" $sysdir/cmd_to_run.sh; then
            # ----- App launch ----- #

            echo "$(cat $sysdir/cmd_to_run.sh | cut -d' ' -f 2 | sed 's/;/\/full_resolution/')"

        elif grep -qF "/mnt/SDCARD/Roms/PORTS/" $sysdir/cmd_to_run.sh; then
            # ----- Port launch ----- #

            dot_port_path=$(grep -o '\/mnt\/SDCARD\/Roms\/PORTS.*\.port' $sysdir/cmd_to_run.sh)

            if grep -qF "FullResolution=1" "$dot_port_path"; then
                # Look for FullResolution=1 in the .port file
                # set full_resolution_path to a file that will always exist
                echo "/tmp/new_res_available"
            fi

        else
            # ----- Everything else ----- #
            echo "$(grep -o '".*launch\.sh"' $sysdir/cmd_to_run.sh | sed 's/"//g; s/launch\.sh/full_resolution/')"
        fi
    fi
}

is_running() {
    process_name="$1"
    pgrep "$process_name" > /dev/null
}

get_info_value() {
    echo "$1" | grep "$2\b" | awk '{split($0,a,"="); print a[2]}' | awk -F'"' '{print $2}' | tr -d '\n'
}

check_switcher() {
    if [ -f $sysdir/.runGameSwitcher ]; then
        launch_switcher
    elif [ -f /tmp/quick_switch ]; then
        # Quick switch
        rm -f /tmp/quick_switch
    else
        # Return to MainUI
        rm $sysdir/cmd_to_run.sh 2> /dev/null
        sync
    fi

    check_off_order "End"
}

launch_switcher() {
    log "\n:: Launch switcher"
    cd $sysdir
    start_audioserver
    LD_PRELOAD="$miyoodir/lib/libfbpin.so:$miyoodir/lib/libpadsp.so" gameSwitcher
    rm $sysdir/.runGameSwitcher
    set_prev_state "switcher"
    sync
}

check_off_order() {
    if [ -f /tmp/.offOrder ]; then
        touch /tmp/shutting_down

        #EmuDeck - CheckOff scripts
        check_off_scripts=$(find "$sysdir/checkoff" -type f -name "*.sh")

        for check_off_script in $check_off_scripts; do
            sh "$check_off_script"
        done

        bootScreen "$1" &
        sleep 1 # Allow the bootScreen to be displayed
        shutdown
    fi
}

recentlist=/mnt/SDCARD/Roms/recentlist.json
recentlist_hidden=/mnt/SDCARD/Roms/recentlist-hidden.json
recentlist_temp=/tmp/recentlist-temp.json

check_hide_recents() {
    # Hide recents on
    if [ ! -f $sysdir/config/.showRecents ]; then
        # Hide recents by removing the json file
        if [ -f $recentlist ]; then
            cat $recentlist $recentlist_hidden > $recentlist_temp
            mv -f $recentlist_temp $recentlist_hidden
            rm -f $recentlist
        fi
    else
        # Restore recentlist
        if [ -f $recentlist_hidden ]; then
            cat $recentlist $recentlist_hidden > $recentlist_temp
            mv -f $recentlist_temp $recentlist
            rm -f $recentlist_hidden
        fi
    fi
    sync
}

mainui_target=$miyoodir/app/MainUI

mount_main_ui() {
    mainui_mode=$([ -f $sysdir/config/.showExpert ] && echo "expert" || echo "clean")
    mainui_srcname="MainUI-$DEVICE_ID-$mainui_mode"
    mainui_mount=$(basename "$(cat /proc/self/mountinfo | grep $mainui_target | cut -d' ' -f4)")

    if [ "$mainui_mount" != "$mainui_srcname" ]; then
        if mount | grep -q "$mainui_target"; then
            umount $mainui_target 2> /dev/null
        fi

        if [ ! -f $mainui_target ]; then
            touch $mainui_target
        fi

        mount -o bind "$sysdir/bin/$mainui_srcname" $mainui_target
    fi
}

#
#   The panel's native mode, as "WxH", or empty if it cannot be read.
#
#   Two firmware families describe the panel differently, and the difference
#   is not cosmetic:
#
#     Flip     "Current TimingWidth=752,TimingWidth=560,..." - the panel's own
#              timing, which is what we want, and which is NOT the framebuffer's
#              current mode: a Flip boots at 640x480 and is pinned to 752x560
#              afterwards.
#
#     Mini v4  has no such line at all. It reports the framebuffer geometry as
#              "xres=752, yres=560" - and on a v4 that already is the panel's
#              native mode at boot, so it is the right answer there.
#
#   Order matters. The timing line wins wherever it exists, because on a Flip
#   the xres/yres pair is the unpinned 640x480 mode and would be the wrong
#   answer. Only when the timing line is absent do we fall back to xres/yres.
#
#   A Mini v4 falling through to the fallback is exactly why Onion used to lock
#   it to 480p: the old probe looked for the timing line only, found nothing,
#   spent five seconds retrying, and gave up on 640x480 while the panel sat
#   there at its native 752x560.
#
read_panel_mode() {
    # sed -n ... p rather than grep | sed: a non-matching line must produce
    # nothing, not pass through unmodified. The upstream expression also
    # required a comma after the second value - real hardware prints
    # "...,TimingWidth=560,hstar=192" so it matched, but a firmware ending the
    # line there would have leaked the raw text through as the "resolution".
    _pm=$(sed -n 's/.*Current TimingWidth=\([0-9][0-9]*\),TimingWidth=\([0-9][0-9]*\).*/\1x\2/p' \
        /proc/mi_modules/fb/mi_fb0 2> /dev/null | tr -d '\r' | head -n 1)

    if [ -n "$_pm" ]; then
        echo "$_pm"
        return
    fi

    # "xres=752, yres=560". Anchored so xres_virtual/yres_virtual cannot match,
    # and tolerant of stray carriage returns.
    sed -n 's/^[[:space:]]*xres=\([0-9][0-9]*\),[[:space:]]*yres=\([0-9][0-9]*\).*/\1x\2/p' \
        /proc/mi_modules/fb/mi_fb0 2> /dev/null | tr -d '\r' | head -n 1
}

#
#   The framebuffer's CURRENT mode, as "WxH". Always the xres/yres pair - on a
#   Flip this is 640x480 before pinning, on a Mini v4 it is already 752x560.
#   Used to decide whether pinning has anything left to do.
#
read_fb_mode() {
    sed -n 's/^[[:space:]]*xres=\([0-9][0-9]*\),[[:space:]]*yres=\([0-9][0-9]*\).*/\1x\2/p' \
        /proc/mi_modules/fb/mi_fb0 2> /dev/null | tr -d '\r' | head -n 1
}

#
#   Is this panel 752x560?
#
#   Purely a property of the panel and the firmware - no model check. The Mini
#   v4 reports DEVICE_ID 283, the same as the 480p Mini v1-v3, so the model
#   could never have separated them; read_panel_mode() identifies the panel
#   directly instead, which is both correct and safe on every device.
#
#   Takes the probed mode as $1. Fails closed: an unreadable firmware version
#   means 640x480.
#
panel_is_560p() {
    p560_reason=""

    # get_screen_resolution() runs twice - once before pinning and again from
    # init_system() - so a pin that failed its read-back has to stay failed,
    # or the second pass would re-enable 560p behind the revert's back. In
    # /tmp, so it lasts the session and no longer.
    if [ -f /tmp/pin_560p_failed ]; then
        p560_reason="an earlier pin attempt failed; holding 640x480 this session"
        return 1
    fi

    if [ "$1" != "752x560" ]; then
        p560_reason="panel reports '$1', not 752x560"
        return 1
    fi

    p560_fw=$(/etc/fw_printenv miyoo_version 2> /dev/null | cut -d'=' -f2 | tr -d '\r')
    case "$p560_fw" in
    '' | *[!0-9]*)
        p560_reason="752x560 panel but firmware unreadable ('$p560_fw')"
        return 1
        ;;
    esac

    if [ "$p560_fw" -lt 202310271401 ] 2> /dev/null; then
        p560_reason="752x560 panel but firmware $p560_fw is older than 202310271401"
        return 1
    fi

    p560_reason="752x560 panel on firmware $p560_fw"
    return 0
}

#
#   attempts to retrieve the physical screen resolution from mi_fb module
#   resolution is stored in /tmp/screen_resolution
#   times out after 5 seconds, defaults to 640x480
#
get_screen_resolution() {
    max_attempts=10
    attempt=0

    log "get_screen_resolution: start"
    while [ "$attempt" -lt "$max_attempts" ]; do
        screen_resolution=$(read_panel_mode)
        if [ -n "$screen_resolution" ]; then
            log "get_screen_resolution: success, resolution: $screen_resolution"
            break
        fi
        log "get_screen_resolution: attempt $attempt failed"
        attempt=$((attempt + 1))
        sleep 0.5
    done

    if [ -z "$screen_resolution" ]; then
        log "get_screen_resolution: failed to get screen resolution, fall back to 640x480"
        touch /tmp/get_screen_resolution_failed
    fi

    # Records the probe result before the gate rewrites it, for the report.
    p560_probed="$screen_resolution"

    if panel_is_560p "$screen_resolution"; then
        touch /tmp/new_res_available
        screen_resolution="752x560"
    else
        # can't use 752x560 without appropriate firmware or screen
        screen_resolution="640x480"
    fi
    log "get_screen_resolution: probed '$p560_probed' -> $screen_resolution ($p560_reason)"

    echo -n "$screen_resolution" > /tmp/screen_resolution

    # The UI renders at the panel's own resolution. Onion's own binaries read
    # their geometry from the framebuffer, so this is all it takes for them to
    # lay out natively; MainUI is handled separately (see launch_main_ui).
    #
    # Once the framebuffer has been pinned, that decision stands for the rest of
    # the session - this function runs a second time from init_system(), and if
    # the early probe timed out and pinned 640x480 while this one succeeds, the
    # two must not disagree. The pinned mode is what everything is actually
    # rendering into.
    pinned=$(cat /tmp/fb_target_res 2> /dev/null)
    if [ -n "$pinned" ]; then
        ui_resolution="$pinned"
    else
        ui_resolution="$screen_resolution"
    fi
}

#
#   Set the framebuffer to the session's mode and hold it there.
#
#   Everything Onion draws, and every game, runs in this one mode for the whole
#   session. That is the point: on 752x560 panels the SigmaStar GOP scaler is
#   visibly garbled for ~2s every time the mode changes, and the only reliable
#   fix is to never change it.
#
#   No-op on 640x480 panels - the kernel already hands over that mode, so there
#   is nothing to set and nothing to defend.
#
pin_ui_resolution() {
    res_x=$(echo "$ui_resolution" | cut -d 'x' -f 1)
    res_y=$(echo "$ui_resolution" | cut -d 'x' -f 2)

    # Recorded for write_560p_report, which runs after this returns.
    pin_before=$(cat /sys/class/graphics/fb0/virtual_size 2> /dev/null)
    pin_reverted=0

    # libfbpin reads this and rewrites any mode set that disagrees with it.
    echo -n "$ui_resolution" > /tmp/fb_target_res

    # 640x480 panels never scale, so there is no GOP reconfiguration to hide
    # and nothing to pin - the kernel already hands over that mode. Setting it
    # anyway is actively harmful: `fbset -g 640 480 640 960 32` would drop
    # yres_virtual from the 1440 (three buffers) a Mini/Mini+ ships with to 960
    # (two), which is a real regression, and would blank the backlight, zero the
    # framebuffer and stall 400ms at every boot for no reason.
    #
    # This is the same test libfbpin applies internally before it will rewrite
    # anything, so the two agree on which devices are left alone.
    if [ "$res_x" = "640" ] && [ "$res_y" = "480" ]; then
        log "pin_ui_resolution: 640x480 panel, nothing to pin"
        return
    fi

    # A Mini v4 boots with the framebuffer already at its native 752x560 and
    # three buffers (yres_virtual 1680). There is nothing to pin, and running
    # fbset anyway would set yres_virtual to 1120 and cost it a buffer - the
    # same regression the 640x480 short-circuit above exists to avoid. Compare
    # the mode itself rather than virtual_size, so any buffer count is left
    # alone.
    if [ "$(read_fb_mode)" = "$ui_resolution" ]; then
        log "pin_ui_resolution: framebuffer is already $ui_resolution, nothing to pin"
        return
    fi

    current=$(cat /sys/class/graphics/fb0/virtual_size 2> /dev/null)
    if [ "$current" = "${res_x},$((res_y * 2))" ]; then
        log "pin_ui_resolution: already $ui_resolution"
        return
    fi

    log "pin_ui_resolution: pinning framebuffer to $ui_resolution"

    # The switch itself is a visible scaler reconfiguration, so hide it behind
    # the backlight. The firmware has already exported the PWM by this point,
    # even though Onion does not configure brightness until init_system().
    _bl=/sys/class/pwm/pwmchip0/pwm0/duty_cycle
    _bl_prev=""
    if [ -w "$_bl" ]; then
        _bl_prev=$(cat "$_bl" 2> /dev/null)
        echo 0 > "$_bl" 2> /dev/null
        # duty=0 can leave the panel faintly lit; disabling the PWM is how
        # Onion itself blanks (bin/adv/advexec.sh).
        echo 0 > /sys/class/pwm/pwmchip0/pwm0/enable 2> /dev/null
    fi

    fbset -g "$res_x" "$res_y" "$res_x" "$((res_y * 2))" 32

    # Confirm the mode actually took before anything renders into it. A panel
    # that cannot do 752x560 must fall back to 640x480 rather than be left
    # displaying garbage - this is the safety net for the Mini v4 path, which
    # qualifies on model+firmware rather than on a probe of the panel itself.
    # Checked while the backlight is still down, so a revert is never seen.
    # Ask the driver what mode it is actually in. virtual_size is the wrong
    # question: the driver is free to keep three buffers, so yres_virtual may
    # legitimately not be res_y * 2.
    pin_readback=$(read_fb_mode)
    if [ "$pin_readback" != "$ui_resolution" ]; then
        log "pin_ui_resolution: FAILED - wanted $ui_resolution, got '$pin_readback'; reverting to 640x480"
        pin_reverted=1
        touch /tmp/pin_560p_failed
        fbset -g 640 480 640 960 32
        res_x=640
        res_y=480
        ui_resolution="640x480"
        screen_resolution="640x480"
        echo -n "640x480" > /tmp/fb_target_res
        echo -n "640x480" > /tmp/screen_resolution
        rm -f /tmp/new_res_available
    fi

    # Zero the aperture so the region that becomes visible when the mode grows
    # is black rather than stale content read at the new stride.
    cat /dev/zero > /dev/fb0 2> /dev/null

    # Let the scaler settle before the panel is lit again.
    usleep 400000

    if [ -n "$_bl_prev" ]; then
        echo "$_bl_prev" > "$_bl" 2> /dev/null
        echo 1 > /sys/class/pwm/pwmchip0/pwm0/enable 2> /dev/null
    fi
}

#
#   Write a panel/firmware report to the root of the SD card.
#
#   Exists because the Mini v4 is unverified and we have no v4 in-house: it
#   reports 480p panels and 560p panels alike as DEVICE_ID 283, and we do not
#   yet know whether its panel probe advertises 752x560, whether its firmware
#   is readable through fw_printenv, or whether libfbpin will agree to pin. A
#   tester boots this build and sends back one file that answers all three.
#
#   Deliberately not behind $sysdir/config/.logging - a tester should not have
#   to enable logging first - and deliberately at the top level of the card
#   rather than in $sysdir/logs, so it can be found without instructions.
#   Appends, so repeated boots accumulate rather than overwrite the one boot
#   that is being asked about.
#
#   Kept for the confirmation round on a Mini v4; remove it, and the
#   .fbpin_debug flag shipped beside it, before general release.
#
report_560p=/mnt/SDCARD/560p_report.txt

write_560p_report() {
    {
        echo "==============================================================="
        echo "Onion 560p report - $(date +"%Y-%m-%d %H:%M:%S")"
        echo "Onion version: $(cat $sysdir/onionVersion/version.txt 2> /dev/null) $(cat $sysdir/onionVersion/variant.txt 2> /dev/null)"
        echo "==============================================================="

        echo
        echo "--- model ---"
        echo "/tmp/deviceModel: $(cat /tmp/deviceModel 2> /dev/null)"
        echo "hall sensor node: $([ -e /sys/devices/soc0/soc/soc:hall-mh248/hallvalue ] && echo present || echo absent)"
        echo "/dev/input/event1: $([ -e /dev/input/event1 ] && echo present || echo absent)"
        echo "axp probe: $(axp 0 > /dev/null 2>&1 && echo ok || echo "failed (no PMU)")"

        echo
        echo "--- firmware ---"
        echo "/etc/fw_printenv: $([ -x /etc/fw_printenv ] && echo executable || echo "MISSING or not executable")"
        echo "raw output: [$(/etc/fw_printenv miyoo_version 2>&1)]"
        echo "exit status: $(/etc/fw_printenv miyoo_version > /dev/null 2>&1; echo $?)"
        echo "parsed value: [$(/etc/fw_printenv miyoo_version 2> /dev/null | cut -d'=' -f2)]"
        echo "gate is >= 202310271401"

        echo
        echo "--- panel probe ---"
        echo "grep result the gate sees: [$(grep 'Current TimingWidth=' /proc/mi_modules/fb/mi_fb0 2>&1 | sed 's/Current TimingWidth=\([0-9]*\),TimingWidth=\([0-9]*\),.*/\1x\2/')]"
        echo "probe timed out: $([ -f /tmp/get_screen_resolution_failed ] && echo YES || echo no)"
        echo "/proc/mi_modules/fb/mi_fb0:"
        cat /proc/mi_modules/fb/mi_fb0 2>&1 | sed 's/^/  | /'

        echo
        echo "--- decision ---"
        echo "panel reports:    [$p560_probed]"
        echo "framebuffer mode: [$(read_fb_mode)]"
        echo "verdict: $([ -f /tmp/new_res_available ] && echo 560p || echo 480p) - $p560_reason"

        echo
        echo "--- framebuffer ---"
        echo "virtual_size before pin: [$pin_before]"
        echo "virtual_size after pin:  [$(cat /sys/class/graphics/fb0/virtual_size 2> /dev/null)]"
        echo "read-back reverted the pin: $([ "$pin_reverted" = "1" ] && echo YES || echo no)"
        echo "fbset -i:"
        fbset -i 2>&1 | sed 's/^/  | /'

        echo
        echo "--- resulting flags ---"
        echo "/tmp/screen_resolution: [$(cat /tmp/screen_resolution 2> /dev/null)]"
        echo "/tmp/fb_target_res:     [$(cat /tmp/fb_target_res 2> /dev/null)]"
        echo "/tmp/new_res_available: $([ -f /tmp/new_res_available ] && echo present || echo absent)"
        echo
    } >> "$report_560p" 2>&1

    sync
}

#
#   libfbpin appends to /tmp/fbpin.log whenever /mnt/SDCARD/.fbpin_debug
#   exists, recording every mode set it saw and whether it rewrote it. That is
#   the direct evidence for whether libfbpin agrees the panel scales, so copy
#   it somewhere the tester can reach. Runs late, once apps have started.
#
copy_fbpin_log() {
    if [ -f /tmp/fbpin.log ]; then
        cp -f /tmp/fbpin.log /mnt/SDCARD/560p_fbpin.log 2> /dev/null
        sync
    fi
}

mute_theme_bgm() {
    system_theme="$(/customer/app/jsonval theme)"
    bgm_file="${system_theme}sound/bgm.mp3"
    muted_bgm_file="${system_theme}sound/bgm_muted.mp3"

    if [ -f "$sysdir/config/.bgmMute" ]; then
        if [[ -f "$bgm_file" ]]; then
            mv -f "$bgm_file" "$muted_bgm_file"
        fi
    else
        if [[ -f "$muted_bgm_file" ]]; then
            mv -f "$muted_bgm_file" "$bgm_file"
        fi
    fi
}

create_swap() {
    swapfile="/mnt/SDCARD/cachefile"
    if [ ! -e "$swapfile" ]; then
        log "Creating swap file"
        dd if=/dev/zero of="$swapfile" bs=1M count=128
        mkswap "$swapfile"
    fi
    log "Enabling swap"
    swapon "$swapfile"
}

init_system() {
    log "\n:: Init system"

    create_swap
    load_settings

    # init_lcd
    cat /proc/ls
    sleep 0.25

    # setup loopback interface for RetroArch CMDs
    ip addr add 127.0.0.1/8 dev lo
    ifconfig lo up

    if [ $DEVICE_ID -eq $MODEL_MMP ] && [ -f $sysdir/config/.lcdvolt ]; then
        $sysdir/script/lcdvolt.sh 2> /dev/null
    fi

    start_audioserver

    brightness=$(/customer/app/jsonval brightness)
    brightness_raw=$(awk "BEGIN { print int(3 * exp(0.350656 * $brightness) + 0.5) }")
    log "brightness: $brightness -> $brightness_raw"

    # init backlight
    echo 0 > /sys/class/pwm/pwmchip0/export
    pwmfile="$sysdir/config/.pwmfrequency"
    if [ -s $pwmfile ]; then
        # 0 - 9 = 100 - 1000 Hz
        frequency=$((($(cat "$pwmfile") + 1) * 100))
    else
        frequency=800
    fi
    echo $frequency > /sys/class/pwm/pwmchip0/pwm0/period
    echo $brightness_raw > /sys/class/pwm/pwmchip0/pwm0/duty_cycle
    echo 1 > /sys/class/pwm/pwmchip0/pwm0/enable

    get_screen_resolution
}

device_uuid=$(read_uuid)
device_settings="/mnt/SDCARD/.tmp_update/config/system/$device_uuid.json"

load_settings() {
    if [ -f "$device_settings" ]; then
        cp -f "$device_settings" /mnt/SDCARD/system.json
    fi

    # make sure MainUI settings exist
    if [ ! -f /mnt/SDCARD/system.json ]; then
        if [ -f /appconfigs/system.json ]; then
            cp -f /appconfigs/system.json /mnt/SDCARD/system.json
        else
            cp -f $sysdir/res/miyoo${DEVICE_ID}_system.json /mnt/SDCARD/system.json
        fi
    fi

    # link /appconfigs/system.json to SD card
    if [ -L /appconfigs/system.json ] && [ "$(readlink /appconfigs/system.json)" == "/mnt/SDCARD/system.json" ]; then
        rm /appconfigs/system.json
    fi
    ln -s /mnt/SDCARD/system.json /appconfigs/system.json

    if [ $DEVICE_ID -eq $MODEL_MM ]; then
        # init charger detection
        if [ ! -f /sys/devices/gpiochip0/gpio/gpio59/direction ]; then
            echo 59 > /sys/class/gpio/export
            echo in > /sys/devices/gpiochip0/gpio/gpio59/direction
        fi

        if [ $(/customer/app/jsonval vol) -ne 20 ] || [ $(/customer/app/jsonval mute) -ne 0 ]; then
            # Force volume and mute settings
            cat /mnt/SDCARD/system.json |
                sed 's/^\s*"vol":\s*[0-9][0-9]*/\t"vol":\t20/g' |
                sed 's/^\s*"mute":\s*[0-9][0-9]*/\t"mute":\t0/g' \
                    > temp
            mv -f temp /mnt/SDCARD/system.json
        fi
    fi

    default_volume="${sysdir}/config/.defaultVolume-${device_uuid}"
    if [ -f "$default_volume" ]; then
        volume=$(printf '%d' "$(cat "$default_volume")")
        if [ $? -eq 0 ]; then
            cat /mnt/SDCARD/system.json |
                sed 's/^\s*"vol":\s*[0-9][0-9]*/\t"vol":\t'$volume'/g' > temp
            mv -f temp /mnt/SDCARD/system.json
        fi
    fi
}

save_settings() {
    if [ -f /mnt/SDCARD/system.json ]; then
        cp -f /mnt/SDCARD/system.json "$device_settings"
    fi
}

update_time() {
    # Detect RTC, if available, do not restore time
    rtc_treshold=15 # usually around 3-5 at this point
    current_time=$(date +%s)

    if [ "$current_time" -gt "$rtc_treshold" ]; then
        log "RTC available, not restoring time. Current time: $current_time"
        touch /tmp/rtc_available
        return
    else
        log "RTC not available, restoring time. Current time: $current_time"
    fi

    timepath=/mnt/SDCARD/Saves/CurrentProfile/saves/currentTime.txt
    currentTime=0
    # Load current time
    if [ -f $timepath ]; then
        log "Restoring time"
        currentTime=$(cat $timepath)
    fi
    date +%s -s @$currentTime

    # Ensure that all play activities are closed
    playActivity stop_all

    #Add 4 hours to the current time
    hours=4
    if [ -f $sysdir/config/startup/addHours ]; then
        hours=$(cat $sysdir/config/startup/addHours)
    fi
    addTime=$(($hours * 3600))
    if [ ! -f $sysdir/config/.ntpState ]; then
        currentTime=$(($currentTime + $addTime))
    fi
    date +%s -s @$currentTime
}

set_startup_tab() {
    startup_tab=0
    if [ -f $sysdir/config/startup/tab ]; then
        startup_tab=$(cat $sysdir/config/startup/tab)
    fi

    cd $sysdir
    setState "$startup_tab"
}

start_audioserver() {
    defvol=$(echo $(/customer/app/jsonval vol) | awk '{ printf "%.0f\n", 48 * (log(1 + $1) / log(10)) - 60 }')
    runifnecessary "audioserver" $miyoodir/app/audioserver $defvol
}

runifnecessary() {
    cnt=0
    #a=`ps | grep $1 | grep -v grep`
    a=$(pgrep $1)
    while [ "$a" == "" ] && [ $cnt -lt 8 ]; do
        log "try to run: $2"
        $2 $3 &
        sleep 0.5
        cnt=$(expr $cnt + 1)
        a=$(pgrep $1)
    done
}

start_networking() {
    rm $sysdir/config/.hotspotState # dont start hotspot at boot

    touch /tmp/network_changed
    sync

    check_networking
}

check_networking() {
    if [ ! -f /tmp/network_changed ]; then
        return
    fi

    if pgrep -f update_networking.sh; then
        log "update_networking already running"
    else
        rm /tmp/network_changed
        $sysdir/script/network/update_networking.sh check
    fi
}

check_installer() {
    # Check if installer is present
    if [ -d $miyoodir/app/.tmp_update ] && fgrep -q "#!/bin/sh" "$miyoodir/app/MainUI"; then
        echo "Installer detected!"
        cd $miyoodir/app
        ./MainUI
        reboot
        sleep 10
        exit
    fi
}

if [ -f $sysdir/config/.logging ]; then
    main
else
    main 2>&1 > /dev/null
fi

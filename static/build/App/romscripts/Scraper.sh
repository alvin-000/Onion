#!/bin/sh
scriptlabel="Scraper (%LIST%)"
scriptinfo="Launches the scraper\nfor the selected system."
require_networking=1
echo $0 $*

sysdir=/mnt/SDCARD/.tmp_update
rm -f /tmp/scraper_script.sh

pressMenu2Kill st &

cd $sysdir
#./bin/st -q -e 	"/mnt/SDCARD/scrap_screenscraper.sh" "MD"   # quick alternative
$sysdir/script/run_scaled.sh ./bin/st -q -e "$sysdir/script/scraper/menu.sh" "$1" "$2"

pkill -9 pressMenu2Kill

# MENU is how the scraper is exited - pressMenu2Kill above watches for it and
# kills st. keymon sees the same press though, and in the MainUI state runs
# action_MainUI_gameSwitcher(), which leaves .runGameSwitcher behind. On the way
# out runtime.sh's check_switcher() finds that flag and opens GameSwitcher
# instead of returning to the game list. The press was consumed by us, so drop
# the request it triggered.
rm -f $sysdir/.runGameSwitcher


# background scraping :
if [ -f /tmp/scraper_script.sh ]; then
    chmod a+x /tmp/scraper_script.sh
    sh /tmp/scraper_script.sh &
fi

# Return to the game list instead of launching the rom.
#
# GLO propagates this script's exit status (game_list_options.sh: exit $?), and
# runtime.sh treats non-zero as "back to MainUI" while zero leaves cmd_to_run.sh
# in place and starts the game. This script used to end on an if-block, which
# yields 0 when its condition is false - so picking Exit in the scraper menu
# launched whatever rom was selected. Netplay ends the same way on purpose,
# since starting the game is the point there.
exit 1

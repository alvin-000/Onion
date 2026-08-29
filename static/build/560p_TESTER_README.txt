Onion 4.5.0-beta - Miyoo Mini v4
================================

Thank you for the last round - the report you sent found it.

Your Mini v4 describes its screen differently from the Mini Flip. Onion was
looking for one particular line to learn the panel size, your firmware does
not print that line, so Onion gave up and fell back to 640x480 even though the
screen was already running at its full 752x560. RetroArch never asked that
question, which is why games looked right and the menus did not.

Onion now reads both formats, so your device is recognised on its own.


WHAT TO DO
----------
There is nothing to set up this time - no file to create, no setting to turn
on. Just flash the card and install as normal.

Then use it and look for anything that garbles, tears, flickers or sits in the
wrong place on screen:

    - the installer / first-time setup, while it runs
    - the main menu
    - a game
    - the Clock app, and File Explorer
    - the volume and brightness bars

The setup screens and the moment you launch or exit a game are the two places
most worth watching, because they are what changed.

If it all looks right, just say so - that is all we need.


IF SOMETHING LOOKS WRONG
------------------------
Tell us what you saw and where, and send this file from the root of the SD
card:

    560p_report.txt

It records what the device decided about the screen and why. There is also a
560p_fbpin.log next to it - send that too if it is there.


WHAT THE REPORT CONTAINS
------------------------
Panel timings, firmware version, detected model, and framebuffer settings. No
personal data, no save files, no network details.

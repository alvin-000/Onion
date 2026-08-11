#include <SDL/SDL.h>
#include <SDL/SDL_image.h>
#include <assert.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "utils/sdl_legacy.h"
#include "system/battery.h"
#include "system/device_model.h"
#include "system/display.h"
#include "system/keymap_hw.h"
#include "system/rumble.h"
#include "system/settings.h"
#include "system/system.h"
#include "theme/config.h"
#include "utils/file.h"
#include "utils/log.h"
#include "utils/msleep.h"

#define RELEASED 0
#define PRESSED 1
#define REPEATING 2

#define DISPLAY_TIMEOUT 15000

static bool quit = false;
static bool suspended = false;
static int input_fd;
static struct input_event ev;
static struct pollfd fds[1];

void getImageDir(const char *theme_path, char *image_dir)
{
    char image0_path[STR_MAX * 2];
    sprintf(image0_path, "%s/skin/extra/chargingState0.png", THEME_OVERRIDES);
    if (exists(image0_path)) {
        sprintf(image_dir, "%s/skin/extra", THEME_OVERRIDES);
        return;
    }

    sprintf(image0_path, "%sskin/extra/chargingState0.png", theme_path);
    if (exists(image0_path)) {
        sprintf(image_dir, "%sskin/extra", theme_path);
        return;
    }

    strcpy(image_dir, "res");
}

static uint32_t saved_brightness = 0;

//
//    Blank the panel with the PWM backlight, not display_setScreen().
//
//    display_setScreen(false) does two things beyond dropping the screen: it
//    pulls GPIO4 low, and it calls display_save(), which reads
//    g_display.fb_addr. chargingState never calls display_init() - it opens the
//    display through legacy_openDisplay(), and display_getRenderResolution()
//    reads the geometry without ever mmapping - so that pointer is null and
//    display_save() walks it. Called from inside the loop, that killed the
//    process silently: traced on a Flip, "idle timeout -> suspending" was the
//    last line written and the process was gone two seconds later, never
//    reaching the next trace point.
//
//    GPIO4 is the other half of the original bug. Nothing raises it again on
//    the next boot, so a device that was blanked this way and then powered off
//    comes back up with a dark panel - which is exactly what a charging boot
//    looked like.
//
void suspend(bool enabled, SDL_Surface *video)
{
    suspended = enabled;
    if (suspended) {
        SDL_FillRect(video, NULL, 0);
        SDL_Flip(video);
        saved_brightness = display_getBrightnessRaw();
        display_setBrightnessRaw(0);
    }
    else {
        display_setBrightnessRaw(saved_brightness ? saved_brightness : 40);
    }
}

static void sigHandler(int sig)
{
    switch (sig) {
    case SIGINT:
    case SIGTERM:
        quit = true;
        break;
    default:
        break;
    }
}

int main(void)
{
    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    bool turn_off = false;

    settings_load();
    display_setBrightness(settings.brightness);

    char image_dir[STR_MAX];
    getImageDir(settings.theme, image_dir);

    getDeviceModel();

    SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_EnableKeyRepeat(300, 50);

    // Draw at 640x480 as always; legacy_present() upscales to the panel.
    SDL_Surface *screen = NULL;
    SDL_Surface *video = legacy_openDisplay(640, 480, SDL_HWSURFACE, &screen);

    int min_delay = 15;
    int frame_delay = 80;
    int frame_count = 0;

    SDL_Surface *frames[24];
    SDL_Surface *image;

    for (int i = 0; i < 24; i++) {
        char image_path[STR_MAX + 50];
        snprintf(image_path, STR_MAX + 49, "%s/chargingState%d.png", image_dir,
                 i);
        if ((image = IMG_Load(image_path)))
            frames[frame_count++] = image;
    }

    char json_path[STR_MAX + 20];
    snprintf(json_path, STR_MAX + 19, "%s/chargingState.json", image_dir);
    if (is_file(json_path)) {
        int value;
        char json_value[STR_MAX];
        if (file_parseKeyValue(json_path, "frame_delay", json_value, ':', 0) !=
            NULL) {
            value = atoi(json_value);
            // accept both microseconds and milliseconds
            frame_delay = value >= 10000 ? value / 1000 : value;
        }
    }

    // Prepare for Poll button input
    input_fd = open("/dev/input/event0", O_RDONLY);
    memset(&fds, 0, sizeof(fds));
    fds[0].fd = input_fd;
    fds[0].events = POLLIN;

    if (frame_delay < min_delay)
        frame_delay = min_delay;

    printf_debug("Frame count: %d\n", frame_count);
    printf_debug("Frame delay: %d ms\n", frame_delay);

    bool power_pressed = false;
    int repeat_power = 0;

    int current_frame = 0;

    // Set the CPU to powersave (charges faster?)
    system_powersave_on();

    uint32_t acc_ticks = 0, last_ticks = SDL_GetTicks(),
             display_timer = last_ticks;

    while (!quit) {
        while (poll(fds, 1, suspended ? 1000 - min_delay : 0)) {
            read(input_fd, &ev, sizeof(ev));

            if (ev.type != EV_KEY || ev.value > REPEATING)
                continue;

            if (ev.code == HW_BTN_POWER) {
                if (ev.value == PRESSED) {
                    power_pressed = true;
                    repeat_power = 0;
                }
                else if (ev.value == RELEASED && power_pressed) {
                    if (suspended) {
                        acc_ticks = 0;
                        last_ticks = SDL_GetTicks();
                    }
                    suspend(!suspended, video);
                    power_pressed = false;
                }
                else if (ev.value == REPEATING) {
                    if (repeat_power >= 5) {
                        quit = true; // power on
                        break;
                    }
                    repeat_power++;
                }
            }

            display_timer = SDL_GetTicks();
        }

        if (!battery_isCharging()) {
            quit = true;
            turn_off = true;
            break;
        }

        if (quit)
            break;

        uint32_t ticks = SDL_GetTicks();

        if (!suspended) {
            if (ticks - display_timer >= DISPLAY_TIMEOUT) {
                // Power off on devices that can, blank the screen on those
                // that cannot - upstream's original split, restored.
                //
                // This never worked before, but not because the design was
                // wrong: display_setScreen(false) crashed in display_save()
                // (see suspend() above) on the line immediately before
                // system("shutdown"), so the shutdown was never reached and the
                // device carried on booting with the panel disabled. With that
                // call replaced, the power-off actually happens.
                //
                // Verified: a manual shutdown with the charger attached stays
                // off - the charger does not power the device back on - so
                // there is nothing to be gained by suspending here instead.
                //
                // The Mini keeps suspending because it has no poweroff:
                // bin/shutdown reboots on device 283 rather than powering down.
                if (HAS_AXP()) {
                    quit = true;
                    turn_off = true;
                    break;
                }
                else {
                    suspend(true, video);
                    continue;
                }
            }

            acc_ticks += ticks - last_ticks;
            last_ticks = ticks;

            if (acc_ticks >= frame_delay) {
                // Clear screen
                SDL_FillRect(screen, NULL, 0);

                if (current_frame < frame_count) {
                    SDL_Surface *frame = frames[current_frame];
                    SDL_Rect frame_rect = {320 - frame->w / 2,
                                           240 - frame->h / 2};
                    SDL_BlitSurface(frame, NULL, screen, &frame_rect);
                    current_frame = (current_frame + 1) % frame_count;
                }

                legacy_present(screen, video);

                acc_ticks -= frame_delay;
            }
        }

        msleep(min_delay);
    }


    // Clear the screen when exiting
    SDL_FillRect(video, NULL, 0);
    SDL_Flip(video);

#ifndef PLATFORM_MIYOOMINI
    msleep(100);
#endif

    for (int i = 0; i < frame_count; i++)
        SDL_FreeSurface(frames[i]);
    SDL_FreeSurface(screen);
    SDL_FreeSurface(video);
    SDL_Quit();

    if (turn_off) {
#ifdef PLATFORM_MIYOOMINI
        // Backlight only - not display_setScreen(false), which pulls GPIO4 low
        // and leaves it there. Nothing raises it on the next boot, so a device
        // powered off this way came back with a dark panel.
        display_setBrightnessRaw(0);
        system("shutdown; sleep 10");
#endif
    }
    else {
#ifdef PLATFORM_MIYOOMINI
        display_setScreen(true);
        short_pulse();
#endif
    }

    // restore CPU performance mode
    system_powersave_off();

    return EXIT_SUCCESS;
}

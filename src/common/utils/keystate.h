#ifndef KEYSTATE_H__
#define KEYSTATE_H__

#include <SDL/SDL.h>

#include "./msleep.h"

typedef enum { RELEASED,
               PRESSED,
               REPEATING } KeyState;

static SDL_Event keystate_event;

//
//    Drains the event queue into keystate[], but stops short of a release that
//    would erase a press this call has not reported yet.
//
//    keystate[] holds one value per key, so a press and its release arriving in
//    the same drain both run: the key ends RELEASED and the caller never sees
//    PRESSED. The tap is not late, it is gone. That needs the frame to outlast
//    the tap, which is why it shows up on a 752x560 panel - a present there is
//    a full scale and rotate - and not on 640x480.
//
//    So a KEYUP for a key pressed during this same call is left on the queue
//    and handled next time round. The release is deferred by one iteration
//    rather than dropped, and every other event path is unchanged.
//
bool updateKeystate(KeyState keystate[320], bool *quit_flag, bool enabled,
                    SDLKey *changed_key)
{
    bool retval = false;
    bool pressed_here[320] = {false};
    SDL_Event peek;

    // SDL_PollEvent pumps the event loop itself; SDL_PeepEvents does not.
    SDL_PumpEvents();

    while (SDL_PeepEvents(&peek, 1, SDL_PEEKEVENT, SDL_ALLEVENTS) > 0) {
        SDLKey key;

        // Leave it queued - reading it now would undo a press the caller has
        // not had a chance to act on. Only applies while enabled, or a
        // disabled app would stop consuming and let the queue grow.
        if (enabled && peek.type == SDL_KEYUP &&
            peek.key.keysym.sym < 320 && pressed_here[peek.key.keysym.sym])
            break;

        if (!SDL_PollEvent(&keystate_event))
            break; // cannot happen after a successful peek; belt and braces

        key = keystate_event.key.keysym.sym;

        if (!enabled)
            continue;

        if (keystate_event.type == SDL_KEYDOWN && key < 320)
            pressed_here[key] = true;

        switch (keystate_event.type) {
        case SDL_QUIT:
            *quit_flag = true;
            if (changed_key != NULL)
                *changed_key = SDLK_UNKNOWN;
            break;
        case SDL_KEYDOWN:
            if (keystate[key] != RELEASED)
                keystate[key] = REPEATING;
            else
                keystate[key] = PRESSED;
            if (changed_key != NULL)
                *changed_key = key;
            retval = true;
            break;
        case SDL_KEYUP:
            keystate[key] = RELEASED;
            if (changed_key != NULL)
                *changed_key = key;
            retval = true;
            break;
        default:
            if (changed_key != NULL)
                *changed_key = SDLK_UNKNOWN;
            break;
        }
    }

    msleep(4);

    return retval;
}

#endif

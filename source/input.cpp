#include "input.h"

#include <cstdlib>

static PadState s_pad;
static Input s_in;
static bool s_wasTouching = false;
static bool s_moved = false;
static u64 s_lastSampling = 0;

// Répétition des directions : premier délai puis cadence (en images à 60 Hz)
static const int REPEAT_DELAY = 18;
static const int REPEAT_RATE = 4;
static const int TAP_SLOP = 18;  // pixels de tolérance pour un "tap"
static int s_repeatFrames = 0;

static const u64 DIRECTIONS = HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft | HidNpadButton_AnyRight;

void input_init()
{
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&s_pad);
    hidInitializeTouchScreen();
}

const Input& input_update()
{
    padUpdate(&s_pad);
    s_in.down = padGetButtonsDown(&s_pad);
    s_in.held = padGetButtons(&s_pad);

    s_in.repeat = s_in.down;
    if (s_in.held & DIRECTIONS) {
        s_repeatFrames++;
        if (s_repeatFrames > REPEAT_DELAY && (s_repeatFrames - REPEAT_DELAY) % REPEAT_RATE == 0)
            s_in.repeat |= s_in.held & DIRECTIONS;
    } else {
        s_repeatFrames = 0;
    }

    // Historique des échantillons (le plus récent en premier) : un toucher bref entre
    // deux images, fréquent quand KataGo occupe le processeur, n'est pas perdu.
    HidTouchScreenState states[16];
    size_t count = hidGetTouchScreenStates(states, 16);
    bool touching = count > 0 && states[0].count > 0;
    bool sawNewTouch = false;
    int firstX = 0, firstY = 0, lastX = s_in.x, lastY = s_in.y;
    for (int i = (int)count - 1; i >= 0; i--) {  // du plus ancien au plus récent
        if (states[i].sampling_number <= s_lastSampling)
            continue;
        if (states[i].count > 0) {
            if (!sawNewTouch) {
                firstX = states[i].touches[0].x;
                firstY = states[i].touches[0].y;
            }
            sawNewTouch = true;
            lastX = states[i].touches[0].x;
            lastY = states[i].touches[0].y;
        }
    }
    if (count > 0)
        s_lastSampling = states[0].sampling_number;

    s_in.touchStarted = sawNewTouch && !s_wasTouching;
    s_in.released = !touching && (s_wasTouching || sawNewTouch);
    s_in.tap = false;
    s_in.dragX = s_in.dragY = 0;
    if (s_in.touchStarted) {
        s_in.startX = firstX;
        s_in.startY = firstY;
        s_in.x = firstX;
        s_in.y = firstY;
        s_moved = false;
    }
    if (sawNewTouch) {
        s_in.dragX = lastX - s_in.x;
        s_in.dragY = lastY - s_in.y;
        s_in.x = lastX;
        s_in.y = lastY;
        if (std::abs(lastX - s_in.startX) > TAP_SLOP || std::abs(lastY - s_in.startY) > TAP_SLOP)
            s_moved = true;
    }
    if (s_in.released)
        s_in.tap = !s_moved;
    s_in.touching = touching;
    s_wasTouching = touching;
    return s_in;
}

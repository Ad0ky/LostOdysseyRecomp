#pragma once
#include <SDL3/SDL.h>
#include <cstdlib>
#include <cstring>
#include <os/platform.h>

namespace hid::face_buttons
{
// SDL2 reported Nintendo face buttons by their printed letter (its
// SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS default); SDL3 reports position.
// Keep the letter: the button labelled A confirms on every A/B/X/Y pad.
// PlayStation shapes and non-face buttons keep their position.
#if LO_PLATFORM_SWITCH
// The console's own controllers (Joy-Con, Pro Controller, handheld) are not
// reported as a Nintendo type by the libnx SDL backend, so their buttons kept
// the Xbox position: the B button confirmed. Use the printed letters there too
// (A right, B bottom, X top, Y left). LO_SWITCH_BUTTONS=position keeps the
// positional layout.
inline bool SwitchLetterLayout()
{
    static const bool letters = [] {
        const char* value = std::getenv("LO_SWITCH_BUTTONS");
        return !value || std::strcmp(value, "position") != 0;
    }();
    return letters;
}
#endif

inline SDL_GamepadButton ByLabel(SDL_GamepadType type, SDL_GamepadButton button)
{
#if LO_PLATFORM_SWITCH
    if (SwitchLetterLayout())
    {
        switch (button)
        {
        case SDL_GAMEPAD_BUTTON_EAST: return SDL_GAMEPAD_BUTTON_SOUTH;  // A
        case SDL_GAMEPAD_BUTTON_SOUTH: return SDL_GAMEPAD_BUTTON_EAST;  // B
        case SDL_GAMEPAD_BUTTON_NORTH: return SDL_GAMEPAD_BUTTON_WEST;  // X
        case SDL_GAMEPAD_BUTTON_WEST: return SDL_GAMEPAD_BUTTON_NORTH;  // Y
        default: return button;
        }
    }
#endif
    switch (SDL_GetGamepadButtonLabelForType(type, button))
    {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return SDL_GAMEPAD_BUTTON_SOUTH;
    case SDL_GAMEPAD_BUTTON_LABEL_B: return SDL_GAMEPAD_BUTTON_EAST;
    case SDL_GAMEPAD_BUTTON_LABEL_X: return SDL_GAMEPAD_BUTTON_WEST;
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return SDL_GAMEPAD_BUTTON_NORTH;
    default: return button;
    }
}

// The Xbox-position meaning of a button event.
inline SDL_GamepadButton FromEvent(const SDL_GamepadButtonEvent& event)
{
    return ByLabel(SDL_GetGamepadTypeForID(event.which), SDL_GamepadButton(event.button));
}

// Rewrites a gamepad button event in place to its Xbox-position meaning.
inline void Normalize(SDL_Event& event)
{
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
        event.gbutton.button = Uint8(FromEvent(event.gbutton));
}

// The physical button that carries the letter of Xbox position `position`.
inline SDL_GamepadButton Physical(SDL_GamepadType type, SDL_GamepadButton position)
{
    for (const auto button : {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
             SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH})
        if (ByLabel(type, button) == position) return button;
    return position;
}
}

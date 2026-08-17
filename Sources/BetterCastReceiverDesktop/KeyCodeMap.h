#pragma once

// BetterCast carries macOS virtual key codes (CGKeyCode / Carbon kVK_*) on the
// wire. That is not an arbitrary choice — the macOS sender feeds InputEvent
// .keyCode straight into CGEvent(keyboardEventSource:virtualKey:), so every
// receiver has to speak that vocabulary for typing to work.
//
// Windows uses a completely different space (VK_*), so both directions need a
// translation:
//   * Windows RECEIVER  → outgoing key events must be converted VK → mac.
//   * Windows SENDER    → incoming key events must be converted mac → VK.
//
// Before this table existed the Windows receiver sent raw VK codes, which the
// macOS sender then interpreted as mac key codes — so typing from a Windows
// receiver produced the wrong characters entirely.

#include <cstdint>

namespace KeyCodeMap {

// macOS virtual key codes (Carbon kVK_*)
enum MacKey : uint16_t {
    Mac_A = 0x00, Mac_S = 0x01, Mac_D = 0x02, Mac_F = 0x03, Mac_H = 0x04,
    Mac_G = 0x05, Mac_Z = 0x06, Mac_X = 0x07, Mac_C = 0x08, Mac_V = 0x09,
    Mac_B = 0x0B, Mac_Q = 0x0C, Mac_W = 0x0D, Mac_E = 0x0E, Mac_R = 0x0F,
    Mac_Y = 0x10, Mac_T = 0x11,
    Mac_1 = 0x12, Mac_2 = 0x13, Mac_3 = 0x14, Mac_4 = 0x15, Mac_6 = 0x16,
    Mac_5 = 0x17, Mac_Equal = 0x18, Mac_9 = 0x19, Mac_7 = 0x1A,
    Mac_Minus = 0x1B, Mac_8 = 0x1C, Mac_0 = 0x1D, Mac_RightBracket = 0x1E,
    Mac_O = 0x1F, Mac_U = 0x20, Mac_LeftBracket = 0x21, Mac_I = 0x22,
    Mac_P = 0x23, Mac_Return = 0x24, Mac_L = 0x25, Mac_J = 0x26,
    Mac_Quote = 0x27, Mac_K = 0x28, Mac_Semicolon = 0x29, Mac_Backslash = 0x2A,
    Mac_Comma = 0x2B, Mac_Slash = 0x2C, Mac_N = 0x2D, Mac_M = 0x2E,
    Mac_Period = 0x2F, Mac_Tab = 0x30, Mac_Space = 0x31, Mac_Grave = 0x32,
    Mac_Delete = 0x33, Mac_Escape = 0x35, Mac_Command = 0x37, Mac_Shift = 0x38,
    Mac_CapsLock = 0x39, Mac_Option = 0x3A, Mac_Control = 0x3B,
    Mac_RightShift = 0x3C, Mac_RightOption = 0x3D, Mac_RightControl = 0x3E,
    Mac_KeypadDecimal = 0x41, Mac_KeypadMultiply = 0x43, Mac_KeypadPlus = 0x45,
    Mac_KeypadClear = 0x47, Mac_KeypadDivide = 0x4B, Mac_KeypadEnter = 0x4C,
    Mac_KeypadMinus = 0x4E, Mac_KeypadEquals = 0x51,
    Mac_Keypad0 = 0x52, Mac_Keypad1 = 0x53, Mac_Keypad2 = 0x54,
    Mac_Keypad3 = 0x55, Mac_Keypad4 = 0x56, Mac_Keypad5 = 0x57,
    Mac_Keypad6 = 0x58, Mac_Keypad7 = 0x59, Mac_Keypad8 = 0x5B,
    Mac_Keypad9 = 0x5C,
    Mac_F5 = 0x60, Mac_F6 = 0x61, Mac_F7 = 0x62, Mac_F3 = 0x63, Mac_F8 = 0x64,
    Mac_F9 = 0x65, Mac_F11 = 0x67, Mac_F13 = 0x69, Mac_F14 = 0x6B,
    Mac_F10 = 0x6D, Mac_F12 = 0x6F, Mac_F15 = 0x71,
    Mac_Home = 0x73, Mac_PageUp = 0x74, Mac_ForwardDelete = 0x75,
    Mac_F4 = 0x76, Mac_End = 0x77, Mac_F2 = 0x78, Mac_PageDown = 0x79,
    Mac_F1 = 0x7A, Mac_Left = 0x7B, Mac_Right = 0x7C, Mac_Down = 0x7D,
    Mac_Up = 0x7E,
};

struct Pair { uint16_t mac; uint16_t vk; };

// Single source of truth. Windows VK_* values are inlined as literals so this
// header stays usable without <Windows.h>.
inline const Pair* table(int& countOut) {
    static const Pair kTable[] = {
        // Letters — VK_A..VK_Z are ASCII 'A'..'Z'
        { Mac_A, 0x41 }, { Mac_B, 0x42 }, { Mac_C, 0x43 }, { Mac_D, 0x44 },
        { Mac_E, 0x45 }, { Mac_F, 0x46 }, { Mac_G, 0x47 }, { Mac_H, 0x48 },
        { Mac_I, 0x49 }, { Mac_J, 0x4A }, { Mac_K, 0x4B }, { Mac_L, 0x4C },
        { Mac_M, 0x4D }, { Mac_N, 0x4E }, { Mac_O, 0x4F }, { Mac_P, 0x50 },
        { Mac_Q, 0x51 }, { Mac_R, 0x52 }, { Mac_S, 0x53 }, { Mac_T, 0x54 },
        { Mac_U, 0x55 }, { Mac_V, 0x56 }, { Mac_W, 0x57 }, { Mac_X, 0x58 },
        { Mac_Y, 0x59 }, { Mac_Z, 0x5A },

        // Digits — VK_0..VK_9 are ASCII '0'..'9'
        { Mac_0, 0x30 }, { Mac_1, 0x31 }, { Mac_2, 0x32 }, { Mac_3, 0x33 },
        { Mac_4, 0x34 }, { Mac_5, 0x35 }, { Mac_6, 0x36 }, { Mac_7, 0x37 },
        { Mac_8, 0x38 }, { Mac_9, 0x39 },

        // Punctuation (VK_OEM_*)
        { Mac_Semicolon,    0xBA },  // VK_OEM_1
        { Mac_Equal,        0xBB },  // VK_OEM_PLUS
        { Mac_Comma,        0xBC },  // VK_OEM_COMMA
        { Mac_Minus,        0xBD },  // VK_OEM_MINUS
        { Mac_Period,       0xBE },  // VK_OEM_PERIOD
        { Mac_Slash,        0xBF },  // VK_OEM_2
        { Mac_Grave,        0xC0 },  // VK_OEM_3
        { Mac_LeftBracket,  0xDB },  // VK_OEM_4
        { Mac_Backslash,    0xDC },  // VK_OEM_5
        { Mac_RightBracket, 0xDD },  // VK_OEM_6
        { Mac_Quote,        0xDE },  // VK_OEM_7

        // Control keys
        { Mac_Return,       0x0D },  // VK_RETURN
        { Mac_Tab,          0x09 },  // VK_TAB
        { Mac_Space,        0x20 },  // VK_SPACE
        { Mac_Delete,       0x08 },  // VK_BACK   (mac Delete == Backspace)
        { Mac_ForwardDelete,0x2E },  // VK_DELETE
        { Mac_Escape,       0x1B },  // VK_ESCAPE
        { Mac_CapsLock,     0x14 },  // VK_CAPITAL

        // Modifiers
        { Mac_Shift,        0xA0 },  // VK_LSHIFT
        { Mac_RightShift,   0xA1 },  // VK_RSHIFT
        { Mac_Control,      0xA2 },  // VK_LCONTROL
        { Mac_RightControl, 0xA3 },  // VK_RCONTROL
        { Mac_Option,       0xA4 },  // VK_LMENU  (Option == Alt)
        { Mac_RightOption,  0xA5 },  // VK_RMENU
        // Command is handled separately — see macToVk()'s commandAsControl.
        { Mac_Command,      0x5B },  // VK_LWIN

        // Navigation
        { Mac_Home,     0x24 }, { Mac_End,   0x23 },
        { Mac_PageUp,   0x21 }, { Mac_PageDown, 0x22 },
        { Mac_Left,     0x25 }, { Mac_Up,    0x26 },
        { Mac_Right,    0x27 }, { Mac_Down,  0x28 },

        // Function keys
        { Mac_F1, 0x70 }, { Mac_F2, 0x71 }, { Mac_F3,  0x72 }, { Mac_F4,  0x73 },
        { Mac_F5, 0x74 }, { Mac_F6, 0x75 }, { Mac_F7,  0x76 }, { Mac_F8,  0x77 },
        { Mac_F9, 0x78 }, { Mac_F10,0x79 }, { Mac_F11, 0x7A }, { Mac_F12, 0x7B },
        { Mac_F13,0x7C }, { Mac_F14,0x7D }, { Mac_F15, 0x7E },

        // Keypad
        { Mac_Keypad0, 0x60 }, { Mac_Keypad1, 0x61 }, { Mac_Keypad2, 0x62 },
        { Mac_Keypad3, 0x63 }, { Mac_Keypad4, 0x64 }, { Mac_Keypad5, 0x65 },
        { Mac_Keypad6, 0x66 }, { Mac_Keypad7, 0x67 }, { Mac_Keypad8, 0x68 },
        { Mac_Keypad9, 0x69 },
        { Mac_KeypadMultiply, 0x6A },  // VK_MULTIPLY
        { Mac_KeypadPlus,     0x6B },  // VK_ADD
        { Mac_KeypadEnter,    0x0D },  // VK_RETURN
        { Mac_KeypadMinus,    0x6D },  // VK_SUBTRACT
        { Mac_KeypadDecimal,  0x6E },  // VK_DECIMAL
        { Mac_KeypadDivide,   0x6F },  // VK_DIVIDE
    };
    countOut = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));
    return kTable;
}

// mac virtual key code → Windows VK. Returns 0 when unmapped.
//
// commandAsControl: map ⌘ to Ctrl rather than the Windows key. Positionally ⌘
// sits where Alt does and semantically it is the Windows key, but the shortcut
// a user actually means by ⌘C is Ctrl+C — so mapping to Ctrl is what makes
// copy, paste, save and friends work from a Mac receiver.
inline uint16_t macToVk(uint16_t macCode, bool commandAsControl = true) {
    if (commandAsControl && macCode == Mac_Command) return 0xA2;  // VK_LCONTROL

    int count = 0;
    const Pair* t = table(count);
    for (int i = 0; i < count; i++) {
        if (t[i].mac == macCode) return t[i].vk;
    }
    return 0;
}

// Windows VK → mac virtual key code. Returns 0 when unmapped.
inline uint16_t vkToMac(uint16_t vk) {
    // Normalise the ambiguous "either side" modifiers onto the left-hand key.
    switch (vk) {
        case 0x10: vk = 0xA0; break;  // VK_SHIFT   → VK_LSHIFT
        case 0x11: vk = 0xA2; break;  // VK_CONTROL → VK_LCONTROL
        case 0x12: vk = 0xA4; break;  // VK_MENU    → VK_LMENU
        default: break;
    }

    int count = 0;
    const Pair* t = table(count);
    for (int i = 0; i < count; i++) {
        if (t[i].vk == vk) return t[i].mac;
    }
    return 0;
}

// Keys that require KEYEVENTF_EXTENDEDKEY when injected with SendInput.
// Omitting the flag makes arrows and the navigation cluster behave as their
// numpad twins whenever NumLock is off.
inline bool isExtendedVk(uint16_t vk) {
    switch (vk) {
        case 0x21: case 0x22:            // PRIOR, NEXT
        case 0x23: case 0x24:            // END, HOME
        case 0x25: case 0x26:            // LEFT, UP
        case 0x27: case 0x28:            // RIGHT, DOWN
        case 0x2D: case 0x2E:            // INSERT, DELETE
        case 0x2C:                       // SNAPSHOT
        case 0x90:                       // NUMLOCK
        case 0x6F:                       // DIVIDE
        case 0xA3:                       // RCONTROL
        case 0xA5:                       // RMENU
        case 0x5B: case 0x5C:            // LWIN, RWIN
            return true;
        default:
            return false;
    }
}

} // namespace KeyCodeMap

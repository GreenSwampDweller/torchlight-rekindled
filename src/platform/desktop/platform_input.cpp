// Desktop input: a pad and a keyboard laid out like one. A pad is read exactly as the console's
// (`core/gamepad.h`); without one the keyboard stands in key for key.
//
// Windows reads the pad through XInput and the keys through GetAsyncKeyState; Linux reads the pad
// through evdev - the kernel's gamepad interface, whose BTN_SOUTH, BTN_EAST, ABS_RX, ... name
// places on the pad rather than labels, with one exception (button_bit) - and the keys through
// X11. Both are turned into one reading in XInput's layout, which the rest of this file maps.
//
// A line of text (a character's name) is typed into the window: Windows' WM_CHAR through the
// event pump (platform_frame.cpp), X11's KeyPress on the game's window.
#include "core/input.h"

#include "core/gamepad.h"
#include "platform/desktop/typed_keys.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>
#else
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#endif

#include <cmath>
#include <cstdint>
#include <string>

namespace tl::core {

namespace {

/// One pad's state in XInput's layout, whichever platform read it.
struct PadReading {
    std::int16_t lx = 0, ly = 0, rx = 0, ry = 0;  ///< up and right positive
    std::uint8_t lt = 0, rt = 0;
    std::uint16_t buttons = 0;  ///< kPad* bits
};

// XInput's button bits (XINPUT_GAMEPAD_*), spelled here for the platforms without its header.
constexpr std::uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008;
constexpr std::uint16_t kPadStart = 0x0010, kPadBack = 0x0020, kPadThumbL = 0x0040,
                        kPadThumbR = 0x0080;
constexpr std::uint16_t kPadShoulderL = 0x0100, kPadShoulderR = 0x0200;
constexpr std::uint16_t kPadA = 0x1000, kPadB = 0x2000, kPadX = 0x4000, kPadY = 0x8000;

/// The keys the keyboard stands in with, named once for both platforms.
enum class Key {
    A,
    D,
    S,
    W,
    Left,
    Right,
    Down,
    Up,
    PageUp,
    PageDown,
    Space,
    Return,
    Backspace,
    X,
    Y,
    Delete,
    Escape,
    I,
    Tab,
    Q,
    E,
    BracketLeft,
    BracketRight,
    M,
    F,
    Plus,
    KeypadPlus,
    Minus,
    KeypadMinus,
    P,
    C,
    O
};

constexpr int kKeyCount = static_cast<int>(Key::O) + 1;

bool focused();
bool key_down(Key key);
bool read_pad(PadReading& pad);

/// Typed since the last take, as UTF-8, while a line is being typed.
std::string g_typed;

/// One character typed, unless no line is being typed. Backspace, Enter and Escape are kept as
/// their control codes; any other control character is dropped.
void note_typed(char32_t code) {
    if (!typing()) return;
    if (code == U'\b' || code == U'\r' || code == U'\x1b') {
        g_typed += static_cast<char>(code);
        return;
    }
    if (code < 0x20 || code == 0x7f || code > 0x10ffff) return;
    if (code < 0x80) {
        g_typed += static_cast<char>(code);
    } else if (code < 0x800) {
        g_typed += static_cast<char>(0xc0 | (code >> 6));
        g_typed += static_cast<char>(0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
        g_typed += static_cast<char>(0xe0 | (code >> 12));
        g_typed += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        g_typed += static_cast<char>(0x80 | (code & 0x3f));
    } else {
        g_typed += static_cast<char>(0xf0 | (code >> 18));
        g_typed += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
        g_typed += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        g_typed += static_cast<char>(0x80 | (code & 0x3f));
    }
}

bool any_key_down() {
    for (int key = 0; key < kKeyCount; ++key) {
        if (key_down(static_cast<Key>(key))) return true;
    }
    return false;
}

// The pad into the game's input, through the console's own filters.
void map_pad(const PadReading& pad, InputState& in, float& x, float& y) {
    std::int16_t lx = pad.lx, ly = pad.ly;
    filter_stick(lx, ly, kLeftStickDeadzone);
    x = static_cast<float>(lx) / kStickMax;
    y = static_cast<float>(ly) / kStickMax;
    in.stick_x = x;
    in.stick_y = y;
    const std::uint16_t b = pad.buttons;
    if (b & kPadLeft) x -= 1.0f, in.dpad_x -= 1.0f;
    if (b & kPadRight) x += 1.0f, in.dpad_x += 1.0f;
    if (b & kPadDown) y -= 1.0f;
    if (b & kPadUp) y += 1.0f;

    std::int16_t rx = pad.rx, ry = pad.ry;
    filter_stick(rx, ry, kRightStickDeadzone);
    in.zoom = static_cast<float>(ry) / kStickMax;
    in.look_x = static_cast<float>(rx) / kStickMax;

    in.confirm = (b & kPadA) != 0;
    in.skip = (b & kPadB) != 0;
    in.attack = (b & kPadX) != 0;
    in.action_y = (b & kPadY) != 0;
    in.quit = (b & kPadStart) != 0;
    in.inventory = (b & kPadBack) != 0;
    // The triggers through the console's own dead range.
    std::uint8_t lt = pad.lt, rt = pad.rt;
    filter_trigger(lt);
    filter_trigger(rt);
    in.menu_prev = lt > 0;
    in.menu_next = rt > 0;
    in.tab_prev = (b & kPadShoulderL) != 0;
    in.tab_next = (b & kPadShoulderR) != 0;
    in.map_toggle = (b & kPadThumbR) != 0;
    in.fps_toggle = (b & kPadThumbL) != 0;
    // The same two D-pad jobs the Switch gives it (platform/switch).
    in.pet_stance = (b & kPadUp) != 0;
    in.pet_town = (b & kPadDown) != 0;
}

// The keyboard, as a pad: WASD or the arrows are the stick, Space or Enter is A, Backspace is B,
// X is X, Y is Y, Escape is START, I or Tab is BACK, Q and E are the shoulders, M is R3 and F is
// L3, Page Up and Page Down are the right stick, O and P are the D-pad's two in-game jobs.
void map_keyboard_movement(InputState& in, float& x, float& y) {
    if (key_down(Key::A) || key_down(Key::Left)) x -= 1.0f, in.stick_x -= 1.0f;
    if (key_down(Key::D) || key_down(Key::Right)) x += 1.0f, in.stick_x += 1.0f;
    if (key_down(Key::S) || key_down(Key::Down)) y -= 1.0f, in.stick_y -= 1.0f;
    if (key_down(Key::W) || key_down(Key::Up)) y += 1.0f, in.stick_y += 1.0f;
    if (key_down(Key::PageUp)) in.zoom = 1.0f;
    if (key_down(Key::PageDown)) in.zoom = -1.0f;
}

void map_keyboard_buttons(InputState& in) {
    in.confirm = in.confirm || key_down(Key::Space) || key_down(Key::Return);
    in.skip = in.skip || key_down(Key::Backspace);
    in.attack = in.attack || key_down(Key::X);
    in.action_y = in.action_y || key_down(Key::Y) || key_down(Key::Delete);
    in.quit = in.quit || key_down(Key::Escape);
    in.inventory = in.inventory || key_down(Key::I) || key_down(Key::Tab);
    in.menu_prev = in.menu_prev || key_down(Key::Q);
    in.menu_next = in.menu_next || key_down(Key::E);
    in.tab_prev = in.tab_prev || key_down(Key::BracketLeft);
    in.tab_next = in.tab_next || key_down(Key::BracketRight);
    in.map_toggle = in.map_toggle || key_down(Key::M);
    in.fps_toggle = in.fps_toggle || key_down(Key::F);
    in.map_zoom_in =
        key_down(Key::Plus) || key_down(Key::KeypadPlus) || key_down(Key::BracketRight);
    in.map_zoom_out =
        key_down(Key::Minus) || key_down(Key::KeypadMinus) || key_down(Key::BracketLeft);
    in.pet_town = in.pet_town || key_down(Key::P);
    if (key_down(Key::C)) in.dpad_x = 1.0f;
    in.pet_stance = in.pet_stance || key_down(Key::O);
}

void map_keyboard(InputState& in, float& x, float& y) {
    map_keyboard_movement(in, x, y);
    map_keyboard_buttons(in);
}

#if defined(_WIN32)

/// GetAsyncKeyState ignores focus. GetActiveWindow() is per-thread and OGRE's window is on this
/// one, so it is non-null exactly when our window has focus.
bool focused() { return GetActiveWindow() != nullptr; }

bool key_down(Key key) {
    static constexpr int kVirtualKeys[] = {
        'A',       'D',         'S',      'W',          VK_LEFT,     VK_RIGHT, VK_DOWN,  VK_UP,
        VK_PRIOR,  VK_NEXT,     VK_SPACE, VK_RETURN,    VK_BACK,     'X',      'Y',      VK_DELETE,
        VK_ESCAPE, 'I',         VK_TAB,   'Q',          'E',         VK_OEM_4, VK_OEM_6, 'M',
        'F',       VK_OEM_PLUS, VK_ADD,   VK_OEM_MINUS, VK_SUBTRACT, 'P',      'C',      'O'};
    return (GetAsyncKeyState(kVirtualKeys[static_cast<int>(key)]) & 0x8000) != 0;
}

/// The first XInput pad that answers, or -1. Polling an empty slot is slow
/// (the driver waits on it), so the search is repeated only every couple of
/// seconds while nothing is attached.
int g_pad_slot = -1;
DWORD g_next_search = 0;

bool read_xinput(XINPUT_STATE& state) {
    if (g_pad_slot >= 0) {
        if (XInputGetState(static_cast<DWORD>(g_pad_slot), &state) == ERROR_SUCCESS) return true;
        g_pad_slot = -1;
    }
    const DWORD now = GetTickCount();
    if (static_cast<int32_t>(now - g_next_search) < 0) return false;
    g_next_search = now + 2000;
    for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        if (XInputGetState(slot, &state) == ERROR_SUCCESS) {
            g_pad_slot = static_cast<int>(slot);
            return true;
        }
    }
    return false;
}

bool read_pad(PadReading& pad) {
    XINPUT_STATE state{};
    if (!read_xinput(state)) return false;
    const XINPUT_GAMEPAD& g = state.Gamepad;
    pad.lx = g.sThumbLX;
    pad.ly = g.sThumbLY;
    pad.rx = g.sThumbRX;
    pad.ry = g.sThumbRY;
    pad.lt = g.bLeftTrigger;
    pad.rt = g.bRightTrigger;
    pad.buttons = g.wButtons;
    return true;
}

/// WM_CHAR arrives through the event pump instead (note_typed_unit).
void read_key_events() {}

#else

// --- Linux: X11 for the keys -----------------------------------------------------------------

Display* x_display() {
    static Display* display = XOpenDisplay(nullptr);
    return display;
}

/// The game's window, once the renderer has made it (set_input_window). OGRE opens its own
/// connection to the display, so the window is another client's to this one.
Window g_window = 0;

/// The game's window has the keyboard: the X input focus is that window, or a window holding it
/// (the frame some window managers focus). The focus and the key states are the whole display's,
/// so without this the game would take keys typed into any other window.
bool focused() {
    Display* display = x_display();
    if (display == nullptr) return false;
    Window focus = 0;
    int revert = 0;
    XGetInputFocus(display, &focus, &revert);
    if (focus == None || focus == PointerRoot) return false;
    // Not made yet (or no window at all): any focus, as before the window was known.
    if (g_window == 0) return true;
    Window window = g_window;
    while (window != 0) {
        if (window == focus) return true;
        Window root = 0, parent = 0;
        Window* children = nullptr;
        unsigned count = 0;
        if (XQueryTree(display, window, &root, &parent, &children, &count) == 0) return false;
        if (children != nullptr) XFree(children);
        if (parent == root) return false;
        window = parent;
    }
    return false;
}

/// The KeyPress events of the game's window since the last call, as typed characters.
void read_key_events() {
    Display* display = x_display();
    if (display == nullptr || g_window == 0) return;
    while (XPending(display) > 0) {
        XEvent event{};
        XNextEvent(display, &event);
        if (event.type != KeyPress) continue;
        char bytes[32] = {};
        KeySym sym = NoSymbol;
        const int count = XLookupString(&event.xkey, bytes, sizeof bytes, &sym, nullptr);
        // XLookupString's text is Latin-1; a keysym past it is Unicode's (0x01000000 + code).
        if (count == 0 && (sym & 0xff000000UL) == 0x01000000UL) {
            note_typed(static_cast<char32_t>(sym & 0x00ffffffUL));
        }
        for (int i = 0; i < count; ++i) note_typed(static_cast<unsigned char>(bytes[i]));
    }
}

bool key_down(Key key) {
    static constexpr KeySym kSyms[] = {
        XK_a,         XK_d,     XK_s,      XK_w,           XK_Left,         XK_Right,
        XK_Down,      XK_Up,    XK_Prior,  XK_Next,        XK_space,        XK_Return,
        XK_BackSpace, XK_x,     XK_y,      XK_Delete,      XK_Escape,       XK_i,
        XK_Tab,       XK_q,     XK_e,      XK_bracketleft, XK_bracketright, XK_m,
        XK_f,         XK_equal, XK_KP_Add, XK_minus,       XK_KP_Subtract,  XK_p,
        XK_c,         XK_o};
    Display* display = x_display();
    if (display == nullptr) return false;
    // One query a frame would do, but the keymap is a single round trip; kept simple.
    char keys[32] = {};
    XQueryKeymap(display, keys);
    const KeyCode code = XKeysymToKeycode(display, kSyms[static_cast<int>(key)]);
    return code != 0 && (keys[code / 8] & (1 << (code % 8))) != 0;
}

// --- Linux: evdev for the pad ----------------------------------------------------------------

struct EvdevPad {
    int fd = -1;
    /// The top and left buttons by place (button_bit).
    bool positional = false;
    input_absinfo axes[ABS_CNT] = {};
    PadReading reading;
    std::chrono::steady_clock::time_point next_search{};
};

EvdevPad& evdev_pad() {
    static EvdevPad pad;
    return pad;
}

bool has_bit(const unsigned long* bits, int bit) {
    constexpr int kWord = static_cast<int>(sizeof(unsigned long) * 8);
    return ((bits[bit / kWord] >> (bit % kWord)) & 1UL) != 0;
}

/// Whether a pad's driver reports its top and left buttons by place, as the kernel's gamepad
/// interface asks (button_bit): Sony's and Nintendo's do.
bool reports_by_place(int fd) {
    input_id id{};
    if (ioctl(fd, EVIOCGID, &id) < 0) return false;
    return id.vendor == 0x054c || id.vendor == 0x057e;
}

/// The first /dev/input/event* that is a gamepad (it has BTN_SOUTH), opened without blocking.
int open_gamepad(input_absinfo* axes) {
    DIR* dir = opendir("/dev/input");
    if (dir == nullptr) return -1;
    int found = -1;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.rfind("event", 0) != 0) continue;
        const int fd = open(("/dev/input/" + name).c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long
            keys[(KEY_CNT + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)] = {};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0 && has_bit(keys, BTN_SOUTH)) {
            for (int axis : {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y}) {
                (void)ioctl(fd, EVIOCGABS(axis), &axes[axis]);
            }
            found = fd;
            break;
        }
        close(fd);
    }
    closedir(dir);
    return found;
}

/// An axis as a signed 16-bit stick value; `flip` for the vertical ones, which evdev counts down.
std::int16_t stick_value(const input_absinfo& info, int value, bool flip) {
    const float span = static_cast<float>(info.maximum - info.minimum);
    if (span <= 0.0f) return 0;
    float unit = (static_cast<float>(value - info.minimum) / span) * 2.0f - 1.0f;
    if (flip) unit = -unit;
    if (unit > 1.0f) unit = 1.0f;
    if (unit < -1.0f) unit = -1.0f;
    return static_cast<std::int16_t>(std::lround(unit * 32767.0f));
}

std::uint8_t trigger_value(const input_absinfo& info, int value) {
    const float span = static_cast<float>(info.maximum - info.minimum);
    if (span <= 0.0f) return 0;
    const float unit = static_cast<float>(value - info.minimum) / span;
    return static_cast<std::uint8_t>(std::lround(std::fmax(0.0f, std::fmin(1.0f, unit)) * 255.0f));
}

/// The XInput bit of an evdev button. BTN_NORTH and BTN_WEST are also spelt BTN_X and BTN_Y,
/// and xpad - every Xbox pad, and the virtual pad Steam gives games - reports them by those
/// labels: X, the left button, as BTN_X (= BTN_NORTH), Y, the top one, as BTN_Y (= BTN_WEST).
/// Taken by place they would swap the two.
std::uint16_t button_bit(int code, bool positional) {
    switch (code) {
        case BTN_SOUTH: return kPadA;
        case BTN_EAST: return kPadB;
        case BTN_NORTH: return positional ? kPadY : kPadX;
        case BTN_WEST: return positional ? kPadX : kPadY;
        case BTN_TL: return kPadShoulderL;
        case BTN_TR: return kPadShoulderR;
        case BTN_SELECT: return kPadBack;
        case BTN_START: return kPadStart;
        case BTN_THUMBL: return kPadThumbL;
        case BTN_THUMBR: return kPadThumbR;
        case BTN_DPAD_UP: return kPadUp;
        case BTN_DPAD_DOWN: return kPadDown;
        case BTN_DPAD_LEFT: return kPadLeft;
        case BTN_DPAD_RIGHT: return kPadRight;
        default: return 0;
    }
}

void apply_event(EvdevPad& pad, const input_event& event) {
    PadReading& r = pad.reading;
    if (event.type == EV_KEY) {
        const std::uint16_t bit = button_bit(event.code, pad.positional);
        r.buttons = event.value != 0 ? (r.buttons | bit) : (r.buttons & ~bit);
        return;
    }
    if (event.type != EV_ABS || event.code >= ABS_CNT) return;
    const input_absinfo& info = pad.axes[event.code];
    switch (event.code) {
        case ABS_X: r.lx = stick_value(info, event.value, false); break;
        case ABS_Y: r.ly = stick_value(info, event.value, true); break;
        case ABS_RX: r.rx = stick_value(info, event.value, false); break;
        case ABS_RY: r.ry = stick_value(info, event.value, true); break;
        case ABS_Z: r.lt = trigger_value(info, event.value); break;
        case ABS_RZ: r.rt = trigger_value(info, event.value); break;
        case ABS_HAT0X:
            r.buttons &= static_cast<std::uint16_t>(~(kPadLeft | kPadRight));
            if (event.value < 0) r.buttons |= kPadLeft;
            if (event.value > 0) r.buttons |= kPadRight;
            break;
        case ABS_HAT0Y:
            r.buttons &= static_cast<std::uint16_t>(~(kPadUp | kPadDown));
            if (event.value < 0) r.buttons |= kPadUp;
            if (event.value > 0) r.buttons |= kPadDown;
            break;
        default: break;
    }
}

/// The pad's state, events read since the last frame folded in. A pad unplugged is let go and
/// looked for again every couple of seconds, as the Windows build looks for its XInput pad.
bool read_pad(PadReading& out) {
    EvdevPad& pad = evdev_pad();
    if (pad.fd < 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now < pad.next_search) return false;
        pad.next_search = now + std::chrono::seconds(2);
        pad.fd = open_gamepad(pad.axes);
        pad.reading = PadReading{};
        if (pad.fd < 0) return false;
        pad.positional = reports_by_place(pad.fd);
    }
    input_event event{};
    ssize_t got = 0;
    while ((got = read(pad.fd, &event, sizeof event)) == static_cast<ssize_t>(sizeof event)) {
        apply_event(pad, event);
    }
    if (got < 0 && errno != EAGAIN) {
        close(pad.fd);
        pad.fd = -1;
        return false;
    }
    out = pad.reading;
    return true;
}

#endif

/// The keys were typing a line and some are still down: the Enter that ended it is not also A.
bool g_keys_typing = false;

}  // namespace

InputState poll_platform_input() {
    InputState in;
    read_key_events();
    const bool has_focus = focused();
    if (!has_focus || !typing()) g_typed.clear();
    if (!has_focus) return in;

    float x = 0.0f, y = 0.0f;
    PadReading pad;
    if (read_pad(pad)) map_pad(pad, in, x, y);
    if (typing()) {
        g_keys_typing = true;
    } else if (g_keys_typing && !any_key_down()) {
        g_keys_typing = false;
    }
    if (!g_keys_typing) map_keyboard(in, x, y);

    // Clamp to the unit disc so a diagonal is not 1.41 times as fast, as the console's round stick.
    const float m = std::sqrt(x * x + y * y);
    if (m > 1.0f) {
        x /= m;
        y /= m;
    }
    in.move_x = x;
    in.move_y = y;
    return in;
}

// No system keyboard on the desktop; a pad script can still answer (`text=` in `--pad-script`).
// Otherwise the line is typed into the window (typing_in_window).
bool text_entry(const std::string& /*title*/, const std::string& /*guide*/, std::string& text,
                unsigned max_chars) {
    std::string scripted;
    if (!take_scripted_text(scripted)) return false;
    if (scripted.size() > max_chars) scripted.resize(max_chars);
    text = scripted;
    return true;
}

bool typing_in_window() { return true; }

std::string take_platform_typed_text() {
    read_key_events();
    std::string typed = std::move(g_typed);
    g_typed.clear();
    return typed;
}

#if defined(_WIN32)

void note_typed_unit(unsigned unit) {
    // A character past the BMP comes as two units, high then low.
    static unsigned high = 0;
    if (unit >= 0xd800 && unit < 0xdc00) {
        high = unit;
        return;
    }
    if (unit >= 0xdc00 && unit < 0xe000) {
        if (high != 0) note_typed(0x10000 + ((high - 0xd800) << 10) + (unit - 0xdc00));
        high = 0;
        return;
    }
    high = 0;
    note_typed(unit);
}

// GetActiveWindow() already tells the game's window from any other.
void set_input_window(std::uintptr_t window) { (void)window; }

#else

void set_input_window(std::uintptr_t window) {
    g_window = static_cast<Window>(window);
    Display* display = x_display();
    if (display == nullptr || g_window == 0) return;
    // Another client's window: KeyPress may be selected by any number of clients.
    XSelectInput(display, g_window, KeyPressMask);
    XFlush(display);
}

#endif

}  // namespace tl::core

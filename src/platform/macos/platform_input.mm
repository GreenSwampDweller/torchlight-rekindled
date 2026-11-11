#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>
#import <Carbon/Carbon.h>

#include "core/gamepad.h"
#include "core/input.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>

bool app_should_keep_running = true;

// What the view hears between frames: the characters typed while a line is being typed, as
// UTF-8 (Return, Delete and Escape as the desktop's \r, \b and \x1b), and the wheel's turn.
static std::string g_mac_typed;
static float g_mac_wheel = 0.0f;

@interface TLGameView : NSView
@end

@implementation TLGameView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent*)event {
    // Swallowed either way, so the system does not beep; the keys themselves are polled.
    if (!tl::core::typing()) return;
    NSString* chars = event.characters;
    for (NSUInteger i = 0; i < chars.length; ++i) {
        const unichar c = [chars characterAtIndex:i];
        if (c == 0x7f || c == NSBackspaceCharacter) {
            g_mac_typed += '\b';
        } else if (c == NSCarriageReturnCharacter || c == NSEnterCharacter) {
            g_mac_typed += '\r';
        } else if (c == 0x1b) {
            g_mac_typed += '\x1b';
        } else if (c >= 0x20 && (c < 0xf700 || c > 0xf8ff)) {
            // Not the function-key range Cocoa gives the arrows and the like.
            NSString* one = [chars substringWithRange:NSMakeRange(i, 1)];
            g_mac_typed += one.UTF8String;
        }
    }
}
- (void)scrollWheel:(NSEvent*)event {
    // A trackpad's precise deltas are points; a wheel's are lines.
    g_mac_wheel += static_cast<float>(event.scrollingDeltaY) *
                   (event.hasPreciseScrollingDeltas ? 0.1f : 1.0f);
}
- (void)mouseDown:(NSEvent*)event { (void)event; }
- (void)rightMouseDown:(NSEvent*)event { (void)event; }
- (void)keyUp:(NSEvent*)event { (void)event; }
- (void)flagsChanged:(NSEvent*)event { (void)event; }
@end

@interface TLWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation TLWindowDelegate
- (BOOL)windowShouldClose:(id)sender {
    (void)sender;
    app_should_keep_running = false;
    return YES;
}
@end

TLWindowDelegate* game_window_delegate = nil;

namespace tl::core {
namespace {

NSWindow* game_window = nil;

void prepare_game_window(std::map<std::string, std::string>& params) {
    // A headless run's window is never seen: no Dock icon, never shown, never brought forward.
    const auto hidden = params.find("hidden");
    const bool headless = hidden != params.end() && hidden->second == "true";
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:headless ? NSApplicationActivationPolicyProhibited
                                        : NSApplicationActivationPolicyRegular];
    // The Dock's icon from the bundle's own, set outright: a bundle replaced in place keeps the
    // icon Launch Services cached for it, which may be none.
    NSImage* icon = [[NSBundle mainBundle] imageForResource:@"torchlight"];
    if (icon != nil && !headless) NSApp.applicationIconImage = icon;
    const NSRect frame = NSMakeRect(0, 0, 1280, 720);
    game_window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:(NSWindowStyleMaskTitled |
                                                         NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskMiniaturizable |
                                                         NSWindowStyleMaskResizable)
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    [game_window setTitle:@"Torchlight Rekindled"];
    [game_window center];
    game_window_delegate = [[TLWindowDelegate alloc] init];
    [game_window setDelegate:game_window_delegate];
    NSView* content = [[TLGameView alloc] initWithFrame:frame];
    [game_window setContentView:content];
    if (!headless) {
        [game_window makeKeyAndOrderFront:nil];
        [game_window makeFirstResponder:content];
        [NSApp activateIgnoringOtherApps:YES];
    }
    params["externalWindowHandle"] = std::to_string(reinterpret_cast<std::uintptr_t>(content));
}

constexpr std::uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;
constexpr std::uint16_t kStart = 0x0010, kBack = 0x0020, kL3 = 0x0040, kR3 = 0x0080;
constexpr std::uint16_t kLB = 0x0100, kRB = 0x0200;
constexpr std::uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008;

enum class Key {
    A, D, S, W, Left, Right, Down, Up, PageUp, PageDown, Space, Return, Backspace, X, Y,
    Delete, Escape, I, Tab, Q, E, BracketLeft, BracketRight, M, F, Plus, KeypadPlus,
    Minus, KeypadMinus, P, C, O, K, One, Two, Three, Four
};

constexpr CGKeyCode kKeys[] = {
    kVK_ANSI_A, kVK_ANSI_D, kVK_ANSI_S, kVK_ANSI_W, kVK_LeftArrow, kVK_RightArrow,
    kVK_DownArrow, kVK_UpArrow, kVK_PageUp, kVK_PageDown, kVK_Space, kVK_Return,
    kVK_Delete, kVK_ANSI_X, kVK_ANSI_Y, kVK_ForwardDelete, kVK_Escape, kVK_ANSI_I,
    kVK_Tab, kVK_ANSI_Q, kVK_ANSI_E, kVK_ANSI_LeftBracket, kVK_ANSI_RightBracket,
    kVK_ANSI_M, kVK_ANSI_F, kVK_ANSI_Equal, kVK_ANSI_KeypadPlus, kVK_ANSI_Minus,
    kVK_ANSI_KeypadMinus, kVK_ANSI_P, kVK_ANSI_C, kVK_ANSI_O, kVK_ANSI_K, kVK_ANSI_1, kVK_ANSI_2,
    kVK_ANSI_3, kVK_ANSI_4};

bool focused() { return NSApp != nil && NSApp.isActive && NSApp.keyWindow != nil; }

bool key_down(Key key) {
    return CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState,
                                 kKeys[static_cast<size_t>(key)]);
}

std::int16_t axis(float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    return static_cast<std::int16_t>(std::lround(v * 32767.0f));
}

struct PadReading {
    std::int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    std::uint8_t lt = 0, rt = 0;
    std::uint16_t buttons = 0;
};

void set_button(std::uint16_t& buttons, bool down, std::uint16_t bit) {
    if (down) buttons |= bit;
}

bool read_pad(PadReading& out) {
    for (GCController* controller in GCController.controllers) {
        GCExtendedGamepad* g = controller.extendedGamepad;
        if (g == nil) continue;
        out.lx = axis(g.leftThumbstick.xAxis.value);
        out.ly = axis(g.leftThumbstick.yAxis.value);
        out.rx = axis(g.rightThumbstick.xAxis.value);
        out.ry = axis(g.rightThumbstick.yAxis.value);
        out.lt = static_cast<std::uint8_t>(std::lround(g.leftTrigger.value * 255.0f));
        out.rt = static_cast<std::uint8_t>(std::lround(g.rightTrigger.value * 255.0f));
        set_button(out.buttons, g.buttonA.isPressed, kA);
        set_button(out.buttons, g.buttonB.isPressed, kB);
        set_button(out.buttons, g.buttonX.isPressed, kX);
        set_button(out.buttons, g.buttonY.isPressed, kY);
        set_button(out.buttons, g.leftShoulder.isPressed, kLB);
        set_button(out.buttons, g.rightShoulder.isPressed, kRB);
        set_button(out.buttons, g.leftThumbstickButton.isPressed, kL3);
        set_button(out.buttons, g.rightThumbstickButton.isPressed, kR3);
        set_button(out.buttons, g.dpad.up.isPressed, kUp);
        set_button(out.buttons, g.dpad.down.isPressed, kDown);
        set_button(out.buttons, g.dpad.left.isPressed, kLeft);
        set_button(out.buttons, g.dpad.right.isPressed, kRight);
        if (@available(macOS 11.0, *)) {
            set_button(out.buttons, g.buttonMenu.isPressed, kStart);
            set_button(out.buttons, g.buttonOptions.isPressed, kBack);
        }
        return true;
    }
    return false;
}

void map_pad(const PadReading& p, InputState& in, float& x, float& y) {
    auto lx = p.lx, ly = p.ly;
    filter_stick(lx, ly, kLeftStickDeadzone);
    x = static_cast<float>(lx) / kStickMax;
    y = static_cast<float>(ly) / kStickMax;
    in.stick_x = x; in.stick_y = y;
    if (p.buttons & kLeft) x -= 1.0f, in.dpad_x -= 1.0f;
    if (p.buttons & kRight) x += 1.0f, in.dpad_x += 1.0f;
    if (p.buttons & kDown) y -= 1.0f;
    if (p.buttons & kUp) y += 1.0f;
    auto rx = p.rx, ry = p.ry;
    filter_stick(rx, ry, kRightStickDeadzone);
    in.zoom = static_cast<float>(ry) / kStickMax;
    in.look_x = static_cast<float>(rx) / kStickMax;
    in.confirm = (p.buttons & kA) != 0; in.skip = (p.buttons & kB) != 0;
    in.attack = (p.buttons & kX) != 0; in.action_y = (p.buttons & kY) != 0;
    in.quit = (p.buttons & kStart) != 0; in.inventory = (p.buttons & kBack) != 0;
    auto lt = p.lt, rt = p.rt;
    filter_trigger(lt); filter_trigger(rt);
    in.menu_prev = lt > 0; in.menu_next = rt > 0;
    in.tab_prev = (p.buttons & kLB) != 0; in.tab_next = (p.buttons & kRB) != 0;
    in.map_toggle = (p.buttons & kR3) != 0; in.fps_toggle = (p.buttons & kL3) != 0;
    in.pet_stance = (p.buttons & kUp) != 0; in.pet_town = (p.buttons & kDown) != 0;
}

void map_keyboard(InputState& in, float& x, float& y) {
    if (key_down(Key::A) || key_down(Key::Left)) x -= 1.0f, in.stick_x -= 1.0f;
    if (key_down(Key::D) || key_down(Key::Right)) x += 1.0f, in.stick_x += 1.0f;
    if (key_down(Key::S) || key_down(Key::Down)) y -= 1.0f, in.stick_y -= 1.0f;
    if (key_down(Key::W) || key_down(Key::Up)) y += 1.0f, in.stick_y += 1.0f;
    if (key_down(Key::PageUp)) in.zoom = 1.0f;
    if (key_down(Key::PageDown)) in.zoom = -1.0f;
    in.confirm |= key_down(Key::Space) || key_down(Key::Return);
    in.skip |= key_down(Key::Backspace); in.attack |= key_down(Key::X);
    in.action_y |= key_down(Key::Y) || key_down(Key::Delete);
    in.quit |= key_down(Key::Escape); in.inventory |= key_down(Key::I) || key_down(Key::Tab);
    in.menu_prev |= key_down(Key::Q); in.menu_next |= key_down(Key::E);
    in.tab_prev |= key_down(Key::BracketLeft); in.tab_next |= key_down(Key::BracketRight);
    in.map_toggle |= key_down(Key::M); in.fps_toggle |= key_down(Key::F);
    in.map_zoom_in = key_down(Key::Plus) || key_down(Key::KeypadPlus) || key_down(Key::BracketRight);
    in.map_zoom_out = key_down(Key::Minus) || key_down(Key::KeypadMinus) || key_down(Key::BracketLeft);
    in.pet_town |= key_down(Key::P); if (key_down(Key::C)) in.dpad_x = 1.0f;
    in.pet_stance |= key_down(Key::O); in.skills |= key_down(Key::K);
    in.hotbar_key[0] |= key_down(Key::One); in.hotbar_key[1] |= key_down(Key::Two);
    in.hotbar_key[2] |= key_down(Key::Three); in.hotbar_key[3] |= key_down(Key::Four);
}
}  // namespace

void platform_window_params(std::map<std::string, std::string>& params) {
    prepare_game_window(params);
}

void pump_events() {
    @autoreleasepool {
        NSEvent* event = nil;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:[NSDate distantPast]
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES])) {
            [NSApp sendEvent:event];
        }
    }
}

extern "C" bool tl_macos_should_keep_running() { return app_should_keep_running; }

extern "C" void tl_macos_error_message(const char* summary, const char* detail) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = [NSString stringWithUTF8String:summary != nullptr ? summary : "Error"];
        alert.informativeText = [NSString stringWithUTF8String:detail != nullptr ? detail : ""];
        [alert addButtonWithTitle:@"OK"];
        [alert runModal];
    }
}

/// The keys were typing a line and some are still down: the Return that ended it is not also A.
static bool g_keys_typing = false;

/// The wheel as the right stick's zoom: each notch holds it over a few frames, as a pushed
/// stick would (`zoom_dolly` is per second).
static int g_zoom_frames = 0;
static float g_zoom_sign = 0.0f;

static void read_mouse(InputState& in) {
    if (g_mac_wheel != 0.0f) {
        g_zoom_sign = g_mac_wheel > 0.0f ? -1.0f : 1.0f;
        g_zoom_frames = std::min(30, g_zoom_frames + static_cast<int>(std::ceil(std::fabs(g_mac_wheel))) * 4);
        g_mac_wheel = 0.0f;
    }
    if (g_zoom_frames > 0) {
        --g_zoom_frames;
        if (in.zoom == 0.0f) in.zoom = g_zoom_sign;
    }
    if (game_window == nil) return;
    NSView* view = game_window.contentView;
    const NSPoint at = [view convertPoint:[game_window mouseLocationOutsideOfEventStream]
                                 fromView:nil];
    const NSRect bounds = view.bounds;
    if (bounds.size.width <= 0 || bounds.size.height <= 0 || !NSPointInRect(at, bounds)) return;
    in.pointer = true;
    in.pointer_x = static_cast<float>(at.x / bounds.size.width);
    // Cocoa counts up from the bottom; the game from the top.
    in.pointer_y = static_cast<float>(1.0 - at.y / bounds.size.height);
    const NSUInteger buttons = [NSEvent pressedMouseButtons];
    // A Control-click is the Mac's right click.
    const bool control = ([NSEvent modifierFlags] & NSEventModifierFlagControl) != 0;
    in.pointer_left = (buttons & 1) != 0 && !control;
    in.pointer_right = (buttons & 2) != 0 || ((buttons & 1) != 0 && control);
}

InputState poll_platform_input() {
    InputState in;
    if (!focused() || !typing()) g_mac_typed.clear();
    if (!focused()) return in;
    float x = 0.0f, y = 0.0f;
    PadReading p;
    if (read_pad(p)) map_pad(p, in, x, y);
    bool any_down = false;
    for (int key = 0; key <= static_cast<int>(Key::Four); ++key) {
        any_down = any_down || key_down(static_cast<Key>(key));
    }
    if (typing()) {
        g_keys_typing = true;
    } else if (g_keys_typing && !any_down) {
        g_keys_typing = false;
    }
    if (!g_keys_typing) map_keyboard(in, x, y);
    if (!typing()) read_mouse(in);
    const float mag = std::sqrt(x * x + y * y);
    if (mag > 1.0f) x /= mag, y /= mag;
    in.move_x = x; in.move_y = y;
    return in;
}

// No system keyboard; a pad script can still answer, else the line is typed into the window.
bool text_entry(const std::string&, const std::string&, std::string& text, unsigned max_chars) {
    std::string scripted;
    if (!take_scripted_text(scripted)) return false;
    if (scripted.size() > max_chars) scripted.resize(max_chars);
    text = scripted;
    return true;
}

bool typing_in_window() { return true; }

std::string take_platform_typed_text() {
    std::string typed = std::move(g_mac_typed);
    g_mac_typed.clear();
    return typed;
}

// The view hears its own keys (TLGameView); nothing to tell it.
void set_input_window(std::uintptr_t window) { (void)window; }
}  // namespace tl::core

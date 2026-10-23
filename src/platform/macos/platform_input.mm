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

@interface TLGameView : NSView
@end

@implementation TLGameView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent*)event { (void)event; }
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
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
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
    [game_window makeKeyAndOrderFront:nil];
    [game_window makeFirstResponder:content];
    [NSApp activateIgnoringOtherApps:YES];
    params["externalWindowHandle"] = std::to_string(reinterpret_cast<std::uintptr_t>(content));
}

constexpr std::uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;
constexpr std::uint16_t kStart = 0x0010, kBack = 0x0020, kL3 = 0x0040, kR3 = 0x0080;
constexpr std::uint16_t kLB = 0x0100, kRB = 0x0200;
constexpr std::uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008;

enum class Key {
    A, D, S, W, Left, Right, Down, Up, PageUp, PageDown, Space, Return, Backspace, X, Y,
    Delete, Escape, I, Tab, Q, E, BracketLeft, BracketRight, M, F, Plus, KeypadPlus,
    Minus, KeypadMinus, P, C, O
};

constexpr CGKeyCode kKeys[] = {
    kVK_ANSI_A, kVK_ANSI_D, kVK_ANSI_S, kVK_ANSI_W, kVK_LeftArrow, kVK_RightArrow,
    kVK_DownArrow, kVK_UpArrow, kVK_PageUp, kVK_PageDown, kVK_Space, kVK_Return,
    kVK_Delete, kVK_ANSI_X, kVK_ANSI_Y, kVK_ForwardDelete, kVK_Escape, kVK_ANSI_I,
    kVK_Tab, kVK_ANSI_Q, kVK_ANSI_E, kVK_ANSI_LeftBracket, kVK_ANSI_RightBracket,
    kVK_ANSI_M, kVK_ANSI_F, kVK_ANSI_Equal, kVK_ANSI_KeypadPlus, kVK_ANSI_Minus,
    kVK_ANSI_KeypadMinus, kVK_ANSI_P, kVK_ANSI_C, kVK_ANSI_O};

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
    in.pet_stance |= key_down(Key::O);
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

InputState poll_platform_input() {
    InputState in;
    if (!focused()) return in;
    float x = 0.0f, y = 0.0f;
    PadReading p;
    if (read_pad(p)) map_pad(p, in, x, y);
    map_keyboard(in, x, y);
    const float mag = std::sqrt(x * x + y * y);
    if (mag > 1.0f) x /= mag, y /= mag;
    in.move_x = x; in.move_y = y;
    return in;
}

bool text_entry(const std::string&, const std::string&, std::string&, unsigned) { return false; }
}  // namespace tl::core

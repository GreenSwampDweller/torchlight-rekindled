#import <UIKit/UIKit.h>
#import <GameController/GCController.h>
#import <GameController/GCControllerButtonInput.h>
#import <GameController/GCControllerAxisInput.h>
#import <GameController/GCControllerDirectionPad.h>
#import <GameController/GCExtendedGamepad.h>
#import "OgreEAGL2View.h"
#import "OgreEAGL2ViewController.h"
#import <atomic>
#import <cstdlib>
#import <thread>
#import <string>
#import <vector>
#import <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include "core/touch_pad.h"
#include "platform/ios/ios_app.h"

extern int tl_engine_main(int argc, char** argv);
static std::atomic<bool> g_active{true};
static std::atomic<bool> g_engine_started{false};
static UIWindow* g_render_window = nil;
static std::string g_program_folder;

struct IOSControllerState {
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
    uint32_t buttons = 0;
    bool connected = false;
};
static std::mutex g_controller_mutex;
static IOSControllerState g_controller_state;

static void publishController(GCExtendedGamepad* gamepad) {
    if (gamepad == nil) return;
    uint32_t buttons = 0;
    using tl::core::TouchPad;
    if (gamepad.buttonA.isPressed) buttons |= TouchPad::kA;
    if (gamepad.buttonB.isPressed) buttons |= TouchPad::kB;
    if (gamepad.buttonX.isPressed) buttons |= TouchPad::kX;
    if (gamepad.buttonY.isPressed) buttons |= TouchPad::kY;
    if (gamepad.leftShoulder.isPressed) buttons |= TouchPad::kL1;
    if (gamepad.rightShoulder.isPressed) buttons |= TouchPad::kR1;
    if (gamepad.leftTrigger.value >= 0.5f) buttons |= TouchPad::kL2;
    if (gamepad.rightTrigger.value >= 0.5f) buttons |= TouchPad::kR2;
    if (gamepad.leftThumbstickButton.isPressed) buttons |= TouchPad::kThumbL;
    if (gamepad.rightThumbstickButton.isPressed) buttons |= TouchPad::kThumbR;
    if (gamepad.buttonMenu.isPressed) buttons |= TouchPad::kStart;
    if (gamepad.buttonOptions.isPressed) buttons |= TouchPad::kSelect;
    if (gamepad.dpad.up.isPressed) buttons |= TouchPad::kUp;
    if (gamepad.dpad.down.isPressed) buttons |= TouchPad::kDown;
    if (gamepad.dpad.left.isPressed) buttons |= TouchPad::kLeft;
    if (gamepad.dpad.right.isPressed) buttons |= TouchPad::kRight;
    std::lock_guard<std::mutex> lock(g_controller_mutex);
    g_controller_state = {gamepad.leftThumbstick.xAxis.value,
                          gamepad.leftThumbstick.yAxis.value,
                          gamepad.rightThumbstick.xAxis.value,
                          gamepad.rightThumbstick.yAxis.value, buttons, true};
}

extern "C" void tl_ios_read_controller(float* left_x, float* left_y, float* right_x,
                                          float* right_y, uint32_t* buttons, bool* connected) {
    std::lock_guard<std::mutex> lock(g_controller_mutex);
    if (left_x) *left_x = g_controller_state.lx;
    if (left_y) *left_y = g_controller_state.ly;
    if (right_x) *right_x = g_controller_state.rx;
    if (right_y) *right_y = g_controller_state.ry;
    if (buttons) *buttons = g_controller_state.buttons;
    if (connected) *connected = g_controller_state.connected;
}

extern "C" const char* tl_ios_program_folder(void) {
    @autoreleasepool {
        if (g_program_folder.empty()) {
            NSString* path = NSBundle.mainBundle.resourcePath;
            g_program_folder = path != nil ? std::string(path.fileSystemRepresentation) + "/" : "";
        }
        return g_program_folder.c_str();
    }
}

static void copyBundledGameData(const std::string& documents, const std::string& bundled) {
    @autoreleasepool {
        NSFileManager* files = [NSFileManager defaultManager];
        NSString* sourceRoot = [NSString stringWithUTF8String:bundled.c_str()];
        NSString* destinationRoot = [NSString stringWithUTF8String:documents.c_str()];
        for (NSString* name in @[@"pak.zip", @"music", @"console_ui"]) {
            NSString* source = [sourceRoot stringByAppendingPathComponent:name];
            NSString* destination = [destinationRoot stringByAppendingPathComponent:name];
            if (![files fileExistsAtPath:source] || [files fileExistsAtPath:destination])
                continue;
            NSError* error = nil;
            if (![files copyItemAtPath:source toPath:destination error:&error]) {
                NSLog(@"Torchlight: could not copy bundled %@ into Documents: %@", name, error);
            }
        }
    }
}

static NSString* findModRootWithMedia(NSString* start) {
    NSFileManager* files = [NSFileManager defaultManager];
    NSMutableArray<NSDictionary<NSString*, id>*>* pending = [NSMutableArray arrayWithObject:@{
        @"path": start, @"depth": @0
    }];
    while (pending.count > 0) {
        NSDictionary<NSString*, id>* entry = pending.firstObject;
        [pending removeObjectAtIndex:0];
        NSString* directory = entry[@"path"];
        NSUInteger depth = [entry[@"depth"] unsignedIntegerValue];
        NSError* error = nil;
        NSArray<NSString*>* names = [files contentsOfDirectoryAtPath:directory error:&error];
        if (names == nil) continue;

        for (NSString* name in names) {
            if ([name caseInsensitiveCompare:@"media"] != NSOrderedSame) continue;
            NSString* media = [directory stringByAppendingPathComponent:name];
            BOOL isDirectory = NO;
            if ([files fileExistsAtPath:media isDirectory:&isDirectory] && isDirectory)
                return directory;
        }

        // Mod archives are often wrapped in one or more named folders (for
        // example, mods/Baklahs Unique gear/Baklah_Unique/media). Find the
        // folder immediately above MEDIA instead of assuming the ZIP was
        // extracted directly into mods/<name>/.
        if (depth >= 6) continue;
        for (NSString* name in names) {
            if ([name hasPrefix:@"."] || [name caseInsensitiveCompare:@"__MACOSX"] == NSOrderedSame)
                continue;
            NSString* child = [directory stringByAppendingPathComponent:name];
            BOOL isDirectory = NO;
            if ([files fileExistsAtPath:child isDirectory:&isDirectory] && isDirectory) {
                [pending addObject:@{@"path": child, @"depth": @(depth + 1)}];
            }
        }
    }
    return nil;
}

static std::vector<std::string> discoverModMediaFolders(const std::string& documents) {
    @autoreleasepool {
        NSFileManager* files = [NSFileManager defaultManager];
        NSString* documentsRoot = [NSString stringWithUTF8String:documents.c_str()];
        NSString* modsRoot = [documentsRoot stringByAppendingPathComponent:@"mods"];
        NSError* error = nil;
        [files createDirectoryAtPath:modsRoot withIntermediateDirectories:YES attributes:nil error:&error];
        if (error != nil) {
            NSLog(@"Torchlight: could not create mods folder: %@", error);
            return {};
        }

        NSArray<NSString*>* names = [files contentsOfDirectoryAtPath:modsRoot error:&error];
        if (names == nil) {
            NSLog(@"Torchlight: could not list mods folder: %@", error);
            return {};
        }

        std::vector<std::string> mediaFolders;
        for (NSString* name in [names sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)]) {
            NSString* modRoot = [modsRoot stringByAppendingPathComponent:name];
            BOOL isDirectory = NO;
            if (![files fileExistsAtPath:modRoot isDirectory:&isDirectory] || !isDirectory) continue;
            NSString* mediaRoot = findModRootWithMedia(modRoot);
            if (mediaRoot == nil) continue;
            NSLog(@"Torchlight: found mod folder %@ at %@", name, mediaRoot);
            mediaFolders.emplace_back(mediaRoot.fileSystemRepresentation);
        }
        if (mediaFolders.empty()) NSLog(@"Torchlight: no extracted mod folders found in %@", modsRoot);
        return mediaFolders;
    }
}

@interface TorchlightAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@interface TorchlightSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@interface TorchlightRenderView : EAGL2View
@end

@interface TorchlightViewController : EAGL2ViewController
@end

@implementation TorchlightViewController
- (BOOL)shouldAutorotate { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}
@end

@implementation TorchlightRenderView
- (void)layoutSubviews {
    // OGRE's EAGL2View implementation now guards UIKit's early layout calls.
    [super layoutSubviews];
}
- (void)forwardTouches:(NSSet<UITouch*>*)touches phase:(NSInteger)phase {
    auto& pad = tl::core::touch_pad();
    const CGFloat scale = self.contentScaleFactor;
    for (UITouch* touch in touches) {
        const CGPoint point = [touch locationInView:self];
        const int id = static_cast<int>(touch.hash & 0x7fffffff);
        if (phase == 0) pad.touch_down(id, point.x * scale, point.y * scale);
        else if (phase == 1) pad.touch_move(id, point.x * scale, point.y * scale);
        else pad.touch_up(id);
    }
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { (void)event; [self forwardTouches:touches phase:0]; }
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { (void)event; [self forwardTouches:touches phase:1]; }
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { (void)event; [self forwardTouches:touches phase:2]; }
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { (void)event; [self forwardTouches:touches phase:2]; }
@end

@implementation TorchlightSceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:[UIWindowScene class]]) return;
    UIWindowScene* windowScene = (UIWindowScene*)scene;
    self.window = [[UIWindow alloc] initWithWindowScene:windowScene];
    g_render_window = self.window;
    EAGL2ViewController* controller = [TorchlightViewController new];
    EAGL2View* renderView = [[TorchlightRenderView alloc] initWithFrame:self.window.bounds];
    renderView.contentScaleFactor = windowScene.screen.nativeScale;
    renderView.multipleTouchEnabled = YES;
    renderView.opaque = YES;
    controller.view = renderView;
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];
    if (!g_engine_started.exchange(true)) {
        NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        const CGFloat scale = windowScene.screen.nativeScale;
        const int width = (int)(self.window.bounds.size.width * scale);
        const int height = (int)(self.window.bounds.size.height * scale);
        tl::core::touch_pad().set_screen((float)width, (float)height);
        std::string root = std::string(docs.UTF8String) + "/";
        std::string bundledGameData =
            std::string(NSBundle.mainBundle.resourcePath.UTF8String) + "/GameData";
        std::thread([root, bundledGameData, width, height]() mutable {
            copyBundledGameData(root, bundledGameData);
            const std::vector<std::string> modMediaFolders = discoverModMediaFolders(root);
            std::string media = std::string(NSBundle.mainBundle.resourcePath.UTF8String);
            std::vector<std::string> arguments = {
                "torchlight", "--game-data", root, "--width", std::to_string(width),
                "--height", std::to_string(height), "--ogre-media", media};
            for (const std::string& modMedia : modMediaFolders) {
                arguments.emplace_back("--mod-data");
                arguments.emplace_back(modMedia);
            }
            std::vector<char*> argv;
            argv.reserve(arguments.size());
            for (std::string& argument : arguments) argv.push_back(argument.data());
            const int result = tl_engine_main(static_cast<int>(argv.size()), argv.data());
            // UIKit's event loop outlives the engine thread. Once the engine
            // has saved and shut down, terminate the process so Quit does not
            // leave a live, black app window behind.
            std::exit(result);
        }).detach();
    }
}
@end

@implementation TorchlightAppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    (void)application; (void)options;
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    [center addObserver:self selector:@selector(controllerConnected:)
                   name:GCControllerDidConnectNotification object:nil];
    [center addObserver:self selector:@selector(controllerDisconnected:)
                   name:GCControllerDidDisconnectNotification object:nil];
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
    for (GCController* controller in GCController.controllers) [self attachController:controller];
    return YES;
}
- (void)attachController:(GCController*)controller {
    GCExtendedGamepad* gamepad = controller.extendedGamepad;
    if (gamepad == nil) return;
    gamepad.valueChangedHandler = ^(GCExtendedGamepad* changed, GCControllerElement* element) {
        (void)element;
        publishController(changed);
    };
    publishController(gamepad);
    tl::core::touch_pad().set_visible(false);
    tl::core::touch_pad().release_all();
}
- (void)controllerConnected:(NSNotification*)notification {
    GCController* controller = (GCController*)notification.object;
    [self attachController:controller];
}
- (void)controllerDisconnected:(NSNotification*)notification {
    (void)notification;
    std::lock_guard<std::mutex> lock(g_controller_mutex);
    g_controller_state = IOSControllerState{};
    tl::core::touch_pad().set_visible(false);
    tl::core::touch_pad().release_all();
}
- (UISceneConfiguration*)application:(UIApplication*)application configurationForConnectingSceneSession:(UISceneSession*)connectingSceneSession options:(UISceneConnectionOptions*)options {
    (void)application; (void)connectingSceneSession; (void)options;
    UISceneConfiguration* config = [[UISceneConfiguration alloc] initWithName:@"Torchlight" sessionRole:UIWindowSceneSessionRoleApplication];
    config.delegateClass = TorchlightSceneDelegate.class;
    return config;
}
- (void)applicationWillResignActive:(UIApplication*)application { (void)application; g_active.store(false); }
- (void)applicationDidBecomeActive:(UIApplication*)application { (void)application; g_active.store(true); }
- (void)applicationWillTerminate:(UIApplication*)application { (void)application; g_active.store(false); }
@end

extern "C" void* tl_ios_view_handle(void) {
    UIWindow* window = g_render_window;
    return (__bridge void*)window.rootViewController.view;
}
extern "C" void* tl_ios_controller_handle(void) {
    UIWindow* window = g_render_window;
    return (__bridge void*)window.rootViewController;
}
extern "C" bool tl_ios_app_active(void) { return g_active.load(); }

extern "C" bool tl_ios_text_entry(const char* title, const char* guide, const char* initial,
                                  unsigned max_chars, char* output, size_t output_size) {
    struct TextEntryResult {
        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
        std::string value;
        bool accepted = false;
    };
    auto result = std::make_shared<TextEntryResult>();
    result->value = initial != nullptr ? initial : "";
    dispatch_async(dispatch_get_main_queue(), ^{
        UIWindow* window = g_render_window;
        UIViewController* presenter = window.rootViewController;
        if (presenter == nil) {
            dispatch_semaphore_signal(result->semaphore);
            return;
        }
        NSString* titleText = [NSString stringWithUTF8String:title != nullptr ? title : ""];
        NSString* guideText = [NSString stringWithUTF8String:guide != nullptr ? guide : ""];
        UIAlertController* alert = [UIAlertController alertControllerWithTitle:titleText
                                                                         message:guideText
                                                                  preferredStyle:UIAlertControllerStyleAlert];
        __block UITextField* nameField = nil;
        [alert addTextFieldWithConfigurationHandler:^(UITextField* field) {
            nameField = field;
            field.text = [NSString stringWithUTF8String:result->value.c_str()];
            field.autocorrectionType = UITextAutocorrectionTypeNo;
            field.autocapitalizationType = UITextAutocapitalizationTypeWords;
            field.clearButtonMode = UITextFieldViewModeWhileEditing;
        }];
        [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                                  style:UIAlertActionStyleCancel
                                                handler:^(__unused UIAlertAction* action) {
            dispatch_semaphore_signal(result->semaphore);
        }]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Done"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction* action) {
            NSString* value = nameField.text != nil ? nameField.text : @"";
            if (value.length > max_chars) value = [value substringToIndex:max_chars];
            const char* utf8 = value.UTF8String;
            result->value = utf8 != nullptr ? utf8 : "";
            result->accepted = true;
            dispatch_semaphore_signal(result->semaphore);
        }]];
        [presenter presentViewController:alert animated:YES completion:^{
            [alert.textFields.firstObject becomeFirstResponder];
        }];
    });
    dispatch_semaphore_wait(result->semaphore, DISPATCH_TIME_FOREVER);
    if (!result->accepted || output == nullptr || output_size == 0) return false;
    const size_t count = std::min(output_size - 1, result->value.size());
    std::memcpy(output, result->value.data(), count);
    output[count] = '\0';
    return true;
}

int main(int argc, char* argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(TorchlightAppDelegate.class)); }
}

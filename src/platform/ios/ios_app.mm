#import <UIKit/UIKit.h>
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
#include "core/touch_pad.h"
#include "platform/ios/ios_app.h"

extern int tl_engine_main(int argc, char** argv);
static std::atomic<bool> g_active{true};
static std::atomic<bool> g_engine_started{false};
static UIWindow* g_render_window = nil;
static std::string g_program_folder;

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
            std::string media = std::string(NSBundle.mainBundle.resourcePath.UTF8String);
            std::vector<std::string> arguments = {
                "torchlight", "--game-data", root, "--width", std::to_string(width),
                "--height", std::to_string(height), "--ogre-media", media};
            // Mods are the Mods page's (core/mods.cpp), read from Documents/mods/.
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
    return YES;
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

#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>

#include "imgui.h"
#include "sr_window.h"

#include <cfloat>
#include <string>
#include <vector>

struct SrWindow {
    NSWindow* window;
    NSView* view;
    bool closed;
    float scale;
    std::vector<uint32_t> frame;
    int frame_w, frame_h;
    std::string drop;
};

static SrWindow* g_window;

static ImGuiKey mac_key(unsigned short code) {
    switch (code) {
        case kVK_Return: return ImGuiKey_Enter;
        case kVK_ANSI_KeypadEnter: return ImGuiKey_KeypadEnter;
        case kVK_Tab: return ImGuiKey_Tab;
        case kVK_Space: return ImGuiKey_Space;
        case kVK_Delete: return ImGuiKey_Backspace;
        case kVK_ForwardDelete: return ImGuiKey_Delete;
        case kVK_Escape: return ImGuiKey_Escape;
        case kVK_LeftArrow: return ImGuiKey_LeftArrow;
        case kVK_RightArrow: return ImGuiKey_RightArrow;
        case kVK_UpArrow: return ImGuiKey_UpArrow;
        case kVK_DownArrow: return ImGuiKey_DownArrow;
        case kVK_Home: return ImGuiKey_Home;
        case kVK_End: return ImGuiKey_End;
        case kVK_PageUp: return ImGuiKey_PageUp;
        case kVK_PageDown: return ImGuiKey_PageDown;
        case kVK_ANSI_A: return ImGuiKey_A;
        case kVK_ANSI_C: return ImGuiKey_C;
        case kVK_ANSI_V: return ImGuiKey_V;
        case kVK_ANSI_X: return ImGuiKey_X;
        case kVK_ANSI_Z: return ImGuiKey_Z;
        default: return ImGuiKey_None;
    }
}

static void mac_modifiers(NSEventModifierFlags flags) {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Shift, (flags & NSEventModifierFlagShift) != 0);
    io.AddKeyEvent(ImGuiMod_Ctrl, (flags & NSEventModifierFlagControl) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (flags & NSEventModifierFlagOption) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (flags & NSEventModifierFlagCommand) != 0);
}

@interface SrView : NSView
@end

@implementation SrView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }

- (void)drawRect:(NSRect)dirty {
    SrWindow* w = g_window;
    if (!w || w->frame.empty()) return;
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef data = CGDataProviderCreateWithData(nullptr, w->frame.data(), w->frame.size() * 4, nullptr);
    CGImageRef image = CGImageCreate((size_t)w->frame_w, (size_t)w->frame_h, 8, 32, (size_t)w->frame_w * 4, space,
                                     kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, data, nullptr, false,
                                     kCGRenderingIntentDefault);
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    NSRect b = [self bounds];
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, b.size.height);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextDrawImage(ctx, CGRectMake(0, 0, b.size.width, b.size.height), image);
    CGContextRestoreGState(ctx);
    CGImageRelease(image);
    CGDataProviderRelease(data);
    CGColorSpaceRelease(space);
}

- (void)mousePos:(NSEvent*)event {
    if (!ImGui::GetCurrentContext()) return;
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    float s = g_window ? g_window->scale : 1.0f;
    ImGui::GetIO().AddMousePosEvent((float)p.x * s, (float)p.y * s);
}
- (void)mouseButton:(NSEvent*)event button:(int)button down:(bool)down {
    if (!ImGui::GetCurrentContext()) return;
    mac_modifiers([event modifierFlags]);
    [self mousePos:event];
    ImGui::GetIO().AddMouseButtonEvent(button, down);
}
- (void)mouseMoved:(NSEvent*)e { [self mousePos:e]; }
- (void)mouseDragged:(NSEvent*)e { [self mousePos:e]; }
- (void)rightMouseDragged:(NSEvent*)e { [self mousePos:e]; }
- (void)otherMouseDragged:(NSEvent*)e { [self mousePos:e]; }
- (void)mouseDown:(NSEvent*)e { [self mouseButton:e button:0 down:true]; }
- (void)mouseUp:(NSEvent*)e { [self mouseButton:e button:0 down:false]; }
- (void)rightMouseDown:(NSEvent*)e { [self mouseButton:e button:1 down:true]; }
- (void)rightMouseUp:(NSEvent*)e { [self mouseButton:e button:1 down:false]; }
- (void)otherMouseDown:(NSEvent*)e { [self mouseButton:e button:2 down:true]; }
- (void)otherMouseUp:(NSEvent*)e { [self mouseButton:e button:2 down:false]; }
- (void)mouseExited:(NSEvent*)e {
    if (ImGui::GetCurrentContext()) ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}
- (void)scrollWheel:(NSEvent*)e {
    if (!ImGui::GetCurrentContext()) return;
    double dx = [e scrollingDeltaX], dy = [e scrollingDeltaY];
    if ([e hasPreciseScrollingDeltas]) {
        dx *= 0.1;
        dy *= 0.1;
    }
    ImGui::GetIO().AddMouseWheelEvent((float)dx, (float)dy);
}
- (void)flagsChanged:(NSEvent*)e { mac_modifiers([e modifierFlags]); }
- (void)keyEvent:(NSEvent*)e down:(bool)down {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();
    mac_modifiers([e modifierFlags]);
    ImGuiKey key = mac_key([e keyCode]);
    if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
    if (!down || ([e modifierFlags] & (NSEventModifierFlagCommand | NSEventModifierFlagControl))) return;
    NSString* chars = [e characters];
    for (NSUInteger i = 0; i < [chars length]; ++i) {
        unichar c = [chars characterAtIndex:i];
        if (c < 0x20 || c == 0x7f || (c >= 0xF700 && c <= 0xF8FF)) continue;
        io.AddInputCharacterUTF16(c);
    }
}
- (void)keyDown:(NSEvent*)e { [self keyEvent:e down:true]; }
- (void)keyUp:(NSEvent*)e { [self keyEvent:e down:false]; }

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender { return NSDragOperationCopy; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray* urls = [[sender draggingPasteboard] readObjectsForClasses:@[ [NSURL class] ]
                                                              options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    if ([urls count] == 0 || !g_window) return NO;
    g_window->drop = [[(NSURL*)urls[0] path] UTF8String];
    return YES;
}
@end

@interface SrWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation SrWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    if (g_window) g_window->closed = true;
    return NO;
}
- (void)windowDidChangeBackingProperties:(NSNotification*)n {
    if (g_window) g_window->scale = (float)[g_window->window backingScaleFactor];
}
@end

static SrWindowDelegate* g_delegate;

SrWindow* sr_window_open(const char* title, int width, int height) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSMenu* bar = [[NSMenu alloc] init];
        NSMenuItem* app_item = [[NSMenuItem alloc] init];
        [bar addItem:app_item];
        NSMenu* app_menu = [[NSMenu alloc] init];
        [app_menu addItemWithTitle:@"Quit Sonic Reimplants installer" action:@selector(terminate:) keyEquivalent:@"q"];
        [app_item setSubmenu:app_menu];
        [NSApp setMainMenu:bar];
        [NSApp finishLaunching];

        SrWindow* w = new SrWindow{};
        g_window = w;
        NSRect frame = NSMakeRect(0, 0, width, height);
        w->window = [[NSWindow alloc] initWithContentRect:frame
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                          NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
        [w->window setReleasedWhenClosed:NO];
        [w->window setTitle:[NSString stringWithUTF8String:title]];
        [w->window setContentMinSize:NSMakeSize(620, 480)];
        w->view = [[SrView alloc] initWithFrame:frame];
        [w->view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
        [w->window setContentView:w->view];
        [w->window setAcceptsMouseMovedEvents:YES];
        g_delegate = [[SrWindowDelegate alloc] init];
        [w->window setDelegate:g_delegate];
        NSTrackingArea* area = [[NSTrackingArea alloc]
            initWithRect:NSZeroRect
                 options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways |
                         NSTrackingInVisibleRect
                   owner:w->view
                userInfo:nil];
        [w->view addTrackingArea:area];
        [w->window center];
        [w->window makeKeyAndOrderFront:nil];
        [w->window makeFirstResponder:w->view];
        [NSApp activateIgnoringOtherApps:YES];
        w->scale = (float)[w->window backingScaleFactor];
        if (w->scale < 1.0f) w->scale = 1.0f;
        return w;
    }
}

void sr_window_close(SrWindow* w) {
    if (!w) return;
    [w->window setDelegate:nil];
    [w->window close];
    g_window = nullptr;
    delete w;
}

float sr_window_scale(SrWindow* w) { return w->scale; }

bool sr_window_pump(SrWindow* w, int timeout_ms) {
    @autoreleasepool {
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:timeout_ms / 1000.0];
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
        while (event) {
            [NSApp sendEvent:event];
            event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES];
        }
        [NSApp updateWindows];
    }
    return !w->closed;
}

void sr_window_size(SrWindow* w, int* width, int* height) {
    NSRect b = [w->view bounds];
    *width = (int)(b.size.width * w->scale);
    *height = (int)(b.size.height * w->scale);
}

void sr_window_present(SrWindow* w, const uint32_t* pixels, int width, int height) {
    w->frame.assign(pixels, pixels + (size_t)width * (size_t)height);
    w->frame_w = width;
    w->frame_h = height;
    [w->view setNeedsDisplay:YES];
    [w->view displayIfNeeded];
}

std::string sr_window_take_drop(SrWindow* w) {
    std::string d;
    d.swap(w->drop);
    return d;
}

#import "../../os.h"

#import <Cocoa/Cocoa.h>
#import <string.h>
#import <stdbool.h>

@interface MainView : NSView {
}
@end

@interface MainAppDelegate : NSObject <NSApplicationDelegate> {
}
@end

@interface MainWindowDelegate : NSObject <NSWindowDelegate> {
}
@end

CGContextRef bufferContext;
int mainViewHeight; // For reversing coordinates
bool shouldExit = false;
MainView *mainView;
MainAppDelegate *mainAppDelegate;
MainWindowDelegate *mainWindowDelegate;

@implementation MainView
	- (void) drawRect:(NSRect)dirtyRect {
		CGContextRef mainContext = [[NSGraphicsContext
			currentContext] CGContext];
		CGContextDrawImage(mainContext, self.bounds,
			CGBitmapContextCreateImage(bufferContext));
	}

	- (void) keyDown:(NSEvent*)theEvent {
		// Override with stub to disable beep sound on keydown
	}
@end

@implementation MainAppDelegate
	- (NSApplicationTerminateReply) applicationShouldTerminate:
			(NSApplication*)sender {
		shouldExit = true;
		return NSTerminateCancel;
	}

/* This doesn't work for some reason; hack around by using window close.
	- (BOOL) applicationShouldTerminateAfterLastWindowClosed:
		(NSApplication*)sender {
		NSLog(@"????");
		return YES;
	}*/
@end

@implementation MainWindowDelegate
	- (void) windowWillClose:(id)sender {
		shouldExit = true;
	}
@end

void os_create_window(const char* name, int width, int height) {
	@autoreleasepool {
		// Initialises NSApp 
		[NSApplication sharedApplication];
		mainAppDelegate = [MainAppDelegate new];
		[NSApp setDelegate:mainAppDelegate];
		#pragma clang diagnostic push
		#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		// Suppressed due to this being simplest for loading MainMenu
		// TODO: Stop depending on MainMenu NIB to stop dependence on XCode?
		// NOTE: XCode is just needed for 2 things, the MainMenu NIB and
		// the bundling into a .app. Everything else can be done w/o XCode.
		[NSBundle loadNibNamed:@"MainMenu" owner:[NSApp delegate]];
		#pragma clang diagnostic pop
		[NSApp activateIgnoringOtherApps:YES];
		[NSApp finishLaunching];

		// Allows our app to be an "app" without a bundle
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

		// Create window
		NSWindow *window = [[NSWindow alloc]
			initWithContentRect:NSMakeRect(0, 0, width, height)
			styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable
			backing:NSBackingStoreBuffered defer:NO];
		NSString *winName = [[NSString alloc] initWithCString:name
			encoding:NSUTF8StringEncoding];
		mainWindowDelegate = [MainWindowDelegate new];
		[window setDelegate:mainWindowDelegate];
		[window setTitle:winName];
		[window makeKeyAndOrderFront:nil];
		[window center];

		// Create rendering view
		mainView = [[MainView alloc]
			initWithFrame:NSMakeRect(0, 0, width, height)];
		[window setContentView:mainView];
		[window makeFirstResponder:mainView];
		mainViewHeight = height;

		// Create backbuffer (decouples drawing in Quartz from drawRect)
		CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(
			kCGColorSpaceGenericRGB);
		int bitmapBytesPerRow = width * 4; // RGBA
		int bitmapByteCount = bitmapBytesPerRow * height;
		void *bitmapData = calloc(bitmapByteCount, sizeof(uint8_t));
		bufferContext = CGBitmapContextCreate(bitmapData, width, height, 8,
			bitmapBytesPerRow, colorSpace, kCGImageAlphaPremultipliedLast);
		CGColorSpaceRelease(colorSpace);

		// Clear backbuffer to black; assuming emulator will only draw when
		// color changes, and color is black by default
		CGContextSetRGBFillColor(bufferContext, 0, 0, 0, 1);
		CGContextFillRect(bufferContext, CGRectMake(0, 0, width, height));
	}
}

void os_create_colormap(const float *colors, int length) {
	// Stub, not used
}

bool os_choose_bin(char* path) {
	NSOpenPanel *p = [NSOpenPanel openPanel];
	[p setTitle:@"Choose a 6502 binary"];
	[p setCanChooseDirectories:NO];
	[p setAllowsMultipleSelection:NO];
	[p setAllowedFileTypes:@[@"bin"]];
	NSInteger result = [p runModal];
	if (result == NSModalResponseOK) {
		NSString *nsPath = [[p.URLs[0] path]
			stringByResolvingSymlinksInPath];
		strcpy(path, [nsPath UTF8String]);
		return true;
	}
	else {
		return false;
	}
}

bool os_should_exit() {
	return shouldExit; // Set when Quit or window close occurs.
}

bool os_poll_event(struct event *ev) {
	@autoreleasepool {
		NSEvent *e;
		if ((e = [NSApp nextEventMatchingMask:NSEventMaskAny
				untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])) {
			switch (e.type) {
				case NSEventTypeKeyDown:
					ev->type = ET_KEYPRESS;
					ev->kp_key = [[e characters] characterAtIndex:0];
					break;
				default:
					ev->type = ET_IGNORE;
					break;
			}
			[NSApp sendEvent:e];
			[NSApp updateWindows]; // ?? What does this do?
			return true;
		}
		return false;
	}
}

// Coordinate system: 0,0 is top left. Quartz is bottom left.
void os_draw_rect(int x, int y, int w, int h, const float* colors, int c) {
	float r = colors[c * 3 + 0];
	float g = colors[c * 3 + 1];
	float b = colors[c * 3 + 2];
	CGContextSetRGBFillColor(bufferContext, r, g, b, 1);
	CGContextFillRect(bufferContext,
		CGRectMake(x, mainViewHeight - y - h, w, h));
}

void os_present() {
	[mainView setNeedsDisplay:YES];
}

void os_close() {
	// Nothing for us!
}

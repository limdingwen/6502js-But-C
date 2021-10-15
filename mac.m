#import <Cocoa/Cocoa.h>
#include <stdio.h>

CGContextRef bufferContext;
int mainViewHeight; // For reversing coordinates
bool shouldExit = false;

@interface MainView : NSView {
}
@end

@implementation MainView
	- (void) drawRect:(NSRect)dirtyRect {
		CGContextRef mainContext = [[NSGraphicsContext
			currentContext] CGContext];
		CGContextDrawImage(mainContext, self.bounds,
			CGBitmapContextCreateImage(bufferContext));
	}
@end

@interface MainAppDelegate : NSObject <NSApplicationDelegate> {
}
@end

@implementation MainAppDelegate
	- (void)applicationWillFinishLaunching:(NSNotification*)notification {
		// Create menu bar, with 1st item being the app name 
		NSMenu *menu = [[NSMenu new] autorelease];
		NSMenuItem *appMenuItem = [[NSMenuItem new] autorelease];
		NSMenu *appMenu = [[NSMenu new] autorelease];
		[menu addItem:appMenuItem];
		[appMenuItem setSubmenu:appMenu];

		// Quit menu item
		NSMenuItem *quitMenuItem = [[[NSMenuItem alloc] initWithTitle:@"Diao"
			action:@selector(terminat:) keyEquivalent:@"q"] autorelease];
		[appMenu addItem:quitMenuItem];
		
		[NSApp setMainMenu:menu];
	}

	- (NSApplicationTerminateReply)applicationShouldTerminate:(id)sender {
		shouldExit = true;
		puts("Diao");
		[bufferContext diaofuck];
		return NSTerminateCancel;
	}
@end

MainView *mainView;

void os_create_window(const char* name, int width, int height) {
	@autoreleasepool {
		// Initialises NSApp 
		[NSApplication sharedApplication];
		MainAppDelegate *mainAppDelegate = [[MainAppDelegate new] autorelease];
		[NSApp setDelegate:mainAppDelegate];

		// Allows our app to be an "app" without a bundle
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

		// Create window
		NSWindow *window = [[[NSWindow alloc]
			initWithContentRect:NSMakeRect(0, 0, width, height)
			styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable
			backing:NSBackingStoreBuffered defer:NO] autorelease];
		NSString *winName = [[[NSString alloc] initWithCString:name
			encoding:NSUTF8StringEncoding] autorelease];
		[window setTitle:winName];
		[window makeKeyAndOrderFront:nil];
		[window center];

		// Create rendering view (retained via global)
		mainView = [[[MainView alloc]
			initWithFrame:NSMakeRect(0, 0, width, height)] autorelease];
		window.contentView = mainView;
		mainViewHeight = height;

		// Create backbuffer (decouples drawing in Quartz from drawRect)
		// (Retained via global)
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

		// Bring app to front or something
		[NSApp activateIgnoringOtherApps:YES];
		[NSApp finishLaunching];
	}
}

void os_poll_event() {
	@autoreleasepool {
		NSEvent *e;
		while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny
				untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])) {
			// Handle events here
			[NSApp sendEvent:e];
		}
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

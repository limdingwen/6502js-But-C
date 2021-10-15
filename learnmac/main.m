#import <Cocoa/Cocoa.h>

int main(int argc, char **argv) {
	@autoreleasepool {
		// Initialises NSApp
		[NSApplication sharedApplication];

		// Allows our app to be an "app" without a bundle
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

		// Create menu bar, with 1st item being the app name 
		NSMenu *menu = [[NSMenu new] autorelease];
		NSMenuItem *appMenuItem = [[NSMenuItem new] autorelease];
		NSMenu *appMenu = [[NSMenu new] autorelease];
		[NSApp setMainMenu:menu];
		[menu addItem:appMenuItem];
		[appMenuItem setSubmenu:appMenu];

		// Quit menu item
		NSMenuItem *quitMenuItem = [[[NSMenuItem alloc] initWithTitle:@"Quit"
			action:@selector(terminate:) keyEquivalent:@"q"] autorelease];
		[appMenu addItem:quitMenuItem];

		// Create window
		NSWindow *window = [[[NSWindow alloc]
			initWithContentRect: NSMakeRect(100, 100, 10 * 32, 10 * 32)
			styleMask:NSTitledWindowMask|NSClosableWindowMask
			backing:NSBackingStoreBuffered defer:NO] autorelease];
		[window setTitle:@"6502"];
		[window makeKeyAndOrderFront:nil];
		
		// Run apps
		[NSApp activateIgnoringOtherApps:YES];
		[NSApp run];

		return 0;
	}
}

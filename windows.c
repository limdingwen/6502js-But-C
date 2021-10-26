#include "os.h"

#ifdef WIN32
#include <windows.h>
#include <stdio.h>

#define CLASS_NAME "WMainClass"
#define WINDOW_STYLE (WS_OVERLAPPEDWINDOW & ~WS_SIZEBOX)

// Storing stuff from WinMain so can pass into CreateWindowEx
HINSTANCE hInstanceStored;
int nCmdShowStored;

HWND windowHandle;
HBRUSH *brushes = NULL;
bool shouldExit = false;

LRESULT CALLBACK WindowProc(HWND windowHandle, UINT uMsg, WPARAM wParam,
		LPARAM lParam) {
	// Main-window-specific events
	switch (uMsg) {
		case WM_DESTROY: {
			PostQuitMessage(0); // Needed to quit... not sure why
			break;
		}
	}
    return DefWindowProc(windowHandle, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
		LPSTR pCmdLine, int nCmdShow) {
	hInstanceStored = hInstance;
	nCmdShowStored = nCmdShow;
	return our_main(__argc, __argv);
}

void os_create_window(const char *name, int width, int height) {
	// Register class
	WNDCLASS winClass = {
		.lpfnWndProc = WindowProc,
		.hInstance = hInstanceStored,
		.lpszClassName = CLASS_NAME,
		.hbrBackground = CreateSolidBrush(RGB(0, 0, 0))
	};
	RegisterClass(&winClass);

	// Calculate window size
	RECT winSize = { 0, 0, width, height };
	AdjustWindowRect(&winSize, WINDOW_STYLE, false);

	// Create window
	windowHandle = CreateWindowEx(0, CLASS_NAME, name,
		WINDOW_STYLE, CW_USEDEFAULT, CW_USEDEFAULT,
		winSize.right - winSize.left, winSize.bottom - winSize.top,
		NULL, NULL, hInstanceStored, NULL);
	ShowWindow(windowHandle, nCmdShowStored);
}

void os_create_colormap(const float *rgb, int length) {
	brushes = malloc(sizeof(HBRUSH) * length);
	for (int i = 0; i < length; i++) {
		unsigned char r = rgb[i * 3 + 0] * 255;
		unsigned char g = rgb[i * 3 + 1] * 255;
		unsigned char b = rgb[i * 3 + 2] * 255;
		brushes[i] = CreateSolidBrush(RGB(r, g, b));
	}
}

bool os_choose_bin(char* path, int pathLength) {
	OPENFILENAME ofn = {0};
	path[0] = 0;
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = windowHandle;
	ofn.lpstrFile = path;
	ofn.nMaxFile = pathLength;
	ofn.lpstrFilter = "Binary Files\0*.BIN\0";
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
	if (GetOpenFileName(&ofn)) {
		return true;
	}
	else {
		printf("OPENFILENAME Problem: %lu\n", CommDlgExtendedError());
		return false;
	}
}

bool os_should_exit(void) {
	return shouldExit; // Set in os_poll_event
}

bool os_poll_event(struct event *ev) {
	MSG msg;
	if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);

		// Handle event
		switch (msg.message) {
			case WM_QUIT: {
				shouldExit = true;
				return false; // Not processed by event loop
			}

			case WM_PAINT: {
				ev->type = ET_EXPOSE;
				return true;
			}

			// FIXME: Creates a 1-event-poll (usually 1-frame) delay for input
			case WM_CHAR: {
				ev->type = ET_KEYPRESS;
				ev->kp_key = msg.wParam;
				return true;
			}

			default: {
				return false;
			}
		}
	}
	return false;
}

void os_draw_rect(int x, int y, int w, int h, const float* rgb, int color) {
	// NOTE: If too slow, extract out into separate function.
	HDC hdc = GetDC(windowHandle);

	RECT rect = { x, y, x + w, y + h };
	FillRect(hdc, &rect, brushes[color]);

	ReleaseDC(windowHandle, hdc);
}

void os_present(void) {
}

void os_close() {
	free(brushes);
}

#endif

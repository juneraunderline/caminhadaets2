#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>
#include <wchar.h>

#include "overlay.h"

namespace overlay {
namespace {

const wchar_t *const WINDOW_CLASS = L"takeawalk_overlay";

const UINT MESSAGE_SHOW = WM_APP + 1;
const UINT MESSAGE_HIDE = WM_APP + 2;
const UINT_PTR HIDE_TIMER = 1;

const UINT DISPLAY_TIME_MS = 4000;

const int FONT_HEIGHT = 26;		// px
const int PADDING_X = 24;		// px
const int PADDING_Y = 12;		// px
const int MAX_TEXT_WIDTH = 1100;	// px

// Distance of the message from the bottom of the game window, as a fraction of its height.
const float BOTTOM_MARGIN = 0.16f;

// Compact contextual prompt, modelled after the TM Real Walk notification.
const int HINT_FONT_HEIGHT = 16;	// px
const int HINT_PADDING_X = 12;		// px
const float HINT_BOTTOM_MARGIN = 0.075f;
const int HINT_ICON_SIZE = 20;
const int HINT_ICON_GAP = 9;

const BYTE PANEL_OPACITY = 205;
const COLORREF PANEL_COLOR = RGB(18, 22, 28);
const COLORREF TEXT_COLOR = RGB(255, 255, 255);
const COLORREF KEY_COLOR = RGB(232, 236, 240);
const COLORREF KEY_TEXT_COLOR = RGB(18, 22, 28);

// A faded line: a key which does nothing where the player is now.
const COLORREF FADED_TEXT_COLOR = RGB(120, 126, 133);
const COLORREF FADED_KEY_COLOR = RGB(78, 83, 90);

HANDLE thread = NULL;
HANDLE ready = NULL;
DWORD  thread_id = 0;
HWND   window = NULL;
HWND   hint_window = NULL;
HFONT  font = NULL;
HFONT  hint_font = NULL;

CRITICAL_SECTION lock;
wchar_t          pending_text[256];
HWND             pending_anchor = NULL;
wchar_t          pending_hint[256];
HWND             pending_hint_anchor = NULL;

/**
 * @brief A bitmap with per-pixel transparency to draw a panel into.
 */
struct canvas_t
{
	HDC     screen;
	HDC     memory;
	HBITMAP bitmap;
	HGDIOBJ previous_bitmap;
	DWORD  *pixels;
	int     width;
	int     height;
};

bool canvas_open(canvas_t &canvas, const HDC screen, const HDC memory, const int width, const int height)
{
	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;

	canvas.screen = screen;
	canvas.memory = memory;
	canvas.pixels = NULL;
	canvas.width = width;
	canvas.height = height;
	canvas.bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, reinterpret_cast<void **>(&canvas.pixels), NULL, 0);
	if (! canvas.bitmap || ! canvas.pixels) {
		return false;
	}
	canvas.previous_bitmap = SelectObject(memory, canvas.bitmap);

	const RECT panel = { 0, 0, width, height };
	const HBRUSH brush = CreateSolidBrush(PANEL_COLOR);
	FillRect(memory, &panel, brush);
	DeleteObject(brush);
	SetBkMode(memory, TRANSPARENT);
	return true;
}

/**
 * @brief Opens a transparent bitmap for the compact prompt card.
 */
bool canvas_open_transparent(canvas_t &canvas, const HDC screen, const HDC memory, const int width, const int height)
{
	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	canvas.screen = screen;
	canvas.memory = memory;
	canvas.pixels = NULL;
	canvas.width = width;
	canvas.height = height;
	canvas.bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, reinterpret_cast<void **>(&canvas.pixels), NULL, 0);
	if (! canvas.bitmap || ! canvas.pixels) { return false; }
	canvas.previous_bitmap = SelectObject(memory, canvas.bitmap);
	memset(canvas.pixels, 0, width * height * sizeof(DWORD));
	SetBkMode(memory, TRANSPARENT);
	return true;
}

/**
 * @brief Puts the drawn panel on screen in the given window and releases the bitmap.
 */
void canvas_show(canvas_t &canvas, const HWND target, POINT position)
{
	GdiFlush();

	// GDI leaves the alpha channel empty. The whole panel gets the same opacity,
	// and the colours are scaled by it as layered windows expect.

	for (int i = 0; i < canvas.width * canvas.height; ++i) {
		const DWORD color = canvas.pixels[i];
		if (color == 0) { continue; } // Transparent rounded corners.
		const DWORD red = ((color >> 16) & 0xFF) * PANEL_OPACITY / 255;
		const DWORD green = ((color >> 8) & 0xFF) * PANEL_OPACITY / 255;
		const DWORD blue = (color & 0xFF) * PANEL_OPACITY / 255;
		canvas.pixels[i] = (static_cast<DWORD>(PANEL_OPACITY) << 24) | (red << 16) | (green << 8) | blue;
	}

	SIZE size = { canvas.width, canvas.height };
	POINT source = { 0, 0 };
	BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
	UpdateLayeredWindow(target, canvas.screen, &position, &size, canvas.memory, &source, 0, &blend, ULW_ALPHA);
	ShowWindow(target, SW_SHOWNOACTIVATE);
	SetWindowPos(target, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

	SelectObject(canvas.memory, canvas.previous_bitmap);
	DeleteObject(canvas.bitmap);
}

/**
 * @brief Draws the message on a dark panel near the bottom of the anchor window.
 */
void present(const wchar_t *const text, const HWND anchor)
{
	RECT anchor_rect;
	if (! anchor || ! GetClientRect(anchor, &anchor_rect)) {
		return;
	}
	POINT anchor_origin = { 0, 0 };
	ClientToScreen(anchor, &anchor_origin);

	const HDC screen = GetDC(NULL);
	const HDC memory = CreateCompatibleDC(screen);
	const HGDIOBJ previous_font = SelectObject(memory, font);

	RECT text_rect = { 0, 0, MAX_TEXT_WIDTH, 0 };
	DrawTextW(memory, text, -1, &text_rect, DT_CALCRECT | DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
	const int width = text_rect.right + PADDING_X * 2;
	const int height = text_rect.bottom + PADDING_Y * 2;

	canvas_t canvas;
	if (canvas_open(canvas, screen, memory, width, height)) {
		SetTextColor(memory, TEXT_COLOR);
		RECT draw_rect = { PADDING_X, PADDING_Y, width - PADDING_X, height - PADDING_Y };
		DrawTextW(memory, text, -1, &draw_rect, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);

		const POINT position = {
			anchor_origin.x + (anchor_rect.right - width) / 2,
			anchor_origin.y + static_cast<int>(anchor_rect.bottom * (1.0f - BOTTOM_MARGIN)) - height
		};
		canvas_show(canvas, window, position);
	}

	SelectObject(memory, previous_font);
	DeleteDC(memory);
	ReleaseDC(NULL, screen);
}

/**
 * @brief One line of the key hint: the key, then what it does.
 */
struct hint_line_t
{
	const wchar_t *key;
	int            key_length;
	const wchar_t *action;
	int            action_length;
	SIZE           key_size;
	SIZE           action_size;
	bool           faded;
};

/**
 * @brief Splits "[key] action" lines separated by line feeds. A line starting with '~' is faded.
 */
/**
 * @brief Draws a compact contextual prompt in the same visual language as TM Real Walk.
 */
void present_hint(const wchar_t *const text, const HWND anchor)
{
	RECT anchor_rect;
	if (! anchor || ! GetClientRect(anchor, &anchor_rect)) { return; }
	POINT anchor_origin = { 0, 0 };
	ClientToScreen(anchor, &anchor_origin);

	const wchar_t *label = text;
	wchar_t composed[256] = {};
	if (text[0] == L'[') {
		const wchar_t *const close = wcschr(text, L']');
		if (close) {
			const wchar_t *action = close + 1;
			while (*action == L' ') { ++action; }
			const int key_length = static_cast<int>(close - text - 1);
			if (key_length > 0 && key_length < 24) {
				wchar_t key[24] = {};
				wmemcpy(key, text + 1, key_length);
				wsprintfW(composed, L"%s  (%s)", action, key);
				label = composed;
			}
		}
	}

	const HDC screen = GetDC(NULL);
	const HDC memory = CreateCompatibleDC(screen);
	const HGDIOBJ previous_font = SelectObject(memory, hint_font);
	SIZE label_size = {};
	GetTextExtentPoint32W(memory, label, static_cast<int>(wcslen(label)), &label_size);
	const int height = 36;
	const int width = HINT_PADDING_X * 2 + HINT_ICON_SIZE + HINT_ICON_GAP + label_size.cx;

	canvas_t canvas;
	if (canvas_open_transparent(canvas, screen, memory, width, height)) {
		const HBRUSH panel_brush = CreateSolidBrush(PANEL_COLOR);
		const HBRUSH icon_brush = CreateSolidBrush(RGB(30, 190, 211));
		const HPEN no_pen = static_cast<HPEN>(GetStockObject(NULL_PEN));
		const HGDIOBJ old_brush = SelectObject(memory, panel_brush);
		const HGDIOBJ old_pen = SelectObject(memory, no_pen);

		RoundRect(memory, 0, 0, width, height - 1, 8, 8);
		RECT accent = { 7, height - 3, width - 7, height - 1 };
		FillRect(memory, &accent, icon_brush);

		SelectObject(memory, icon_brush);
		Ellipse(memory, HINT_PADDING_X, (height - HINT_ICON_SIZE) / 2 - 1,
			HINT_PADDING_X + HINT_ICON_SIZE, (height + HINT_ICON_SIZE) / 2 - 1);
		const HPEN icon_pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
		SelectObject(memory, icon_pen);
		SelectObject(memory, GetStockObject(HOLLOW_BRUSH));
		const int ix = HINT_PADDING_X + 6;
		const int iy = (height - 14) / 2 - 1;
		Rectangle(memory, ix, iy, ix + 8, iy + 14);
		MoveToEx(memory, ix + 5, iy + 7, NULL);
		LineTo(memory, ix + 6, iy + 7);

		SetTextColor(memory, TEXT_COLOR);
		RECT text_rect = { HINT_PADDING_X + HINT_ICON_SIZE + HINT_ICON_GAP, 0, width - HINT_PADDING_X, height - 2 };
		DrawTextW(memory, label, -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

		SelectObject(memory, old_pen);
		SelectObject(memory, old_brush);
		DeleteObject(icon_pen);
		DeleteObject(icon_brush);
		DeleteObject(panel_brush);

		const POINT position = {
			anchor_origin.x + (anchor_rect.right - width) / 2,
			anchor_origin.y + static_cast<int>(anchor_rect.bottom * (1.0f - HINT_BOTTOM_MARGIN)) - height
		};
		canvas_show(canvas, hint_window, position);
	}
	SelectObject(memory, previous_font);
	DeleteDC(memory);
	ReleaseDC(NULL, screen);
}

LRESULT CALLBACK window_procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message) {
		case MESSAGE_SHOW: {
			const bool hint = (handle == hint_window);
			wchar_t text[256];
			HWND anchor = NULL;
			EnterCriticalSection(&lock);
			memcpy(text, hint ? pending_hint : pending_text, sizeof(text));
			anchor = hint ? pending_hint_anchor : pending_anchor;
			LeaveCriticalSection(&lock);

			if (hint) {
				// Stays until it is taken away.

				present_hint(text, anchor);
			}
			else {
				present(text, anchor);
				SetTimer(handle, HIDE_TIMER, DISPLAY_TIME_MS, NULL);
			}
			return 0;
		}
		case MESSAGE_HIDE:
		case WM_TIMER: {
			KillTimer(handle, HIDE_TIMER);
			ShowWindow(handle, SW_HIDE);
			return 0;
		}
	}
	return DefWindowProcW(handle, message, wparam, lparam);
}

DWORD WINAPI thread_main(LPVOID)
{
	WNDCLASSEXW window_class = {};
	window_class.cbSize = sizeof(window_class);
	window_class.lpfnWndProc = window_procedure;
	window_class.hInstance = GetModuleHandleW(NULL);
	window_class.lpszClassName = WINDOW_CLASS;
	RegisterClassExW(&window_class);

	// Never take focus or mouse clicks, and stay out of the task bar.

	const DWORD style = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
	window = CreateWindowExW(style, WINDOW_CLASS, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, window_class.hInstance, NULL);
	hint_window = CreateWindowExW(style, WINDOW_CLASS, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, window_class.hInstance, NULL);
	font = CreateFontW(-FONT_HEIGHT, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
	hint_font = CreateFontW(-HINT_FONT_HEIGHT, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
	SetEvent(ready);

	if (window && hint_window) {
		MSG message;
		while (GetMessageW(&message, NULL, 0, 0) > 0) {
			DispatchMessageW(&message);
		}
	}
	if (hint_window) {
		DestroyWindow(hint_window);
		hint_window = NULL;
	}
	if (window) {
		DestroyWindow(window);
		window = NULL;
	}
	if (font) {
		DeleteObject(font);
		font = NULL;
	}
	if (hint_font) {
		DeleteObject(hint_font);
		hint_font = NULL;
	}
	UnregisterClassW(WINDOW_CLASS, window_class.hInstance);
	return 0;
}

} // namespace

bool init(void)
{
	InitializeCriticalSection(&lock);
	ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	thread = CreateThread(NULL, 0, thread_main, NULL, 0, &thread_id);
	if (! thread) {
		return false;
	}
	WaitForSingleObject(ready, 2000);
	return (window != NULL) && (hint_window != NULL);
}

void shutdown(void)
{
	if (thread) {
		PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
		WaitForSingleObject(thread, 2000);
		CloseHandle(thread);
		thread = NULL;
		DeleteCriticalSection(&lock);
	}
	if (ready) {
		CloseHandle(ready);
		ready = NULL;
	}
}

void show(const char *const text)
{
	if (! window) {
		return;
	}

	// Shown over whichever window is in front, which is the game when a key press caused this.

	EnterCriticalSection(&lock);
	MultiByteToWideChar(CP_UTF8, 0, text, -1, pending_text, 256);
	pending_text[255] = 0;
	pending_anchor = GetForegroundWindow();
	LeaveCriticalSection(&lock);
	PostMessageW(window, MESSAGE_SHOW, 0, 0);
}

void hide(void)
{
	if (window) {
		PostMessageW(window, MESSAGE_HIDE, 0, 0);
	}
}

void set_hands(const bool)
{
	// The old 2D hand drawing was removed; no body overlay is shown.
}

void set_hint(const char *const text)
{
	if (! hint_window) {
		return;
	}
	if (! text || ! *text) {
		PostMessageW(hint_window, MESSAGE_HIDE, 0, 0);
		return;
	}
	EnterCriticalSection(&lock);
	MultiByteToWideChar(CP_UTF8, 0, text, -1, pending_hint, 256);
	pending_hint[255] = 0;
	pending_hint_anchor = GetForegroundWindow();
	LeaveCriticalSection(&lock);
	PostMessageW(hint_window, MESSAGE_SHOW, 0, 0);
}

} // namespace overlay

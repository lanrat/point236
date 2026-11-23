// Internal declarations for the Win32-on-browser shim used by the Peter runtime port.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <msacm.h>
#include <commdlg.h>
#include <commctrl.h>
#include <richedit.h>
#include <errno.h>
#include <malloc.h>
#include <functional>
#include <emscripten.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <cstdint>
#include <cstring>
#include <cstdio>

#define SHIM_LOG(...) do { std::printf(__VA_ARGS__); std::printf("\n"); } while (0)
void shim_unimpl(const char* name);
#define UNIMPL() shim_unimpl(__func__)

// ---------------------------------------------------------------- surfaces
// RGBA surface, pixel layout = canvas ImageData (bytes R,G,B,A -> uint32 0xAABBGGRR)
struct Surface {
	int w = 0, h = 0;
	uint32_t* px = nullptr;
	int dl = 0, dt = 0, dr = 0, db = 0;	// dirty rect (empty when dr<=dl)
	void resize(int nw, int nh);
	void dirty(int l, int t, int r, int b);
	~Surface();
};
static inline uint32_t RGBA_FROM_COLORREF(COLORREF c) { return 0xff000000u | (c & 0xffffffu); } // COLORREF is 0x00BBGGRR already
static inline COLORREF COLORREF_FROM_RGBA(uint32_t p) { return p & 0xffffffu; }

// ---------------------------------------------------------------- GDI objects
enum GdiType { GDI_NONE = 0, GDI_BRUSH, GDI_PEN, GDI_FONT, GDI_BITMAP, GDI_PALETTE, GDI_REGION };
struct GdiObj {
	uint32_t magic = 0x47444930;
	GdiType type = GDI_NONE;
	bool stock = false;
	// brush/pen
	COLORREF color = 0; int style = 0; int width = 1; bool null_ = false;
	// font
	LOGFONTA lf{}; std::string css;
	// bitmap
	Surface* surf = nullptr; int bpp = 32;
	// palette
	std::vector<PALETTEENTRY> pal;
};
GdiObj* gdi_get(HGDIOBJ h, GdiType t);

struct Wnd;
struct DCRec {
	uint32_t magic = 0x44435f30;
	Wnd* wnd = nullptr;			// window DC
	GdiObj* bitmap = nullptr;	// memory DC selected bitmap
	GdiObj* font = nullptr;
	GdiObj* brush = nullptr;
	GdiObj* pen = nullptr;
	GdiObj* palette = nullptr;
	COLORREF textcol = 0, bkcol = 0xffffff;
	int bkmode = OPAQUE;
	UINT align = TA_LEFT | TA_TOP;
	int stretchmode = BLACKONWHITE;
	bool printer = false;
	Surface* surface();
	int offx = 0, offy = 0;	// origin offset inside surface
};
DCRec* dc_get(HDC h);

// ---------------------------------------------------------------- windows
enum CtrlKind { CK_NONE = 0, CK_BUTTON, CK_CHECK, CK_RADIO, CK_GROUP, CK_EDIT, CK_MEMO, CK_LISTBOX,
	CK_COMBO, CK_COMBOEDIT, CK_STATIC, CK_SCROLL, CK_TRACK, CK_PROGRESS, CK_STATUS, CK_TAB, CK_LISTVIEW };

struct Wnd {
	uint32_t magic = 0x574e4430;
	int id = 0;						// DOM id
	std::string cls;
	WNDPROC proc = nullptr;
	DWORD style = 0, exstyle = 0;
	Wnd* parent = nullptr;
	std::vector<Wnd*> children;
	HMENU menu = 0;					// control id
	RECT rc{};						// window rect in parent client coords (desktop coords for top-level)
	std::string text;
	bool visible = false, enabled = true, destroyed = false;
	std::map<int, LONG_PTR> longs;	// GWL_ values and extra bytes
	CtrlKind kind = CK_NONE;
	int check = 0;
	std::vector<std::string> items;
	int cursel = -1;
	int selstart = 0, selend = 0;
	int pos = 0, rmin = 0, rmax = 100;	// scroll/track/progress
	HFONT font = 0;
	bool readonly = false;
	Surface surf;					// client area pixels (GDI)
	bool hasCanvas = false;
	bool glCanvas = false;
	int pixelformat = 0;
	HCURSOR clsCursor = 0;
	HBRUSH clsBrush = 0;
	UINT clsStyle = 0;
	bool needPaint = false;
	bool erase = false;
	bool zoomed = false, iconic = false;
	RECT clientRect() const;		// client rect in window coords (left/top = NC offsets)
};
Wnd* wnd_get(HWND h);
POINT wnd_client_origin(Wnd* w);	// desktop coordinates of client (0,0)
void wnd_sync_dom(Wnd* w);
LRESULT wnd_send(Wnd* w, UINT msg, WPARAM wp, LPARAM lp);
void post_msg(HWND h, UINT msg, WPARAM wp, LPARAM lp);
extern int g_screenW, g_screenH;
extern Wnd* g_focus;
void flush_surfaces();
void surface_present(Wnd* w);
void shim_ctl_invalidated(Wnd* w);
void ensure_canvas(Wnd* w);

// text helpers
std::string ansi_to_utf8(const char* s, int len = -1);
std::string utf8_to_ansi(const char* s);

// paths
std::string win_to_unix(const char* path);

// time
DWORD shim_ticks();

// GL
void* gl_get_proc(const char* name);
bool gl_attach_window(Wnd* w);
void gl_swap(Wnd* w);
void gl_resize(Wnd* w);

// audio
void audio_pump();
void midi_pump();

// generic object handle registry check
template<class T> static inline T* checked(void* p, uint32_t magic) {
	if (!p) return nullptr; T* t = (T*)p;
	if ((uintptr_t)p < 4096) return nullptr;
	return (*(uint32_t*)t == magic) ? t : nullptr;
}

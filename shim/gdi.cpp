// Win32 shim: GDI (device contexts, bitmaps, brushes, fonts, text, DIB blits) on RGBA surfaces
#include "shim.h"
#include <algorithm>
#include <cmath>

// ------------------------------------------------------------------ surfaces
void Surface::resize(int nw, int nh)
{
	if (nw < 0) nw = 0; if (nh < 0) nh = 0;
	if (nw == w && nh == h && px) return;
	uint32_t* n = (uint32_t*)calloc((size_t)std::max(1, nw * nh), 4);
	// keep old content
	if (px) for (int y = 0; y < std::min(h, nh); y++) memcpy(n + y * nw, px + y * w, std::min(w, nw) * 4);
	free(px); px = n; w = nw; h = nh;
	dl = 0; dt = 0; dr = w; db = h;
}
void Surface::dirty(int l, int t, int r, int b)
{
	l = std::max(l, 0); t = std::max(t, 0); r = std::min(r, w); b = std::min(b, h);
	if (r <= l || b <= t) return;
	if (dr <= dl) { dl = l; dt = t; dr = r; db = b; return; }
	dl = std::min(dl, l); dt = std::min(dt, t); dr = std::max(dr, r); db = std::max(db, b);
}
Surface::~Surface() { free(px); }

EM_JS(void, jsg_blit, (int id, const uint32_t* px, int w, int h, int l, int t, int r, int b), { PeterWin.blit(id, px, w, h, l, t, r, b); });

static std::vector<Wnd*> g_canvasWnds;
void ensure_canvas(Wnd* w)
{
	if (w->hasCanvas) return;
	w->hasCanvas = true;
	RECT c = w->clientRect();
	w->surf.resize(c.right - c.left, c.bottom - c.top);
	// initial content: class background
	uint32_t bg = (w->kind != CK_NONE) ? RGBA_FROM_COLORREF(GetSysColor(COLOR_BTNFACE)) : 0xff000000;
	if (w->clsBrush) { GdiObj* b = gdi_get(w->clsBrush, GDI_BRUSH); if (b) bg = RGBA_FROM_COLORREF(b->color); else if ((uintptr_t)w->clsBrush <= 31) bg = RGBA_FROM_COLORREF(GetSysColor((int)(uintptr_t)w->clsBrush - 1)); }
	for (int i = 0; i < w->surf.w * w->surf.h; i++) w->surf.px[i] = bg;
	g_canvasWnds.push_back(w);
}
void surface_present(Wnd* w)
{
	Surface& s = w->surf;
	if (!w->hasCanvas || s.dr <= s.dl || !s.px) return;
	jsg_blit(w->id, s.px, s.w, s.h, s.dl, s.dt, s.dr, s.db);
	s.dl = s.dt = s.dr = s.db = 0;
}
void flush_surfaces()
{
	for (size_t i = 0; i < g_canvasWnds.size();)
	{
		Wnd* w = g_canvasWnds[i];
		if (w->magic != 0x574e4430) { g_canvasWnds.erase(g_canvasWnds.begin() + i); continue; }
		surface_present(w);
		i++;
	}
}

// ------------------------------------------------------------------ objects
GdiObj* gdi_get(HGDIOBJ h, GdiType t)
{
	GdiObj* o = checked<GdiObj>(h, 0x47444930);
	if (!o) return nullptr;
	if (t != GDI_NONE && o->type != t) return nullptr;
	return o;
}
static GdiObj* new_obj(GdiType t) { GdiObj* o = new GdiObj; o->type = t; return o; }

static std::string font_css(const LOGFONTA& lf, double* emOut)
{
	double em;
	if (lf.lfHeight < 0) em = -lf.lfHeight;
	else if (lf.lfHeight > 0) em = lf.lfHeight / 1.15;
	else em = 13;
	std::string face = lf.lfFaceName;
	std::string fl; for (char c : face) fl += (char)tolower((unsigned char)c);
	std::string fam;
	if (fl.find("courier") != std::string::npos || (lf.lfPitchAndFamily & 3) == FIXED_PITCH || (lf.lfPitchAndFamily & 0xf0) == FF_MODERN)
		fam = "'Courier New', 'Liberation Mono', Courier, monospace";
	else if (fl.find("times") != std::string::npos || (lf.lfPitchAndFamily & 0xf0) == FF_ROMAN)
		fam = "'Times New Roman', 'Liberation Serif', Times, serif";
	else if (fl.find("system") != std::string::npos)
		fam = "Tahoma, Arial, 'Liberation Sans', sans-serif";
	else fam = "'" + face + "', Arial, 'Liberation Sans', Helvetica, sans-serif";
	char buf[300];
	snprintf(buf, sizeof buf, "%s%s|%s%d %.2fpx %s", lf.lfUnderline ? "u" : "", lf.lfStrikeOut ? "s" : "", lf.lfItalic ? "italic " : "", lf.lfWeight ? (int)lf.lfWeight : 400, em, fam.c_str());
	if (emOut) *emOut = em;
	return buf;
}

static GdiObj g_stock[STOCK_LAST + 1];
static GdiObj* g_sysBrush[32];
static void init_stock()
{
	static bool done = false; if (done) return; done = true;
	for (int i = 0; i <= STOCK_LAST; i++) { g_stock[i].stock = true; }
	auto br = [](int i, COLORREF c, bool nul = false) { g_stock[i].type = GDI_BRUSH; g_stock[i].color = c; g_stock[i].null_ = nul; };
	br(WHITE_BRUSH, RGB(255, 255, 255)); br(LTGRAY_BRUSH, RGB(192, 192, 192)); br(GRAY_BRUSH, RGB(128, 128, 128));
	br(DKGRAY_BRUSH, RGB(64, 64, 64)); br(BLACK_BRUSH, 0); br(NULL_BRUSH, 0, true); br(DC_BRUSH, RGB(255, 255, 255));
	auto pn = [](int i, COLORREF c, bool nul = false) { g_stock[i].type = GDI_PEN; g_stock[i].color = c; g_stock[i].null_ = nul; };
	pn(WHITE_PEN, RGB(255, 255, 255)); pn(BLACK_PEN, 0); pn(NULL_PEN, 0, true); pn(DC_PEN, 0);
	int fonts[] = {OEM_FIXED_FONT, ANSI_FIXED_FONT, ANSI_VAR_FONT, SYSTEM_FONT, DEVICE_DEFAULT_FONT, SYSTEM_FIXED_FONT, DEFAULT_GUI_FONT};
	for (int f : fonts)
	{
		g_stock[f].type = GDI_FONT;
		LOGFONTA& lf = g_stock[f].lf; memset(&lf, 0, sizeof lf);
		lf.lfHeight = (f == SYSTEM_FONT) ? 16 : -11; lf.lfWeight = (f == SYSTEM_FONT) ? FW_BOLD : FW_NORMAL;
		strcpy(lf.lfFaceName, (f == OEM_FIXED_FONT || f == ANSI_FIXED_FONT || f == SYSTEM_FIXED_FONT) ? "Courier New" : (f == SYSTEM_FONT ? "System" : "Tahoma"));
		g_stock[f].css = font_css(lf, nullptr);
	}
	g_stock[DEFAULT_PALETTE].type = GDI_PALETTE;
	g_stock[DEFAULT_PALETTE].pal.resize(20);
	static const COLORREF def20[20] = {0x000000,0x000080,0x008000,0x008080,0x800000,0x800080,0x808000,0xc0c0c0,0xc0dcc0,0xf0caa6,
		0xf0fbff,0xa4a0a0,0x808080,0x0000ff,0x00ff00,0x00ffff,0xff0000,0xff00ff,0xffff00,0xffffff};
	for (int i = 0; i < 20; i++) { g_stock[DEFAULT_PALETTE].pal[i] = {GetRValue(def20[i]), GetGValue(def20[i]), GetBValue(def20[i]), 0}; }
}

static GdiObj* g_defBitmap = nullptr;

// ------------------------------------------------------------------ DCs
DCRec* dc_get(HDC h) { return checked<DCRec>(h, 0x44435f30); }
Surface* DCRec::surface()
{
	if (wnd) { if (!wnd->hasCanvas && !wnd->glCanvas) ensure_canvas(wnd); return &wnd->surf; }
	if (bitmap && bitmap->surf) return bitmap->surf;
	return nullptr;
}
static DCRec* new_dc()
{
	init_stock();
	DCRec* d = new DCRec;
	d->font = &g_stock[SYSTEM_FONT]; d->brush = &g_stock[WHITE_BRUSH]; d->pen = &g_stock[BLACK_PEN]; d->palette = &g_stock[DEFAULT_PALETTE];
	return d;
}
static void mark(DCRec* d, int l, int t, int r, int b)
{
	if (d->wnd) d->wnd->surf.dirty(l, t, r, b);
}

extern "C" {

HDC WINAPI GetDC(HWND h)
{
	DCRec* d = new_dc();
	Wnd* w = wnd_get(h);
	if (w) d->wnd = w;
	return (HDC)d;
}
int WINAPI ReleaseDC(HWND h, HDC dc) { DCRec* d = dc_get(dc); if (d) { d->magic = 0; delete d; } return 1; }
HDC WINAPI CreateCompatibleDC(HDC dc)
{
	DCRec* d = new_dc();
	if (!g_defBitmap) { g_defBitmap = new_obj(GDI_BITMAP); g_defBitmap->stock = true; g_defBitmap->surf = new Surface; g_defBitmap->surf->resize(1, 1); g_defBitmap->bpp = 1; }
	d->bitmap = g_defBitmap;
	return (HDC)d;
}
HDC WINAPI CreateDCA(LPCSTR drv, LPCSTR dev, LPCSTR port, const DEVMODEA* dm) { return NULL; }
WINBOOL WINAPI DeleteDC(HDC dc) { DCRec* d = dc_get(dc); if (d) { d->magic = 0; delete d; } return TRUE; }

HDC WINAPI BeginPaint(HWND h, LPPAINTSTRUCT ps)
{
	Wnd* w = wnd_get(h);
	memset(ps, 0, sizeof(*ps));
	HDC dc = GetDC(h);
	ps->hdc = dc;
	if (w)
	{
		RECT c = w->clientRect();
		ps->rcPaint = {0, 0, c.right - c.left, c.bottom - c.top};
		if (w->erase) { w->erase = false; ps->fErase = !wnd_send(w, WM_ERASEBKGND, (WPARAM)dc, 0); }
		w->needPaint = false;
	}
	return dc;
}
WINBOOL WINAPI EndPaint(HWND h, const PAINTSTRUCT* ps) { ReleaseDC(h, ps->hdc); return TRUE; }
WINBOOL WINAPI InvalidateRect(HWND h, const RECT* r, WINBOOL erase)
{
	Wnd* w = wnd_get(h);
	if (!w) return FALSE;
	w->needPaint = true; if (erase) w->erase = true;
	if (w->kind != CK_NONE) shim_ctl_invalidated(w);
	return TRUE;
}

HGDIOBJ WINAPI GetStockObject(int i) { init_stock(); if (i < 0 || i > STOCK_LAST) return NULL; return (HGDIOBJ)&g_stock[i]; }
HBRUSH WINAPI GetSysColorBrush(int i)
{
	if (i < 0 || i >= 32) i = COLOR_BTNFACE;
	if (!g_sysBrush[i]) { g_sysBrush[i] = new_obj(GDI_BRUSH); g_sysBrush[i]->stock = true; g_sysBrush[i]->color = GetSysColor(i); }
	return (HBRUSH)g_sysBrush[i];
}
HBRUSH WINAPI CreateSolidBrush(COLORREF c) { GdiObj* o = new_obj(GDI_BRUSH); o->color = c & 0xffffff; return (HBRUSH)o; }
HBRUSH WINAPI CreateHatchBrush(int style, COLORREF c) { GdiObj* o = new_obj(GDI_BRUSH); o->color = c & 0xffffff; o->style = style + 1; return (HBRUSH)o; }
HFONT WINAPI CreateFontA(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD under, DWORD strike, DWORD cs,
	DWORD op, DWORD cp, DWORD q, DWORD pf, LPCSTR face)
{
	GdiObj* o = new_obj(GDI_FONT);
	LOGFONTA& lf = o->lf;
	lf.lfHeight = h; lf.lfWidth = w; lf.lfEscapement = esc; lf.lfOrientation = orient; lf.lfWeight = weight;
	lf.lfItalic = (BYTE)italic; lf.lfUnderline = (BYTE)under; lf.lfStrikeOut = (BYTE)strike; lf.lfCharSet = (BYTE)cs;
	lf.lfPitchAndFamily = (BYTE)pf;
	strncpy(lf.lfFaceName, face ? face : "Arial", LF_FACESIZE - 1);
	o->css = font_css(lf, nullptr);
	return (HFONT)o;
}
HPALETTE WINAPI CreatePalette(const LOGPALETTE* lp)
{
	GdiObj* o = new_obj(GDI_PALETTE);
	o->pal.assign(lp->palPalEntry, lp->palPalEntry + lp->palNumEntries);
	return (HPALETTE)o;
}
HBITMAP WINAPI CreateBitmap(int w, int h, UINT planes, UINT bpp, const VOID* bits)
{
	GdiObj* o = new_obj(GDI_BITMAP);
	o->surf = new Surface; o->surf->resize(w, h); o->bpp = bpp * planes;
	if (bits && o->bpp == 1)
	{
		int stride = ((w + 15) / 16) * 2;
		const BYTE* b = (const BYTE*)bits;
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
			o->surf->px[y * w + x] = ((b[y * stride + x / 8] >> (7 - (x & 7))) & 1) ? 0xffffffff : 0xff000000;
	}
	else if (bits && o->bpp == 32)
	{
		const uint32_t* b = (const uint32_t*)bits;
		for (int i = 0; i < w * h; i++) { uint32_t p = b[i]; o->surf->px[i] = 0xff000000 | ((p & 0xff) << 16) | (p & 0xff00) | ((p >> 16) & 0xff); }
	}
	else for (int i = 0; i < w * h; i++) o->surf->px[i] = 0xff000000;
	return (HBITMAP)o;
}
HBITMAP WINAPI CreateCompatibleBitmap(HDC dc, int w, int h)
{
	GdiObj* o = new_obj(GDI_BITMAP);
	o->surf = new Surface; o->surf->resize(w, h); o->bpp = 32;
	for (int i = 0; i < w * h; i++) o->surf->px[i] = 0xff000000;
	DCRec* d = dc_get(dc);
	if (d && !d->wnd && d->bitmap && d->bitmap->bpp == 1) o->bpp = 1;
	return (HBITMAP)o;
}
HGDIOBJ WINAPI SelectObject(HDC dc, HGDIOBJ h)
{
	DCRec* d = dc_get(dc); GdiObj* o = gdi_get(h, GDI_NONE);
	if (!d || !o) return NULL;
	GdiObj* old = nullptr;
	switch (o->type)
	{
	case GDI_BRUSH: old = d->brush; d->brush = o; break;
	case GDI_PEN: old = d->pen; d->pen = o; break;
	case GDI_FONT: old = d->font; d->font = o; break;
	case GDI_BITMAP: old = d->bitmap; d->bitmap = o; break;
	case GDI_PALETTE: old = d->palette; d->palette = o; break;
	default: break;
	}
	return (HGDIOBJ)old;
}
WINBOOL WINAPI DeleteObject(HGDIOBJ h)
{
	GdiObj* o = gdi_get(h, GDI_NONE);
	if (!o || o->stock) return FALSE;
	if (o->surf) { delete o->surf; o->surf = nullptr; }
	o->magic = 0; delete o;
	return TRUE;
}
HPALETTE WINAPI SelectPalette(HDC dc, HPALETTE p, WINBOOL bg)
{
	DCRec* d = dc_get(dc); GdiObj* o = gdi_get(p, GDI_PALETTE);
	if (!d) return NULL;
	GdiObj* old = d->palette;
	if (o) d->palette = o;
	return (HPALETTE)old;
}
UINT WINAPI RealizePalette(HDC dc) { return 0; }
WINBOOL WINAPI UnrealizeObject(HGDIOBJ h) { return TRUE; }

int WINAPI GetDeviceCaps(HDC dc, int i)
{
	switch (i)
	{
	case BITSPIXEL: return 32;
	case PLANES: return 1;
	case HORZRES: return g_screenW;
	case VERTRES: return g_screenH;
	case LOGPIXELSX: case LOGPIXELSY: return 96;
	case RASTERCAPS: return RC_BITBLT | RC_STRETCHBLT | RC_DIBTODEV | RC_STRETCHDIB;
	case NUMCOLORS: return -1;
	case SIZEPALETTE: return 0;
	case TECHNOLOGY: return DT_RASDISPLAY;
	case HORZSIZE: return g_screenW * 254 / 960;
	case VERTSIZE: return g_screenH * 254 / 960;
	case VREFRESH: return 60;
	}
	return 0;
}
COLORREF WINAPI SetTextColor(HDC dc, COLORREF c) { DCRec* d = dc_get(dc); if (!d) return CLR_INVALID; COLORREF o = d->textcol; d->textcol = c & 0xffffff; return o; }
COLORREF WINAPI SetBkColor(HDC dc, COLORREF c) { DCRec* d = dc_get(dc); if (!d) return CLR_INVALID; COLORREF o = d->bkcol; d->bkcol = c & 0xffffff; return o; }
int WINAPI SetBkMode(HDC dc, int m) { DCRec* d = dc_get(dc); if (!d) return 0; int o = d->bkmode; d->bkmode = m; return o; }
UINT WINAPI SetTextAlign(HDC dc, UINT a) { DCRec* d = dc_get(dc); if (!d) return GDI_ERROR; UINT o = d->align; d->align = a; return o; }
int WINAPI SetStretchBltMode(HDC dc, int m) { DCRec* d = dc_get(dc); if (!d) return 0; int o = d->stretchmode; d->stretchmode = m; return o; }

// ------------------------------------------------------------------ drawing primitives
static void fill_rect(DCRec* d, int l, int t, int r, int b, uint32_t c)
{
	Surface* s = d->surface(); if (!s || !s->px) return;
	l = std::max(l, 0); t = std::max(t, 0); r = std::min(r, s->w); b = std::min(b, s->h);
	for (int y = t; y < b; y++) { uint32_t* p = s->px + y * s->w; for (int x = l; x < r; x++) p[x] = c; }
	mark(d, l, t, r, b);
}
int WINAPI FillRect(HDC dc, const RECT* r, HBRUSH hb)
{
	DCRec* d = dc_get(dc); if (!d) return 0;
	GdiObj* b = gdi_get(hb, GDI_BRUSH);
	COLORREF c;
	if (b) { if (b->null_) return 1; c = b->color; }
	else if ((uintptr_t)hb <= 31 && hb) c = GetSysColor((int)(uintptr_t)hb - 1);
	else return 0;
	fill_rect(d, r->left, r->top, r->right, r->bottom, RGBA_FROM_COLORREF(c));
	return 1;
}

// read a DIB pixel as RGBA
struct DibReader {
	const BITMAPINFO* bi; const BYTE* bits; int w, h, bpp, stride; bool topdown; uint32_t pal[256]; DWORD masks[3]; int sh[3]; int bw[3];
	DibReader(const BITMAPINFO* b, const void* data, UINT usage, DCRec* dc) : bi(b), bits((const BYTE*)data)
	{
		w = b->bmiHeader.biWidth; h = b->bmiHeader.biHeight; topdown = h < 0; if (h < 0) h = -h;
		bpp = b->bmiHeader.biBitCount; stride = ((w * bpp + 31) / 32) * 4;
		int ncol = b->bmiHeader.biClrUsed ? (int)b->bmiHeader.biClrUsed : (bpp <= 8 ? (1 << bpp) : 0);
		const BYTE* colors = (const BYTE*)b + b->bmiHeader.biSize;
		if (bpp <= 8)
		{
			for (int i = 0; i < 256; i++) pal[i] = 0xff000000;
			if (usage == DIB_PAL_COLORS)
			{
				const WORD* idx = (const WORD*)colors;
				static const std::vector<PALETTEENTRY> none;
				const std::vector<PALETTEENTRY>& P = (dc && dc->palette) ? dc->palette->pal : none;
				for (int i = 0; i < ncol && i < 256; i++) { int k = idx[i]; if (k < (int)P.size()) pal[i] = 0xff000000 | (P[k].peBlue << 16) | (P[k].peGreen << 8) | P[k].peRed; }
			}
			else
			{
				const RGBQUAD* q = (const RGBQUAD*)colors;
				for (int i = 0; i < ncol && i < 256; i++) pal[i] = 0xff000000 | (q[i].rgbBlue << 16) | (q[i].rgbGreen << 8) | q[i].rgbRed;
			}
		}
		if (b->bmiHeader.biCompression == BI_BITFIELDS) { const DWORD* m = (const DWORD*)colors; masks[0] = m[0]; masks[1] = m[1]; masks[2] = m[2]; }
		else if (bpp == 16) { masks[0] = 0x7c00; masks[1] = 0x03e0; masks[2] = 0x001f; }
		else { masks[0] = 0xff0000; masks[1] = 0xff00; masks[2] = 0xff; }
		for (int k = 0; k < 3; k++) { DWORD m = masks[k]; sh[k] = 0; bw[k] = 0; if (m) { while (!(m & 1)) { m >>= 1; sh[k]++; } while (m & 1) { m >>= 1; bw[k]++; } } }
	}
	inline uint32_t get(int x, int y) const
	{
		int row = topdown ? y : (h - 1 - y);
		const BYTE* p = bits + (size_t)row * stride;
		switch (bpp)
		{
		case 8: return pal[p[x]];
		case 4: return pal[(p[x >> 1] >> ((x & 1) ? 0 : 4)) & 15];
		case 1: return pal[(p[x >> 3] >> (7 - (x & 7))) & 1];
		case 24: { const BYTE* q = p + x * 3; return 0xff000000 | (q[0] << 16) | (q[1] << 8) | q[2]; }
		case 32: { const BYTE* q = p + x * 4; if (bi->bmiHeader.biCompression != BI_BITFIELDS) return 0xff000000 | (q[0] << 16) | (q[1] << 8) | q[2];
			DWORD v = *(const DWORD*)q; return 0xff000000 | (comp(v, 2) << 16) | (comp(v, 1) << 8) | comp(v, 0); }
		case 16: { DWORD v = *(const WORD*)(p + x * 2); return 0xff000000 | (comp(v, 2) << 16) | (comp(v, 1) << 8) | comp(v, 0); }
		}
		return 0xff000000;
	}
	inline uint32_t comp(DWORD v, int k) const { DWORD c = (v & masks[k]) >> sh[k]; if (bw[k] >= 8) return c >> (bw[k] - 8); return bw[k] ? c * 255 / ((1u << bw[k]) - 1) : 0; }
};
// note: comp() returns component order R=index0 when masks are (R,G,B)
static inline uint32_t rop_apply(uint32_t dst, uint32_t src, DWORD rop)
{
	switch (rop)
	{
	case SRCCOPY: return src;
	case SRCAND: return (dst & src) | 0xff000000;
	case SRCPAINT: return dst | src;
	case SRCINVERT: return ((dst ^ src) & 0xffffff) | 0xff000000;
	case NOTSRCCOPY: return (~src & 0xffffff) | 0xff000000;
	case BLACKNESS: return 0xff000000;
	case WHITENESS: return 0xffffffff;
	case DSTINVERT: return (~dst & 0xffffff) | 0xff000000;
	}
	return src;
}

int WINAPI StretchDIBits(HDC dc, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs,
	const VOID* bits, const BITMAPINFO* bi, UINT usage, DWORD rop)
{
	DCRec* d = dc_get(dc); if (!d || !bits || !bi) return 0;
	Surface* s = d->surface(); if (!s || !s->px) return 0;
	DibReader rd(bi, bits, usage, d);
	if (ws == 0 || hs == 0 || wd == 0 || hd == 0) return 0;
	bool flipx = (wd < 0) != (ws < 0), flipy = (hd < 0) != (hs < 0);
	if (wd < 0) { xd += wd; wd = -wd; } if (hd < 0) { yd += hd; hd = -hd; }
	if (ws < 0) { xs += ws; ws = -ws; } if (hs < 0) { ys += hs; hs = -hs; }
	// source y in DIB coords: ys counts from the bottom for bottom-up DIBs
	int x0 = std::max(xd, 0), x1 = std::min(xd + wd, s->w), y0 = std::max(yd, 0), y1 = std::min(yd + hd, s->h);
	for (int y = y0; y < y1; y++)
	{
		int ry = (int)(((long long)(y - yd) * hs) / hd); if (flipy) ry = hs - 1 - ry;
		int sy;
		if (rd.topdown) sy = ys + ry; else sy = rd.h - (ys + hs) + ry;	// convert bottom-up source origin to row from top
		if (sy < 0 || sy >= rd.h) continue;
		uint32_t* dp = s->px + y * s->w;
		for (int x = x0; x < x1; x++)
		{
			int rx = (int)(((long long)(x - xd) * ws) / wd); if (flipx) rx = ws - 1 - rx;
			int sx = xs + rx; if (sx < 0 || sx >= rd.w) continue;
			dp[x] = rop_apply(dp[x], rd.get(sx, sy), rop);
		}
	}
	mark(d, x0, y0, x1, y1);
	return hs;
}

static Surface* dc_surface_or_screen(DCRec* d) { return d ? d->surface() : nullptr; }

WINBOOL WINAPI BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
	DCRec* dd = dc_get(dst); if (!dd) return FALSE;
	Surface* ds = dd->surface(); if (!ds || !ds->px) return FALSE;
	DCRec* sd = dc_get(src);
	Surface* ss = dc_surface_or_screen(sd);
	for (int j = 0; j < h; j++)
	{
		int ty = y + j; if (ty < 0 || ty >= ds->h) continue;
		for (int i = 0; i < w; i++)
		{
			int tx = x + i; if (tx < 0 || tx >= ds->w) continue;
			uint32_t sp = 0xff000000;
			if (ss && ss->px) { int px = sx + i, py = sy + j; if (px >= 0 && py >= 0 && px < ss->w && py < ss->h) sp = ss->px[py * ss->w + px]; }
			uint32_t& dp = ds->px[ty * ds->w + tx];
			dp = rop_apply(dp, sp, rop);
		}
	}
	mark(dd, x, y, x + w, y + h);
	return TRUE;
}

int WINAPI GetDIBits(HDC dc, HBITMAP hb, UINT start, UINT lines, LPVOID bits, LPBITMAPINFO bi, UINT usage)
{
	GdiObj* b = gdi_get(hb, GDI_BITMAP); if (!b || !b->surf) return 0;
	Surface* s = b->surf;
	BITMAPINFOHEADER& h = bi->bmiHeader;
	if (!bits) { h.biWidth = s->w; h.biHeight = s->h; h.biPlanes = 1; if (!h.biBitCount) h.biBitCount = 32; h.biSizeImage = ((s->w * h.biBitCount + 31) / 32) * 4 * s->h; return s->h; }
	int w = s->w, H = s->h; bool topdown = h.biHeight < 0;
	int bpp = h.biBitCount, stride = ((w * bpp + 31) / 32) * 4;
	RGBQUAD* pal = (RGBQUAD*)((BYTE*)bi + h.biSize);
	if (bpp == 1) { pal[0] = {0, 0, 0, 0}; pal[1] = {255, 255, 255, 0}; }
	int n = 0;
	for (UINT l = 0; l < lines; l++)
	{
		int dibRow = start + l;	// row index in DIB order
		int sy = topdown ? dibRow : (H - 1 - dibRow);
		if (sy < 0 || sy >= H) continue;
		BYTE* row = (BYTE*)bits + (size_t)l * stride;
		const uint32_t* sp = s->px + sy * w;
		if (bpp == 32) for (int x = 0; x < w; x++) { uint32_t p = sp[x]; row[x * 4] = (p >> 16) & 255; row[x * 4 + 1] = (p >> 8) & 255; row[x * 4 + 2] = p & 255; row[x * 4 + 3] = 0; }
		else if (bpp == 24) for (int x = 0; x < w; x++) { uint32_t p = sp[x]; row[x * 3] = (p >> 16) & 255; row[x * 3 + 1] = (p >> 8) & 255; row[x * 3 + 2] = p & 255; }
		else if (bpp == 1)
		{
			memset(row, 0, stride);
			for (int x = 0; x < w; x++) { uint32_t p = sp[x]; int lum = (int)(p & 255) * 3 + (int)((p >> 8) & 255) * 6 + (int)((p >> 16) & 255); if (lum >= 1280) row[x >> 3] |= (BYTE)(0x80 >> (x & 7)); }
		}
		else if (bpp == 8)
		{
			// grey palette approximation
			for (int i = 0; i < 256; i++) pal[i] = {(BYTE)i, (BYTE)i, (BYTE)i, 0};
			for (int x = 0; x < w; x++) { uint32_t p = sp[x]; row[x] = (BYTE)(((p & 255) * 3 + ((p >> 8) & 255) * 6 + ((p >> 16) & 255)) / 10); }
		}
		n++;
	}
	return n;
}

// ------------------------------------------------------------------ text
EM_JS(int, jsg_text, (const char* font, const char* text, double sx, double angle, int measureOnly, int* out), {
	return PeterWin.renderText(UTF8ToString(font), UTF8ToString(text), sx, angle, measureOnly, out);
});

struct TextImg { int w = 0, h = 0, ascent = 0, ox = 0, oy = 0, advance = 0; uint8_t* alpha = nullptr; };
static double font_xscale(GdiObj* f, double em)
{
	if (!f || f->lf.lfWidth <= 0) return 1.0;
	// average character width of the scaled font should equal lfWidth
	int out[8] = {0};
	jsg_text(f->css.c_str(), "\x01avg", 1.0, 0, 1, out);	// special request: avg char width*100
	double avg = out[0] / 100.0;
	if (avg <= 0) return 1.0;
	return f->lf.lfWidth / avg;
}
static TextImg render_text(GdiObj* f, const std::string& utf8, bool measure)
{
	TextImg t; int out[8] = {0};
	double sx = font_xscale(f, 0);
	double ang = f ? f->lf.lfEscapement / 10.0 : 0;
	int ptr = jsg_text(f ? f->css.c_str() : "13px Arial", utf8.c_str(), sx, ang, measure ? 1 : 0, out);
	t.w = out[0]; t.h = out[1]; t.ascent = out[2]; t.ox = out[3]; t.oy = out[4]; t.advance = out[5];
	t.alpha = (uint8_t*)(uintptr_t)ptr;
	return t;
}

static void draw_text_at(DCRec* d, int x, int y, const char* s, int n, const RECT* clip, bool forceOpaque = false)
{
	Surface* surf = d->surface(); if (!surf || !surf->px) return;
	GdiObj* f = d->font;
	std::string u = ansi_to_utf8(s, n);
	TextImg t = render_text(f, u, false);
	// alignment (unrotated reference box of advance width x line height)
	int lineH = t.h ? t.h : 16;
	UINT ha = d->align & (TA_LEFT | TA_RIGHT | TA_CENTER), va = d->align & (TA_TOP | TA_BOTTOM | TA_BASELINE);
	int bx = x, by = y;
	if (ha == TA_RIGHT) bx -= t.advance; else if (ha == TA_CENTER) bx -= t.advance / 2;
	if (va == TA_BASELINE) by -= t.ascent; else if (va == TA_BOTTOM) by -= lineH;
	int cl = 0, ct = 0, cr = surf->w, cb = surf->h;
	if (clip) { cl = std::max(cl, (int)clip->left); ct = std::max(ct, (int)clip->top); cr = std::min(cr, (int)clip->right); cb = std::min(cb, (int)clip->bottom); }
	if (d->bkmode == OPAQUE || forceOpaque)
	{
		uint32_t bk = RGBA_FROM_COLORREF(d->bkcol);
		int l = std::max(bx, cl), r = std::min(bx + t.advance, cr), tp = std::max(by, ct), b = std::min(by + lineH, cb);
		if (f && f->lf.lfEscapement == 0)
			for (int yy = tp; yy < b; yy++) for (int xx = l; xx < r; xx++) surf->px[yy * surf->w + xx] = bk;
	}
	uint32_t tc = d->textcol;
	int tr = tc & 255, tg = (tc >> 8) & 255, tb = (tc >> 16) & 255;
	int ox = bx + t.ox, oy = by + t.oy;
	for (int j = 0; j < t.h; j++)
	{
		int yy = oy + j; if (yy < ct || yy >= cb) continue;
		for (int i = 0; i < t.w; i++)
		{
			int xx = ox + i; if (xx < cl || xx >= cr) continue;
			int a = t.alpha[j * t.w + i]; if (!a) continue;
			uint32_t& p = surf->px[yy * surf->w + xx];
			if (a >= 250) { p = 0xff000000 | (tb << 16) | (tg << 8) | tr; continue; }
			int pr = p & 255, pg = (p >> 8) & 255, pb = (p >> 16) & 255;
			pr += (tr - pr) * a / 255; pg += (tg - pg) * a / 255; pb += (tb - pb) * a / 255;
			p = 0xff000000 | (pb << 16) | (pg << 8) | pr;
		}
	}
	mark(d, ox, oy, ox + t.w, oy + t.h);
	if (d->bkmode == OPAQUE) mark(d, bx, by, bx + t.advance, by + lineH);
	free(t.alpha);
}

WINBOOL WINAPI TextOutA(HDC dc, int x, int y, LPCSTR s, int n)
{
	DCRec* d = dc_get(dc); if (!d) return FALSE;
	draw_text_at(d, x, y, s, n, nullptr);
	return TRUE;
}
WINBOOL WINAPI GetTextExtentPoint32A(HDC dc, LPCSTR s, int n, LPSIZE sz)
{
	DCRec* d = dc_get(dc); if (!d) return FALSE;
	TextImg t = render_text(d->font, ansi_to_utf8(s, n), true);
	sz->cx = t.advance; sz->cy = t.h;
	return TRUE;
}
int WINAPI DrawTextA(HDC dc, LPCSTR s, int n, LPRECT r, UINT fmt)
{
	DCRec* d = dc_get(dc); if (!d) return 0;
	if (n < 0) n = (int)strlen(s);
	// split into lines (no word wrap except DT_WORDBREAK simple implementation)
	std::vector<std::string> lines;
	std::string cur;
	for (int i = 0; i < n; i++)
	{
		char c = s[i];
		if (c == '\r') continue;
		if (c == '\n' && !(fmt & DT_SINGLELINE)) { lines.push_back(cur); cur.clear(); continue; }
		if (c == '&' && !(fmt & DT_NOPREFIX)) { if (i + 1 < n && s[i + 1] == '&') { cur += '&'; i++; } continue; }
		cur += c;
	}
	lines.push_back(cur);
	if (fmt & DT_WORDBREAK)
	{
		std::vector<std::string> wrapped;
		int maxw = r->right - r->left;
		for (auto& l : lines)
		{
			std::string line;
			size_t i = 0;
			while (i <= l.size())
			{
				size_t j = l.find(' ', i); if (j == std::string::npos) j = l.size();
				std::string word = l.substr(i, j - i);
				std::string test = line.empty() ? word : line + " " + word;
				SIZE sz; GetTextExtentPoint32A(dc, test.c_str(), (int)test.size(), &sz);
				if (sz.cx > maxw && !line.empty()) { wrapped.push_back(line); line = word; }
				else line = test;
				i = j + 1;
			}
			wrapped.push_back(line);
		}
		lines = wrapped;
	}
	SIZE one; GetTextExtentPoint32A(dc, "Xg", 2, &one);
	int lh = one.cy ? one.cy : 16;
	int total = lh * (int)lines.size();
	if (fmt & DT_CALCRECT)
	{
		int mw = 0; for (auto& l : lines) { SIZE sz; GetTextExtentPoint32A(dc, l.c_str(), (int)l.size(), &sz); mw = std::max(mw, (int)sz.cx); }
		r->right = r->left + mw; r->bottom = r->top + total;
		return total;
	}
	int y = r->top;
	if (fmt & DT_SINGLELINE) { if (fmt & DT_VCENTER) y = (r->top + r->bottom - lh) / 2; else if (fmt & DT_BOTTOM) y = r->bottom - lh; }
	UINT olda = d->align; d->align = TA_LEFT | TA_TOP;
	for (auto& l : lines)
	{
		SIZE sz; GetTextExtentPoint32A(dc, l.c_str(), (int)l.size(), &sz);
		int x = r->left;
		if (fmt & DT_CENTER) x = (r->left + r->right - sz.cx) / 2; else if (fmt & DT_RIGHT) x = r->right - sz.cx;
		draw_text_at(d, x, y, l.c_str(), (int)l.size(), (fmt & DT_NOCLIP) ? nullptr : r);
		y += lh;
	}
	d->align = olda;
	return total;
}
int WINAPI EnumFontFamiliesA(HDC dc, LPCSTR fam, FONTENUMPROCA proc, LPARAM lp)
{
	static const char* fams[] = {"Arial", "Courier New", "Times New Roman", "Tahoma", "Verdana"};
	int r = 1;
	for (auto f : fams)
	{
		if (fam && strcasecmp(fam, f) != 0) continue;
		ENUMLOGFONTA elf{}; NEWTEXTMETRICA tm{};
		strcpy(elf.elfLogFont.lfFaceName, f); strcpy((char*)elf.elfFullName, f);
		elf.elfLogFont.lfCharSet = ANSI_CHARSET;
		r = proc((LOGFONTA*)&elf, (TEXTMETRICA*)&tm, TRUETYPE_FONTTYPE, lp);
		if (!r) break;
	}
	return r;
}

// ------------------------------------------------------------------ printing (unsupported)
int WINAPI StartDocA(HDC dc, const DOCINFOA* di) { return SP_ERROR; }
int WINAPI EndDoc(HDC dc) { return 0; }
int WINAPI StartPage(HDC dc) { return 0; }
int WINAPI EndPage(HDC dc) { return 0; }

} // extern "C"

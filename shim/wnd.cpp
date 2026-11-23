// Win32 shim: windows, messages, timers, input, cursors and DOM synchronisation
#include "shim.h"
#include <algorithm>
#include <set>

int g_screenW = 1024, g_screenH = 768;
Wnd* g_focus = nullptr;
static Wnd* g_active = nullptr;
static int g_nextId = 1;
static std::map<int, Wnd*> g_byId;
static std::vector<Wnd*> g_top;	// top-level windows in creation order

struct ClassRec { std::string name; WNDPROC proc; UINT style; HCURSOR cursor; HBRUSH brush; int extra; };
static std::map<std::string, ClassRec> g_classes;
static std::string lc(const char* s) { std::string r = s ? s : ""; for (auto& c : r) c = (char)tolower((unsigned char)c); return r; }

static std::deque<MSG> g_queue;
static bool g_quit = false;
static int g_quitCode = 0;
static std::map<DWORD, int> g_keyChar;	// msg.time -> translated character
static DWORD g_msgSeq = 1;

struct Timer { HWND hwnd; UINT_PTR id; UINT ms; double next; TIMERPROC proc; };
static std::vector<Timer> g_timers;

static BYTE g_keys[256];		// bit 7 = down, bit 0 = toggled
static POINT g_cursor = {0, 0};
static int g_showCursor = 0;
static HCURSOR g_curCursor = 0;

// ------------------------------------------------------------------ JS side (DOM)
EM_JS(void, jsw_create, (int id, int parent, int kind, int flags, const char* text), { PeterWin.create(id, parent, kind, flags, UTF8ToString(text)); });
EM_JS(void, jsw_destroy, (int id), { PeterWin.destroy(id); });
EM_JS(void, jsw_rect, (int id, int x, int y, int w, int h, int cx, int cy, int cw, int ch), { PeterWin.rect(id, x, y, w, h, cx, cy, cw, ch); });
EM_JS(void, jsw_show, (int id, int vis), { PeterWin.show(id, vis); });
EM_JS(void, jsw_enable, (int id, int en), { PeterWin.enable(id, en); });
EM_JS(void, jsw_text, (int id, const char* t), { PeterWin.text(id, UTF8ToString(t)); });
EM_JS(void, jsw_check, (int id, int st), { PeterWin.check(id, st); });
EM_JS(void, jsw_items, (int id, const char* t, int n, int sel), { PeterWin.items(id, n ? UTF8ToString(t).split('\n') : [], sel); });
EM_JS(void, jsw_sel, (int id, int sel), { PeterWin.sel(id, sel); });
EM_JS(void, jsw_range, (int id, int mn, int mx, int pos), { PeterWin.range(id, mn, mx, pos); });
EM_JS(void, jsw_focus, (int id), { PeterWin.focus(id); });
EM_JS(void, jsw_front, (int id), { PeterWin.front(id); });
EM_JS(void, jsw_cursor, (const char* css), { PeterWin.cursor(UTF8ToString(css)); });
EM_JS(void, jsw_font, (int id, const char* css), { PeterWin.font(id, UTF8ToString(css)); });
EM_JS(void, jsw_readonly, (int id, int ro), { PeterWin.readonly(id, ro); });
EM_JS(void, jsw_screen, (int w, int h), { PeterWin.screen(w, h); });
EM_JS(void, jsw_pointerlock, (int on), { PeterWin.pointerLock(on); });
EM_JS(void, jsw_colors, (int id, int bg, int fg), { PeterWin.colors(id, bg, fg); });

// ------------------------------------------------------------------ helpers
Wnd* wnd_get(HWND h) { return checked<Wnd>(h, 0x574e4430); }
static HWND H(Wnd* w) { return (HWND)w; }

static void nc_metrics(DWORD style, DWORD exstyle, bool child, int& bx, int& by, int& cap)
{
	bx = by = cap = 0;
	if (!child || (style & WS_CAPTION) == WS_CAPTION)
	{
		if (style & WS_THICKFRAME) bx = by = 4;
		else if ((style & WS_CAPTION) == WS_CAPTION || (style & WS_DLGFRAME)) bx = by = 3;
		else if (style & WS_BORDER) bx = by = 1;
		if ((style & WS_CAPTION) == WS_CAPTION) cap = (exstyle & WS_EX_TOOLWINDOW) ? 15 : 19;
	}
	else if (style & WS_BORDER) bx = by = 1;
	if (exstyle & WS_EX_CLIENTEDGE) { bx += 2; by += 2; }
}
RECT Wnd::clientRect() const
{
	int bx, by, cap; nc_metrics(style, exstyle, (style & WS_CHILD) != 0, bx, by, cap);
	if (kind != CK_NONE && kind != CK_STATIC) { bx = by = cap = 0; }
	RECT r; r.left = bx; r.top = by + cap;
	r.right = (rc.right - rc.left) - bx; r.bottom = (rc.bottom - rc.top) - by;
	if (r.right < r.left) r.right = r.left;
	if (r.bottom < r.top) r.bottom = r.top;
	return r;
}
POINT wnd_client_origin(Wnd* w)
{
	POINT p = {0, 0};
	for (Wnd* x = w; x; x = x->parent)
	{
		RECT c = x->clientRect();
		p.x += x->rc.left + c.left; p.y += x->rc.top + c.top;
		if (x->parent) { /* rc is in parent's client coords */ }
	}
	return p;
}

void wnd_sync_dom(Wnd* w)
{
	RECT c = w->clientRect();
	jsw_rect(w->id, w->rc.left, w->rc.top, w->rc.right - w->rc.left, w->rc.bottom - w->rc.top,
		c.left, c.top, c.right - c.left, c.bottom - c.top);
	int cw = c.right - c.left, ch = c.bottom - c.top;
	if (w->hasCanvas && (w->surf.w != cw || w->surf.h != ch)) { w->surf.resize(cw, ch); }
	if (w->glCanvas) gl_resize(w);
}

static CtrlKind kind_of(const std::string& cls, DWORD style)
{
	if (cls == "button")
	{
		switch (style & 0x0f)
		{
		case BS_CHECKBOX: case BS_AUTOCHECKBOX: case BS_3STATE: case BS_AUTO3STATE: return (style & BS_PUSHLIKE) ? CK_BUTTON : CK_CHECK;
		case BS_RADIOBUTTON: case BS_AUTORADIOBUTTON: return (style & BS_PUSHLIKE) ? CK_BUTTON : CK_RADIO;
		case BS_GROUPBOX: return CK_GROUP;
		default: return CK_BUTTON;
		}
	}
	if (cls == "edit") return (style & ES_MULTILINE) ? CK_MEMO : CK_EDIT;
	if (cls.rfind("richedit", 0) == 0) return (style & ES_MULTILINE) ? CK_MEMO : CK_EDIT;
	if (cls == "listbox") return CK_LISTBOX;
	if (cls == "combobox") return ((style & 3) == CBS_DROPDOWNLIST) ? CK_COMBO : CK_COMBOEDIT;
	if (cls == "static") return CK_STATIC;
	if (cls == "scrollbar") return CK_SCROLL;
	if (cls == "msctls_trackbar32") return CK_TRACK;
	if (cls == "msctls_progress32") return CK_PROGRESS;
	if (cls == "msctls_statusbar32") return CK_STATUS;
	if (cls == "systabcontrol32") return CK_TAB;
	if (cls == "syslistview32") return CK_LISTVIEW;
	return CK_NONE;
}

void ensure_canvas(Wnd* w);
LRESULT wnd_send(Wnd* w, UINT msg, WPARAM wp, LPARAM lp)
{
	if (!w || w->destroyed || !w->proc) return 0;
	return w->proc(H(w), msg, wp, lp);
}

void post_msg(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	MSG m{}; m.hwnd = h; m.message = msg; m.wParam = wp; m.lParam = lp; m.time = g_msgSeq++;
	m.pt = g_cursor;
	g_queue.push_back(m);
}

static void notify_parent(Wnd* w, WORD code)
{
	if (w->parent) wnd_send(w->parent, WM_COMMAND, MAKEWPARAM((WORD)(uintptr_t)w->menu, code), (LPARAM)H(w));
}

// ask the parent for the control's colours (WM_CTLCOLORxxx), as Windows controls do when painting
static void apply_ctlcolor(Wnd* w)
{
	if (!w || w->kind == CK_NONE || !w->parent) return;
	UINT msg;
	switch (w->kind)
	{
	case CK_EDIT: case CK_MEMO: case CK_COMBOEDIT: msg = WM_CTLCOLOREDIT; break;
	case CK_LISTBOX: case CK_COMBO: msg = WM_CTLCOLORLISTBOX; break;
	case CK_BUTTON: msg = WM_CTLCOLORBTN; break;
	case CK_SCROLL: msg = WM_CTLCOLORSCROLLBAR; break;
	default: msg = WM_CTLCOLORSTATIC; break;
	}
	HDC dc = GetDC((HWND)w);
	DCRec* d = dc_get(dc);
	if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) { d->bkcol = GetSysColor(COLOR_WINDOW); d->textcol = GetSysColor(COLOR_WINDOWTEXT); }
	else { d->bkcol = GetSysColor(COLOR_BTNFACE); d->textcol = GetSysColor(COLOR_BTNTEXT); }
	LRESULT br = wnd_send(w->parent, msg, (WPARAM)dc, (LPARAM)w);
	int bg = -1;
	GdiObj* b = gdi_get((HGDIOBJ)br, GDI_BRUSH);
	if (b && !b->null_) bg = (int)(b->color & 0xffffff);
	else if (br && (uintptr_t)br <= 31) bg = (int)GetSysColor((int)(uintptr_t)br - 1);
	int fg = (int)(d->textcol & 0xffffff);
	ReleaseDC((HWND)w, dc);
	jsw_colors(w->id, bg, fg);
}

static std::string ctrl_items_join(Wnd* w)
{
	std::string s;
	for (size_t i = 0; i < w->items.size(); i++) { if (i) s += '\n'; s += ansi_to_utf8(w->items[i].c_str()); }
	return s;
}
static void ctrl_sync_items(Wnd* w) { jsw_items(w->id, ctrl_items_join(w).c_str(), (int)w->items.size(), w->cursel); }

// ------------------------------------------------------------------ built-in control procedure
static LRESULT CALLBACK ctrl_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	Wnd* w = wnd_get(h);
	if (!w) return 0;
	if (msg >= WM_USER && msg < 0x1000)
	{
		// WM_USER-based messages overlap between control classes: dispatch by kind
		if (w->kind == CK_TRACK)
			switch (msg)
			{
			case TBM_SETRANGE: w->rmin = LOWORD(lp); w->rmax = HIWORD(lp); jsw_range(w->id, w->rmin, w->rmax, w->pos); return 0;
			case TBM_SETPOS: w->pos = (int)lp; jsw_range(w->id, w->rmin, w->rmax, w->pos); return 0;
			case TBM_GETPOS: return w->pos;
			default: return 0;
			}
		if (w->kind == CK_PROGRESS)
			switch (msg)
			{
			case PBM_SETRANGE: w->rmin = LOWORD(lp); w->rmax = HIWORD(lp); jsw_range(w->id, w->rmin, w->rmax, w->pos); return 0;
			case PBM_SETPOS: { int o = w->pos; w->pos = (int)wp; jsw_range(w->id, w->rmin, w->rmax, w->pos); return o; }
			case PBM_GETPOS: return w->pos;
			default: return 0;
			}
		if (w->kind == CK_STATUS)
			switch (msg)
			{
			case SB_SETTEXTA: w->text = (const char*)lp; jsw_text(w->id, ansi_to_utf8(w->text.c_str()).c_str()); return TRUE;
			default: return TRUE;
			}
	}
	switch (msg)
	{
	// --- buttons
	case BM_GETCHECK: return w->check;
	case BM_SETCHECK: w->check = (int)wp; jsw_check(w->id, w->check); return 0;
	case BM_GETSTATE: return w->check ? BST_CHECKED : 0;
	case BM_CLICK: notify_parent(w, BN_CLICKED); return 0;
	case BM_SETSTYLE: w->style = (w->style & ~0xffff) | (WORD)wp; return 0;
	case BM_SETIMAGE: return 0;

	// --- edit
	case EM_SETLIMITTEXT: case EM_EXLIMITTEXT: case EM_SETEVENTMASK: case EM_SETTEXTMODE: case EM_SETPARAFORMAT:
	case EM_SCROLLCARET: case EM_LINESCROLL: case EM_SETMARGINS: return 0;
	case EM_SETREADONLY: w->readonly = wp != 0; jsw_readonly(w->id, w->readonly); return TRUE;
	case EM_GETSEL:
		if (wp) *(DWORD*)wp = w->selstart;
		if (lp) *(DWORD*)lp = w->selend;
		return MAKELONG(w->selstart, w->selend);
	case EM_SETSEL: w->selstart = (int)wp; w->selend = (int)lp; return 0;
	case EM_EXGETSEL: { CHARRANGE* cr = (CHARRANGE*)lp; cr->cpMin = w->selstart; cr->cpMax = w->selend; return 0; }
	case EM_EXSETSEL: { CHARRANGE* cr = (CHARRANGE*)lp; w->selstart = cr->cpMin; w->selend = cr->cpMax; return 0; }
	case EM_LINEFROMCHAR:
	{
		int pos = (int)wp; if (pos < 0) pos = w->selstart;
		int line = 0; for (int i = 0; i < pos && i < (int)w->text.size(); i++) if (w->text[i] == '\n') line++;
		return line;
	}
	case EM_LINEINDEX:
	{
		int line = (int)wp; if (line < 0) return 0;
		int idx = 0; for (int l = 0; l < line; l++) { size_t p = w->text.find('\n', idx); if (p == std::string::npos) return -1; idx = (int)p + 1; }
		return idx;
	}
	case EM_GETLINECOUNT: { int n = 1; for (char c : w->text) if (c == '\n') n++; return n; }
	case EM_GETFIRSTVISIBLELINE: return 0;
	case EM_REPLACESEL:
	{
		std::string s = (const char*)lp;
		int a = std::min(w->selstart, (int)w->text.size()), b = std::min(w->selend, (int)w->text.size());
		w->text = w->text.substr(0, a) + s + w->text.substr(b);
		w->selstart = w->selend = a + (int)s.size();
		jsw_text(w->id, ansi_to_utf8(w->text.c_str()).c_str());
		return 0;
	}
	case EM_STREAMIN: case EM_STREAMOUT: case EM_FINDTEXT: case EM_FINDTEXTEX: return -1;

	// --- listbox
	case LB_ADDSTRING: case CB_ADDSTRING: w->items.push_back((const char*)lp); ctrl_sync_items(w); return (LRESULT)w->items.size() - 1;
	case LB_INSERTSTRING: case CB_INSERTSTRING:
	{
		int i = (int)wp; if (i < 0 || i > (int)w->items.size()) i = (int)w->items.size();
		w->items.insert(w->items.begin() + i, (const char*)lp); ctrl_sync_items(w); return i;
	}
	case LB_DELETESTRING: case CB_DELETESTRING:
		if ((int)wp < (int)w->items.size()) w->items.erase(w->items.begin() + wp);
		if (w->cursel >= (int)w->items.size()) w->cursel = -1;
		ctrl_sync_items(w); return (LRESULT)w->items.size();
	case LB_RESETCONTENT: case CB_RESETCONTENT: w->items.clear(); w->cursel = -1; ctrl_sync_items(w); return 0;
	case LB_GETCOUNT: case CB_GETCOUNT: return (LRESULT)w->items.size();
	case LB_GETCURSEL: case CB_GETCURSEL: return w->cursel;
	case LB_SETCURSEL: case CB_SETCURSEL:
		w->cursel = ((int)wp >= 0 && (int)wp < (int)w->items.size()) ? (int)wp : -1;
		if (w->kind == CK_COMBO || w->kind == CK_COMBOEDIT) w->text = w->cursel >= 0 ? w->items[w->cursel] : "";
		jsw_sel(w->id, w->cursel); return w->cursel;
	case LB_GETTEXTLEN: case CB_GETLBTEXTLEN: return ((int)wp < (int)w->items.size()) ? (LRESULT)w->items[wp].size() : LB_ERR;
	case LB_GETTEXT: case CB_GETLBTEXT:
		if ((int)wp >= (int)w->items.size()) return LB_ERR;
		strcpy((char*)lp, w->items[wp].c_str()); return (LRESULT)w->items[wp].size();
	case LB_FINDSTRING: case CB_FINDSTRING: case LB_FINDSTRINGEXACT: case CB_FINDSTRINGEXACT:
	case LB_SELECTSTRING: case CB_SELECTSTRING:
	{
		std::string s = lc((const char*)lp);
		bool exact = (msg == LB_FINDSTRINGEXACT || msg == CB_FINDSTRINGEXACT);
		int n = (int)w->items.size(), start = (int)wp;
		for (int k = 1; k <= n; k++)
		{
			int i = (start + k) % n; if (start < 0) i = k - 1;
			std::string it = lc(w->items[i].c_str());
			if (exact ? (it == s) : (it.compare(0, s.size(), s) == 0))
			{
				if (msg == LB_SELECTSTRING || msg == CB_SELECTSTRING)
				{
					w->cursel = i;
					if (w->kind != CK_LISTBOX) w->text = w->items[i];
					jsw_sel(w->id, i);
				}
				return i;
			}
		}
		return LB_ERR;
	}
	case LB_SETTOPINDEX: case LB_GETTOPINDEX: case LB_SETITEMHEIGHT: case CB_SETITEMHEIGHT: case CB_SHOWDROPDOWN:
	case CB_SETEXTENDEDUI: case CB_LIMITTEXT: return 0;
	case LB_GETITEMHEIGHT: case CB_GETITEMHEIGHT: return 16;


	// --- tabs (minimal)
	case TCM_INSERTITEMA: { TCITEMA* it = (TCITEMA*)lp; int i = std::min((int)wp, (int)w->items.size()); w->items.insert(w->items.begin() + i, (it->mask & TCIF_TEXT) && it->pszText ? it->pszText : ""); if (w->cursel < 0) w->cursel = 0; ctrl_sync_items(w); return i; }
	case TCM_GETITEMCOUNT: return (LRESULT)w->items.size();
	case TCM_GETCURSEL: return w->cursel;
	case TCM_SETCURSEL: { int o = w->cursel; w->cursel = (int)wp; jsw_sel(w->id, w->cursel); return o; }
	case TCM_DELETEALLITEMS: w->items.clear(); w->cursel = -1; ctrl_sync_items(w); return TRUE;
	case TCM_ADJUSTRECT: if (!wp) { RECT* r = (RECT*)lp; r->top += 24; r->left += 2; r->right -= 2; r->bottom -= 2; } return 0;


	// --- static images
	case STM_SETIMAGE: case STM_SETICON: return 0;

	// --- listview (not supported in detail)
	case LVM_INSERTITEMA: case LVM_INSERTCOLUMNA: return 0;
	case LVM_DELETEALLITEMS: case LVM_SETITEMSTATE: case LVM_ENSUREVISIBLE: case LVM_SETTEXTCOLOR:
	case LVM_SETTEXTBKCOLOR: case LVM_SETBKCOLOR: case LVM_SETCOLUMNWIDTH: case LVM_SETCOLUMNA: case LVM_SETITEMTEXTA: return TRUE;
	case LVM_GETITEMSTATE: return 0;
	case LVM_GETCOLUMNA: case LVM_GETITEMRECT: case LVM_GETSUBITEMRECT: return FALSE;

	case WM_SIZE:
		if (w->kind == CK_STATUS && w->parent)
		{
			// status bars dock themselves to the bottom of the parent when it sends WM_SIZE
			RECT c = w->parent->clientRect();
			int cw = c.right - c.left, ch = c.bottom - c.top;
			SetWindowPos(h, NULL, 0, ch - 20, cw, 20, SWP_NOZORDER);
			return 0;
		}
		break;
	case WM_SETFONT: w->font = (HFONT)wp; { GdiObj* f = gdi_get((HGDIOBJ)wp, GDI_FONT); if (f) jsw_font(w->id, f->css.c_str()); } return 0;
	case WM_GETFONT: return (LRESULT)w->font;
	case WM_GETDLGCODE: return DLGC_WANTALLKEYS;

	case WM_PAINT:
		if (w->kind == CK_STATIC && (w->style & SS_TYPEMASK) == SS_OWNERDRAW)
		{
			PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
			DRAWITEMSTRUCT dis{}; dis.CtlType = ODT_STATIC; dis.CtlID = (UINT)(uintptr_t)w->menu; dis.itemAction = ODA_DRAWENTIRE;
			dis.hwndItem = h; dis.hDC = dc; GetClientRect(h, &dis.rcItem);
			if (w->parent) wnd_send(w->parent, WM_DRAWITEM, (WPARAM)dis.CtlID, (LPARAM)&dis);
			EndPaint(h, &ps);
			return 0;
		}
		break;
	}
	return DefWindowProcA(h, msg, wp, lp);
}

static void register_builtin()
{
	const char* names[] = {"button", "edit", "listbox", "combobox", "static", "scrollbar", "richedit", "richedit20a",
		"msctls_trackbar32", "msctls_progress32", "msctls_statusbar32", "systabcontrol32", "syslistview32"};
	for (auto n : names) g_classes[n] = ClassRec{n, ctrl_proc, 0, 0, 0, 0};
}

// ------------------------------------------------------------------ exported entry points from JS
static Wnd* top_of(Wnd* w) { while (w && w->parent && (w->style & WS_CHILD)) w = w->parent; return w; }

static void set_focus_internal(Wnd* w, bool fromDom)
{
	if (w == g_focus) return;
	Wnd* old = g_focus;
	g_focus = w;
	if (old && !old->destroyed) wnd_send(old, WM_KILLFOCUS, (WPARAM)H(w), 0);
	if (w)
	{
		wnd_send(w, WM_SETFOCUS, (WPARAM)H(old), 0);
		if (w->kind == CK_EDIT || w->kind == CK_MEMO || w->kind == CK_COMBOEDIT) notify_parent(w, EN_SETFOCUS);
		if (!fromDom) jsw_focus(w->id);
	}
}

extern "C" {

EMSCRIPTEN_KEEPALIVE void shim_on_mouse(int id, int msg, int x, int y, int keys)
{
	g_cursor.x = x; g_cursor.y = y;
	Wnd* w = g_byId.count(id) ? g_byId[id] : nullptr;
	if (msg == 0) return;	// pure cursor tracking
	if (!w || w->destroyed) return;
	if (msg == WM_MOUSEMOVE)
	{
		LRESULT r = wnd_send(w, WM_SETCURSOR, (WPARAM)H(w), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
		(void)r;
	}
	if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN)
	{
		Wnd* t = top_of(w);
		if (t && t != g_active) { g_active = t; jsw_front(t->id); }
		if (w->kind == CK_NONE && g_focus && g_focus->kind != CK_NONE && top_of(g_focus) == t) {} // keep control focus
	}
	POINT o = wnd_client_origin(w);
	WPARAM wp = 0;
	if (keys & 1) wp |= MK_LBUTTON; if (keys & 2) wp |= MK_RBUTTON; if (keys & 4) wp |= MK_MBUTTON;
	if (g_keys[VK_SHIFT] & 0x80) wp |= MK_SHIFT; if (g_keys[VK_CONTROL] & 0x80) wp |= MK_CONTROL;
	post_msg(H(w), msg, wp, MAKELPARAM((short)(x - o.x), (short)(y - o.y)));
}

EMSCRIPTEN_KEEPALIVE void shim_on_nc(int id, int what)
{
	Wnd* w = g_byId.count(id) ? g_byId[id] : nullptr;
	if (!w) return;
	if (what == 1) post_msg(H(w), WM_SYSCOMMAND, SC_CLOSE, 0);
	if (what == 2) { Wnd* t = top_of(w); if (t && t != g_active) { g_active = t; jsw_front(t->id); wnd_send(t, WM_ACTIVATE, WA_CLICKACTIVE, 0); } }
}

EMSCRIPTEN_KEEPALIVE void shim_on_move(int id, int x, int y)
{
	Wnd* w = g_byId.count(id) ? g_byId[id] : nullptr;
	if (!w) return;
	SetWindowPos(H(w), NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

EMSCRIPTEN_KEEPALIVE void shim_on_key(int vk, int down, int repeat, int scan, int ch, int targetId)
{
	if (vk <= 0 || vk > 255) return;
	if (down) { if (!(g_keys[vk] & 0x80)) g_keys[vk] ^= 1; g_keys[vk] |= 0x80; }
	else g_keys[vk] &= ~0x80;
	// generic modifiers
	if (vk == VK_LSHIFT || vk == VK_RSHIFT) { g_keys[VK_SHIFT] = (g_keys[VK_LSHIFT] | g_keys[VK_RSHIFT]) & 0x80; }
	if (vk == VK_LCONTROL || vk == VK_RCONTROL) { g_keys[VK_CONTROL] = (g_keys[VK_LCONTROL] | g_keys[VK_RCONTROL]) & 0x80; }
	if (vk == VK_LMENU || vk == VK_RMENU) { g_keys[VK_MENU] = (g_keys[VK_LMENU] | g_keys[VK_RMENU]) & 0x80; }
	Wnd* t = (targetId && g_byId.count(targetId)) ? g_byId[targetId] : nullptr;
	if (!t) t = g_focus ? g_focus : g_active;
	if (!t && !g_top.empty()) t = g_top.front();
	if (!t) return;
	// map generic VK for the message (Windows sends VK_SHIFT etc.)
	int mvk = vk;
	bool ext = false;
	if (vk == VK_LSHIFT || vk == VK_RSHIFT) mvk = VK_SHIFT;
	if (vk == VK_LCONTROL || vk == VK_RCONTROL) { mvk = VK_CONTROL; ext = vk == VK_RCONTROL; }
	if (vk == VK_LMENU || vk == VK_RMENU) { mvk = VK_MENU; ext = vk == VK_RMENU; }
	if ((vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE || vk == VK_DIVIDE || vk == VK_NUMLOCK) ext = true;
	bool alt = (g_keys[VK_MENU] & 0x80) != 0;
	UINT msg = down ? ((alt || mvk == VK_MENU || mvk == VK_F10) ? WM_SYSKEYDOWN : WM_KEYDOWN) : ((alt || mvk == VK_MENU || mvk == VK_F10) ? WM_SYSKEYUP : WM_KEYUP);
	LPARAM lp = 1 | ((scan & 0xff) << 16) | (ext ? (1 << 24) : 0) | (alt ? (1 << 29) : 0) | (repeat ? (1 << 30) : 0) | (down ? 0 : (3u << 30));
	post_msg(H(t), msg, mvk, lp);
	if (down && ch > 0) g_keyChar[g_queue.back().time] = ch;
}

EMSCRIPTEN_KEEPALIVE void shim_on_blur()
{
	memset(g_keys, 0, sizeof g_keys);
}

// a DOM control was activated by the user
EMSCRIPTEN_KEEPALIVE void shim_on_ctrl(int id, int what, int value, const char* text)
{
	Wnd* w = g_byId.count(id) ? g_byId[id] : nullptr;
	if (!w || w->destroyed) return;
	switch (what)
	{
	case 1:	// click
		if (w->kind == CK_CHECK)
		{
			DWORD bs = w->style & 0x0f;
			if (bs == BS_AUTOCHECKBOX) w->check = w->check ? 0 : 1;
			else if (bs == BS_AUTO3STATE) w->check = (w->check + 1) % 3;
			jsw_check(w->id, w->check);
		}
		else if (w->kind == CK_RADIO)
		{
			if ((w->style & 0x0f) == BS_AUTORADIOBUTTON && w->parent)
			{
				// uncheck the other radios of the same group
				auto& sib = w->parent->children;
				int i = (int)(std::find(sib.begin(), sib.end(), w) - sib.begin());
				int a = i; while (a > 0 && !(sib[a]->style & WS_GROUP)) a--;
				for (int k = a; k < (int)sib.size(); k++)
				{
					if (k > a && (sib[k]->style & WS_GROUP)) break;
					if (sib[k]->kind == CK_RADIO && sib[k] != w && sib[k]->check) { sib[k]->check = 0; jsw_check(sib[k]->id, 0); }
				}
				w->check = 1; jsw_check(w->id, 1);
			}
		}
		set_focus_internal(w, true);
		notify_parent(w, BN_CLICKED);
		break;
	case 2:	// text changed
		w->text = utf8_to_ansi(text);
		w->selstart = w->selend = (int)w->text.size();
		if (w->kind == CK_COMBOEDIT) notify_parent(w, CBN_EDITCHANGE);
		else notify_parent(w, EN_CHANGE);
		break;
	case 3:	// selection changed
		w->cursel = value;
		if (w->kind == CK_COMBO || w->kind == CK_COMBOEDIT) { if (value >= 0 && value < (int)w->items.size()) w->text = w->items[value]; notify_parent(w, CBN_SELCHANGE); }
		else if (w->kind == CK_TAB)
		{
			NMHDR nm{}; nm.hwndFrom = H(w); nm.idFrom = (UINT_PTR)w->menu; nm.code = TCN_SELCHANGE;
			if (w->parent) wnd_send(w->parent, WM_NOTIFY, (WPARAM)w->menu, (LPARAM)&nm);
		}
		else notify_parent(w, LBN_SELCHANGE);
		break;
	case 4:	// double click on list
		notify_parent(w, LBN_DBLCLK);
		break;
	case 5:	// range position changed (trackbar/scrollbar)
		w->pos = value;
		if (w->parent)
		{
			bool vert = (w->kind == CK_TRACK) ? (w->style & TBS_VERT) != 0 : (w->style & SBS_VERT) != 0;
			wnd_send(w->parent, vert ? WM_VSCROLL : WM_HSCROLL, MAKEWPARAM(w->kind == CK_TRACK ? TB_THUMBTRACK : SB_THUMBTRACK, value), (LPARAM)H(w));
		}
		break;
	case 6:	// focus gained
		set_focus_internal(w, true);
		break;
	}
}

} // extern "C"

// ------------------------------------------------------------------ Win32 API
extern "C" {

ATOM WINAPI RegisterClassA(const WNDCLASSA* wc)
{
	ClassRec c{wc->lpszClassName, wc->lpfnWndProc, wc->style, wc->hCursor, wc->hbrBackground, wc->cbWndExtra};
	g_classes[lc(wc->lpszClassName)] = c;
	return (ATOM)g_classes.size();
}

HWND WINAPI CreateWindowExA(DWORD exstyle, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
	HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
	static bool init = false; if (!init) { init = true; register_builtin(); }
	std::string cn = lc(cls);
	auto it = g_classes.find(cn);
	if (it == g_classes.end()) { SHIM_LOG("[shim] CreateWindow: unknown class %s", cls); return NULL; }
	Wnd* wn = new Wnd;
	wn->id = g_nextId++;
	g_byId[wn->id] = wn;
	wn->cls = cn; wn->proc = it->second.proc; wn->clsCursor = it->second.cursor; wn->clsBrush = it->second.brush; wn->clsStyle = it->second.style;
	wn->style = style; wn->exstyle = exstyle;
	wn->parent = wnd_get(parent);
	wn->menu = menu;
	wn->text = name ? name : "";
	wn->kind = kind_of(cn, style);
	if (wn->kind == CK_TRACK) { wn->rmin = 0; wn->rmax = 100; }
	if (wn->kind == CK_SCROLL) { wn->rmin = 0; wn->rmax = 100; }
	if (x == (int)CW_USEDEFAULT) { x = 40; y = 40; }
	if (w == (int)CW_USEDEFAULT) { w = 640; h = 480; }
	wn->rc = {x, y, x + w, y + h};
	if (wn->parent && (style & WS_CHILD)) wn->parent->children.push_back(wn);
	else { wn->style &= ~WS_CHILD; g_top.push_back(wn); }
	int flags = 0;
	if ((style & WS_CAPTION) == WS_CAPTION) flags |= 1;
	if (style & WS_SYSMENU) flags |= 2;
	if (exstyle & WS_EX_CLIENTEDGE) flags |= 4;
	if (style & WS_BORDER) flags |= 8;
	if (style & WS_THICKFRAME) flags |= 16;
	if (wn->kind == CK_BUTTON && ((style & 0x0f) == BS_DEFPUSHBUTTON)) flags |= 32;
	if (wn->kind == CK_STATIC) flags |= ((style & SS_TYPEMASK) == SS_CENTER) ? 64 : ((style & SS_TYPEMASK) == SS_RIGHT) ? 128 : 0;
	if (wn->kind == CK_STATIC && (style & SS_TYPEMASK) == SS_OWNERDRAW) flags |= 256;
	if ((wn->kind == CK_TRACK && (style & TBS_VERT)) || (wn->kind == CK_SCROLL && (style & SBS_VERT))) flags |= 512;
	if (wn->kind == CK_EDIT && (style & ES_PASSWORD)) flags |= 1024;
	if (style & WS_DISABLED) { wn->enabled = false; }
	jsw_create(wn->id, wn->parent && (style & WS_CHILD) ? wn->parent->id : 0, wn->kind, flags, ansi_to_utf8(wn->text.c_str()).c_str());
	if (!wn->enabled) jsw_enable(wn->id, 0);
	wnd_sync_dom(wn);
	if (wn->kind == CK_STATIC && (style & SS_TYPEMASK) == SS_OWNERDRAW) ensure_canvas(wn);

	CREATESTRUCTA cs{}; cs.lpCreateParams = param; cs.hInstance = inst; cs.hMenu = menu; cs.hwndParent = parent;
	cs.cy = h; cs.cx = w; cs.y = y; cs.x = x; cs.style = style; cs.lpszName = name; cs.lpszClass = cls; cs.dwExStyle = exstyle;
	wnd_send(wn, WM_NCCREATE, 0, (LPARAM)&cs);
	if (wnd_send(wn, WM_CREATE, 0, (LPARAM)&cs) == -1) { DestroyWindow(H(wn)); return NULL; }
	if (style & WS_VISIBLE) { wn->visible = false; ShowWindow(H(wn), SW_SHOW); }
	if (!wn->parent || !(style & WS_CHILD)) { if (!g_active) g_active = wn; }
	return H(wn);
}

WINBOOL WINAPI DestroyWindow(HWND h)
{
	Wnd* w = wnd_get(h); if (!w || w->destroyed) return FALSE;
	auto kids = w->children;
	for (auto c : kids) DestroyWindow(H(c));
	wnd_send(w, WM_DESTROY, 0, 0);
	wnd_send(w, WM_NCDESTROY, 0, 0);
	w->destroyed = true;
	if (w->parent) { auto& v = w->parent->children; v.erase(std::remove(v.begin(), v.end(), w), v.end()); }
	g_top.erase(std::remove(g_top.begin(), g_top.end(), w), g_top.end());
	if (g_focus == w) g_focus = nullptr;
	if (g_active == w) g_active = g_top.empty() ? nullptr : g_top.back();
	jsw_destroy(w->id);
	g_byId.erase(w->id);
	for (auto& m : g_queue) if (m.hwnd == h) m.hwnd = NULL, m.message = WM_NULL;
	g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(), [&](const Timer& t) { return t.hwnd == h; }), g_timers.end());
	w->magic = 0xdead;	// keep memory (stale handles may still be used), but invalid
	return TRUE;
}
WINBOOL WINAPI IsWindow(HWND h) { return wnd_get(h) != nullptr; }
WINBOOL WINAPI IsIconic(HWND h) { Wnd* w = wnd_get(h); return w && w->iconic; }
WINBOOL WINAPI IsZoomed(HWND h) { Wnd* w = wnd_get(h); return w && w->zoomed; }

static void move_internal(Wnd* w, int x, int y, int cx, int cy, bool move, bool size)
{
	RECT old = w->rc;
	if (move) { w->rc.right += x - w->rc.left; w->rc.bottom += y - w->rc.top; w->rc.left = x; w->rc.top = y; }
	if (size) { w->rc.right = w->rc.left + cx; w->rc.bottom = w->rc.top + cy; }
	wnd_sync_dom(w);
	if (move && (old.left != w->rc.left || old.top != w->rc.top))
	{
		POINT o = {0, 0}; RECT c = w->clientRect(); o.x = w->rc.left + c.left; o.y = w->rc.top + c.top;
		wnd_send(w, WM_MOVE, 0, MAKELPARAM(o.x, o.y));
	}
	if (size && (old.right - old.left != w->rc.right - w->rc.left || old.bottom - old.top != w->rc.bottom - w->rc.top))
	{
		RECT c = w->clientRect();
		wnd_send(w, WM_SIZE, w->zoomed ? SIZE_MAXIMIZED : SIZE_RESTORED, MAKELPARAM(c.right - c.left, c.bottom - c.top));
		w->needPaint = true; w->erase = true;
	}
}
WINBOOL WINAPI MoveWindow(HWND h, int x, int y, int cx, int cy, WINBOOL rp)
{
	Wnd* w = wnd_get(h); if (!w) return FALSE;
	move_internal(w, x, y, cx, cy, true, true);
	if (rp) InvalidateRect(h, NULL, TRUE);
	return TRUE;
}
WINBOOL WINAPI SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
	Wnd* w = wnd_get(h); if (!w) return FALSE;
	if (flags & SWP_FRAMECHANGED) { int f = 0; if ((w->style & WS_CAPTION) == WS_CAPTION) f |= 1; if (w->style & WS_SYSMENU) f |= 2; if (w->exstyle & WS_EX_CLIENTEDGE) f |= 4; if (w->style & WS_BORDER) f |= 8; if (w->style & WS_THICKFRAME) f |= 16;
		EM_ASM({ PeterWin.frame($0, $1); }, w->id, f); }
	move_internal(w, x, y, cx, cy, !(flags & SWP_NOMOVE), !(flags & SWP_NOSIZE));
	if (flags & SWP_SHOWWINDOW) ShowWindow(h, SW_SHOW);
	if (flags & SWP_HIDEWINDOW) ShowWindow(h, SW_HIDE);
	if (!(flags & SWP_NOZORDER) && !w->parent) jsw_front(w->id);
	if (flags & SWP_FRAMECHANGED) { w->needPaint = true; w->erase = true; }
	return TRUE;
}
static RECT g_restore[1];
WINBOOL WINAPI ShowWindow(HWND h, int cmd)
{
	Wnd* w = wnd_get(h); if (!w) return FALSE;
	bool was = w->visible;
	switch (cmd)
	{
	case SW_HIDE: w->visible = false; break;
	case SW_MAXIMIZE:
		if (!w->zoomed) { w->longs[-1000] = w->rc.left; w->longs[-1001] = w->rc.top; w->longs[-1002] = w->rc.right; w->longs[-1003] = w->rc.bottom; }
		w->zoomed = true; w->iconic = false; w->visible = true;
		{
			int bx, by, cap; nc_metrics(w->style, w->exstyle, false, bx, by, cap);
			move_internal(w, -bx, -by, g_screenW + 2 * bx, g_screenH + 2 * by, true, true);
		}
		break;
	case SW_MINIMIZE: case SW_SHOWMINIMIZED: case SW_SHOWMINNOACTIVE: w->iconic = true; break;
	case SW_RESTORE: case SW_SHOWNORMAL:
		if (w->zoomed && w->longs.count(-1000))
		{
			w->zoomed = false;
			move_internal(w, (int)w->longs[-1000], (int)w->longs[-1001], (int)(w->longs[-1002] - w->longs[-1000]), (int)(w->longs[-1003] - w->longs[-1001]), true, true);
		}
		w->zoomed = false; w->iconic = false; w->visible = true; break;
	default: w->visible = true; w->iconic = false; break;
	}
	if (w->visible != was)
	{
		jsw_show(w->id, w->visible);
		wnd_send(w, WM_SHOWWINDOW, w->visible, 0);
		if (w->visible) { w->needPaint = true; w->erase = true; for (auto c : w->children) if (c->visible) { c->needPaint = true; c->erase = true; } }
		if (w->visible) { apply_ctlcolor(w); for (auto c : w->children) apply_ctlcolor(c); }
		if (w->visible && !w->parent) { g_active = w; jsw_front(w->id); }
	}
	return was;
}
WINBOOL WINAPI UpdateWindow(HWND h)
{
	Wnd* w = wnd_get(h); if (!w) return FALSE;
	if (w->needPaint && w->visible) wnd_send(w, WM_PAINT, 0, 0);
	return TRUE;
}
WINBOOL WINAPI GetWindowRect(HWND h, LPRECT r)
{
	Wnd* w = wnd_get(h);
	if (!w) { *r = {0, 0, g_screenW, g_screenH}; return h == NULL; }
	POINT o = {0, 0}; if (w->parent && (w->style & WS_CHILD)) o = wnd_client_origin(w->parent);
	r->left = o.x + w->rc.left; r->top = o.y + w->rc.top; r->right = o.x + w->rc.right; r->bottom = o.y + w->rc.bottom;
	return TRUE;
}
WINBOOL WINAPI GetClientRect(HWND h, LPRECT r)
{
	Wnd* w = wnd_get(h);
	if (!w) { *r = {0, 0, g_screenW, g_screenH}; return FALSE; }
	RECT c = w->clientRect();
	*r = {0, 0, c.right - c.left, c.bottom - c.top};
	return TRUE;
}
WINBOOL WINAPI ClientToScreen(HWND h, LPPOINT p) { Wnd* w = wnd_get(h); if (!w) return FALSE; POINT o = wnd_client_origin(w); p->x += o.x; p->y += o.y; return TRUE; }
WINBOOL WINAPI ScreenToClient(HWND h, LPPOINT p) { Wnd* w = wnd_get(h); if (!w) return FALSE; POINT o = wnd_client_origin(w); p->x -= o.x; p->y -= o.y; return TRUE; }
WINBOOL WINAPI AdjustWindowRectEx(LPRECT r, DWORD style, WINBOOL menu, DWORD ex)
{
	int bx, by, cap; nc_metrics(style, ex, (style & WS_CHILD) != 0, bx, by, cap);
	r->left -= bx; r->right += bx; r->top -= by + cap; r->bottom += by;
	if (menu) r->top -= 19;
	return TRUE;
}
WINBOOL WINAPI GetWindowPlacement(HWND h, WINDOWPLACEMENT* wp)
{
	Wnd* w = wnd_get(h); if (!w) return FALSE;
	wp->length = sizeof(*wp); wp->flags = 0;
	wp->showCmd = w->zoomed ? SW_SHOWMAXIMIZED : w->iconic ? SW_SHOWMINIMIZED : SW_SHOWNORMAL;
	if (w->zoomed && w->longs.count(-1000)) wp->rcNormalPosition = {(LONG)w->longs[-1000], (LONG)w->longs[-1001], (LONG)w->longs[-1002], (LONG)w->longs[-1003]};
	else wp->rcNormalPosition = w->rc;
	return TRUE;
}
WINBOOL WINAPI SetWindowPlacement(HWND h, const WINDOWPLACEMENT* wp)
{
	Wnd* w = wnd_get(h); if (!w) return FALSE;
	const RECT& r = wp->rcNormalPosition;
	if (wp->showCmd == SW_SHOWMAXIMIZED)
	{
		w->longs[-1000] = r.left; w->longs[-1001] = r.top; w->longs[-1002] = r.right; w->longs[-1003] = r.bottom;
		w->zoomed = false; ShowWindow(h, SW_MAXIMIZE);
	}
	else { w->zoomed = false; move_internal(w, r.left, r.top, r.right - r.left, r.bottom - r.top, true, true); ShowWindow(h, wp->showCmd); }
	return TRUE;
}

LONG WINAPI GetWindowLongA(HWND h, int idx)
{
	Wnd* w = wnd_get(h); if (!w) return 0;
	switch (idx)
	{
	case GWL_STYLE: return (LONG)(w->style | (w->visible ? WS_VISIBLE : 0) | (w->enabled ? 0 : WS_DISABLED));
	case GWL_EXSTYLE: return (LONG)w->exstyle;
	case GWL_WNDPROC: return (LONG)(uintptr_t)w->proc;
	case GWL_ID: return (LONG)(uintptr_t)w->menu;
	case GWL_HWNDPARENT: return (LONG)(uintptr_t)w->parent;
	default: return (LONG)w->longs[idx];
	}
}
LONG WINAPI SetWindowLongA(HWND h, int idx, LONG v)
{
	Wnd* w = wnd_get(h); if (!w) return 0;
	LONG old = GetWindowLongA(h, idx);
	switch (idx)
	{
	case GWL_STYLE: w->style = (DWORD)v & ~(WS_VISIBLE | WS_DISABLED); if (!w->parent) w->style &= ~WS_CHILD; break;
	case GWL_EXSTYLE: w->exstyle = (DWORD)v; break;
	case GWL_WNDPROC: w->proc = (WNDPROC)(uintptr_t)v; break;
	case GWL_ID: w->menu = (HMENU)(uintptr_t)v; break;
	default: w->longs[idx] = v; break;
	}
	return old;
}

int WINAPI GetWindowTextA(HWND h, LPSTR buf, int n) { return (int)SendMessageA(h, WM_GETTEXT, n, (LPARAM)buf); }
int WINAPI GetWindowTextLengthA(HWND h) { return (int)SendMessageA(h, WM_GETTEXTLENGTH, 0, 0); }
WINBOOL WINAPI SetWindowTextA(HWND h, LPCSTR t) { return (WINBOOL)SendMessageA(h, WM_SETTEXT, 0, (LPARAM)t); }
int WINAPI GetClassNameA(HWND h, LPSTR buf, int n)
{
	Wnd* w = wnd_get(h); if (!w || n <= 0) return 0;
	std::string c = w->cls;
	if (c == "button") c = "Button"; else if (c == "edit") c = "Edit"; else if (c == "listbox") c = "ListBox";
	else if (c == "combobox") c = "ComboBox"; else if (c == "static") c = "Static";
	strncpy(buf, c.c_str(), n); buf[n - 1] = 0; return (int)strlen(buf);
}
HWND WINAPI GetDlgItem(HWND h, int id)
{
	Wnd* w = wnd_get(h); if (!w) return NULL;
	for (auto c : w->children) if ((int)(uintptr_t)c->menu == id) return H(c);
	return NULL;
}
HWND WINAPI GetWindow(HWND h, UINT cmd)
{
	Wnd* w = wnd_get(h); if (!w) return NULL;
	if (cmd == GW_CHILD) return w->children.empty() ? NULL : H(w->children.front());
	if (cmd == GW_OWNER) return NULL;
	auto& sib = (w->parent && (w->style & WS_CHILD)) ? w->parent->children : g_top;
	auto it = std::find(sib.begin(), sib.end(), w);
	if (it == sib.end()) return NULL;
	if (cmd == GW_HWNDNEXT) { ++it; return it == sib.end() ? NULL : H(*it); }
	if (cmd == GW_HWNDPREV) { return it == sib.begin() ? NULL : H(*(it - 1)); }
	if (cmd == GW_HWNDFIRST) return H(sib.front());
	if (cmd == GW_HWNDLAST) return H(sib.back());
	return NULL;
}
WINBOOL WINAPI EnableWindow(HWND h, WINBOOL en)
{
	Wnd* w = wnd_get(h); if (!w) return FALSE;
	bool was = !w->enabled;
	if (w->enabled != (en != 0)) { w->enabled = en != 0; jsw_enable(w->id, w->enabled); wnd_send(w, WM_ENABLE, en, 0); }
	return was;
}
HWND WINAPI SetFocus(HWND h)
{
	Wnd* old = g_focus;
	Wnd* w = wnd_get(h);
	set_focus_internal(w, false);
	return H(old);
}
HWND WINAPI GetFocus(void) { return H(g_focus); }
HWND WINAPI GetForegroundWindow(void) { return H(g_active); }
WINBOOL WINAPI SetForegroundWindow(HWND h) { Wnd* w = wnd_get(h); if (w) { g_active = top_of(w); jsw_front(g_active->id); } return TRUE; }

// ---------------- messages
LRESULT WINAPI DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	Wnd* w = wnd_get(h); if (!w) return 0;
	switch (msg)
	{
	case WM_NCCREATE: return TRUE;
	case WM_SETTEXT:
		w->text = lp ? (const char*)lp : "";
		if (w->kind == CK_COMBO || w->kind == CK_COMBOEDIT) { /* keep selection */ }
		jsw_text(w->id, ansi_to_utf8(w->text.c_str()).c_str());
		return TRUE;
	case WM_GETTEXT:
	{
		if ((int)wp <= 0) return 0;
		int n = std::min((int)wp - 1, (int)w->text.size());
		memcpy((char*)lp, w->text.data(), n); ((char*)lp)[n] = 0;
		return n;
	}
	case WM_GETTEXTLENGTH: return (LRESULT)w->text.size();
	case WM_CLOSE: DestroyWindow(h); return 0;
	case WM_SYSCOMMAND:
		if ((wp & 0xfff0) == SC_CLOSE) { wnd_send(w, WM_CLOSE, 0, 0); return 0; }
		if ((wp & 0xfff0) == SC_MAXIMIZE) { ShowWindow(h, SW_MAXIMIZE); return 0; }
		if ((wp & 0xfff0) == SC_RESTORE) { ShowWindow(h, SW_RESTORE); return 0; }
		return 0;
	case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
	case WM_ERASEBKGND:
		if (w->clsBrush && w->hasCanvas) { RECT r; GetClientRect(h, &r); HBRUSH b = w->clsBrush; if ((uintptr_t)b <= 31) b = GetSysColorBrush((int)(uintptr_t)b - 1); FillRect((HDC)wp, &r, b); return 1; }
		return 0;
	case WM_SETCURSOR:
		if (LOWORD(lp) == HTCLIENT && w->clsCursor) { SetCursor(w->clsCursor); return TRUE; }
		if (w->parent && (w->style & WS_CHILD)) return wnd_send(w->parent, msg, wp, lp);
		SetCursor(LoadCursorA(NULL, IDC_ARROW));
		return FALSE;
	case WM_SETFONT: w->font = (HFONT)wp; return 0;
	case WM_GETFONT: return (LRESULT)w->font;
	case WM_SETREDRAW: return 0;
	case WM_SETICON: return 0;
	case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: case WM_CTLCOLORDLG: case WM_CTLCOLORLISTBOX:
		return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
	}
	return 0;
}

LRESULT WINAPI SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	Wnd* w = wnd_get(h);
	if (!w) return 0;
	return wnd_send(w, msg, wp, lp);
}

UINT WINAPI RegisterWindowMessageA(LPCSTR name)
{
	static std::map<std::string, UINT> m; static UINT next = 0xC000;
	auto it = m.find(name); if (it != m.end()) return it->second;
	return m[name] = next++;
}
VOID WINAPI PostQuitMessage(int code) { g_quit = true; g_quitCode = code; }

UINT_PTR WINAPI SetTimer(HWND h, UINT_PTR id, UINT ms, TIMERPROC proc)
{
	if (ms < 10) ms = 10;
	for (auto& t : g_timers) if (t.hwnd == h && t.id == id) { t.ms = ms; t.next = emscripten_get_now() + ms; t.proc = proc; return id; }
	if (!h && !id) id = 0x7000 + g_timers.size();
	g_timers.push_back({h, id, ms, emscripten_get_now() + ms, proc});
	return id;
}
WINBOOL WINAPI KillTimer(HWND h, UINT_PTR id)
{
	auto n = g_timers.size();
	g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(), [&](const Timer& t) { return t.hwnd == h && t.id == id; }), g_timers.end());
	return n != g_timers.size();
}

static void pump_timers()
{
	double now = emscripten_get_now();
	for (auto& t : g_timers)
	{
		if (now >= t.next)
		{
			// at most one pending WM_TIMER per timer (like Windows)
			bool pending = false;
			for (auto& m : g_queue) if (m.message == WM_TIMER && m.hwnd == t.hwnd && m.wParam == t.id) { pending = true; break; }
			if (!pending) post_msg(t.hwnd, WM_TIMER, t.id, (LPARAM)t.proc);
			t.next += t.ms;
			if (t.next < now) t.next = now + t.ms;	// don't accumulate backlog
		}
	}
}
static double next_timer_due()
{
	double d = 1e18; for (auto& t : g_timers) d = std::min(d, t.next);
	return d;
}

static Wnd* find_paint(Wnd* w)
{
	if (!w->visible || w->destroyed) return nullptr;
	if (w->needPaint) return w;
	for (auto c : w->children) { Wnd* r = find_paint(c); if (r) return r; }
	return nullptr;
}
static Wnd* any_paint()
{
	for (auto t : g_top) { Wnd* r = find_paint(t); if (r) return r; }
	return nullptr;
}

static bool match(const MSG& m, HWND h, UINT mn, UINT mx)
{
	if (h && m.hwnd != h) { Wnd* w = wnd_get(m.hwnd); bool child = false; for (; w; w = w->parent) if (H(w) == h) { child = true; break; } if (!child) return false; }
	if (mn == 0 && mx == 0) return true;
	return m.message >= mn && m.message <= mx;
}

static double g_lastYield = 0;
static void yield_browser(int ms)
{
	flush_surfaces();
	audio_pump();
	emscripten_sleep(ms);
	g_lastYield = emscripten_get_now();
	audio_pump();
}

static bool take_msg(MSG* out, HWND h, UINT mn, UINT mx, bool remove)
{
	pump_timers();
	for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
	{
		if (it->message == WM_NULL && !it->hwnd) { if (remove) { g_queue.erase(it); return take_msg(out, h, mn, mx, remove); } continue; }
		if (match(*it, h, mn, mx))
		{
			*out = *it;
			if (remove) g_queue.erase(it);
			return true;
		}
	}
	// synthesize WM_PAINT when no other messages
	if (mn == 0 && mx == 0 || (WM_PAINT >= mn && WM_PAINT <= mx))
	{
		Wnd* p = any_paint();
		if (p && (!h || H(p) == h))
		{
			MSG m{}; m.hwnd = H(p); m.message = WM_PAINT; m.time = g_msgSeq++; *out = m;
			return true;
		}
	}
	if (g_quit && (mn == 0 && mx == 0)) { MSG m{}; m.message = WM_QUIT; m.wParam = g_quitCode; *out = m; if (remove) g_quit = false; return true; }
	return false;
}

WINBOOL WINAPI GetMessageA(LPMSG msg, HWND h, UINT mn, UINT mx)
{
	for (;;)
	{
		if (take_msg(msg, h, mn, mx, true))
		{
			if (msg->message == WM_PAINT) { Wnd* w = wnd_get(msg->hwnd); if (w) { /* stays dirty until BeginPaint */ } }
			// keep the browser responsive even when messages never run out
			if (emscripten_get_now() - g_lastYield > 33) yield_browser(0);
			return msg->message != WM_QUIT;
		}
		double due = next_timer_due() - emscripten_get_now();
		int ms = due > 1e17 ? 16 : (int)std::max(0.0, std::min(due, 16.0));
		yield_browser(ms);
	}
}
WINBOOL WINAPI PeekMessageA(LPMSG msg, HWND h, UINT mn, UINT mx, UINT flags)
{
	if (emscripten_get_now() - g_lastYield > 33) yield_browser(0);
	bool r = take_msg(msg, h, mn, mx, (flags & PM_REMOVE) != 0);
	return r;
}
WINBOOL WINAPI TranslateMessage(const MSG* m)
{
	if (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN)
	{
		auto it = g_keyChar.find(m->time);
		if (it != g_keyChar.end())
		{
			int ch = it->second; g_keyChar.erase(it);
			MSG c = *m; c.message = (m->message == WM_SYSKEYDOWN) ? WM_SYSCHAR : WM_CHAR; c.wParam = ch;
			c.time = g_msgSeq++;
			g_queue.push_front(c);
			return TRUE;
		}
	}
	return FALSE;
}
LRESULT WINAPI DispatchMessageA(const MSG* m)
{
	if (m->message == WM_TIMER && m->lParam) { ((TIMERPROC)m->lParam)(m->hwnd, WM_TIMER, m->wParam, shim_ticks()); return 0; }
	Wnd* w = wnd_get(m->hwnd);
	if (!w) return 0;
	if (m->message == WM_PAINT && !w->needPaint) return 0;
	LRESULT r = wnd_send(w, m->message, m->wParam, m->lParam);
	if (m->message == WM_PAINT) w->needPaint = false;
	return r;
}
WINBOOL WINAPI IsDialogMessageA(HWND h, LPMSG m) { return FALSE; }

// ---------------- keyboard
SHORT WINAPI GetKeyState(int vk) { if (vk < 0 || vk > 255) return 0; return (SHORT)(((g_keys[vk] & 0x80) ? 0x8000 : 0) | (g_keys[vk] & 1)); }
SHORT WINAPI GetAsyncKeyState(int vk) { if (vk < 0 || vk > 255) return 0; return (g_keys[vk] & 0x80) ? (SHORT)0x8000 : 0; }
WINBOOL WINAPI GetKeyboardState(PBYTE k) { memcpy(k, g_keys, 256); return TRUE; }
WINBOOL WINAPI SetKeyboardState(LPBYTE k) { for (int i = 0; i < 256; i++) g_keys[i] = (g_keys[i] & 0x80) | (k[i] & 1); return TRUE; }

// ---------------- cursors / icons
struct CursorRec { uint32_t magic = 0x43555253; std::string css; };
static HCURSOR mk_cursor(const char* css) { CursorRec* c = new CursorRec; c->css = css; return (HCURSOR)c; }
HCURSOR WINAPI LoadCursorA(HINSTANCE inst, LPCSTR name)
{
	static std::map<uintptr_t, HCURSOR> stock;
	if (!inst && IS_INTRESOURCE(name))
	{
		uintptr_t id = (uintptr_t)name;
		auto it = stock.find(id); if (it != stock.end()) return it->second;
		const char* css = "default";
		if (name == IDC_IBEAM) css = "text"; else if (name == IDC_WAIT) css = "wait"; else if (name == IDC_CROSS) css = "crosshair";
		else if (name == IDC_UPARROW) css = "n-resize"; else if (name == IDC_SIZENWSE) css = "nwse-resize"; else if (name == IDC_SIZENESW) css = "nesw-resize";
		else if (name == IDC_SIZEWE) css = "ew-resize"; else if (name == IDC_SIZENS) css = "ns-resize"; else if (name == IDC_SIZEALL) css = "move";
		else if (name == IDC_NO) css = "not-allowed"; else if (name == IDC_HAND) css = "pointer"; else if (name == IDC_APPSTARTING) css = "progress";
		else if (name == IDC_HELP) css = "help";
		return stock[id] = mk_cursor(css);
	}
	return mk_cursor("default");
}
HCURSOR WINAPI SetCursor(HCURSOR h)
{
	HCURSOR old = g_curCursor;
	g_curCursor = h;
	CursorRec* c = checked<CursorRec>(h, 0x43555253);
	jsw_cursor(g_showCursor < 0 ? "none" : (c ? c->css.c_str() : "none"));
	return old;
}
int WINAPI ShowCursor(WINBOOL show)
{
	g_showCursor += show ? 1 : -1;
	CursorRec* c = checked<CursorRec>(g_curCursor, 0x43555253);
	jsw_cursor(g_showCursor < 0 ? "none" : (c ? c->css.c_str() : "default"));
	return g_showCursor;
}
EM_JS(char*, jsw_make_cursor, (int w, int h, int hx, int hy, const uint32_t* rgba), {
	var c = document.createElement('canvas'); c.width = w; c.height = h;
	var ctx = c.getContext('2d'); var img = ctx.createImageData(w, h);
	img.data.set(HEAPU8.subarray(rgba, rgba + w * h * 4)); ctx.putImageData(img, 0, 0);
	// A fully transparent cursor is how programs hide the pointer for mouse-look: report it
	// as 'none' so the page switches to relative (pointer-lock) mouse input.
	var visible = false;
	for (var i = 3; i < img.data.length; i += 4) if (img.data[i]) { visible = true; break; }
	if (!visible) return stringToNewUTF8('none');
	var s = 'url(' + c.toDataURL() + ') ' + hx + ' ' + hy + ', default';
	return stringToNewUTF8(s);
});
HCURSOR WINAPI CreateCursor(HINSTANCE inst, int hx, int hy, int w, int h, const VOID* andp, const VOID* xorp)
{
	// monochrome AND/XOR masks -> RGBA
	std::vector<uint32_t> px(w * h);
	int stride = ((w + 15) / 16) * 2;
	const BYTE* a = (const BYTE*)andp; const BYTE* x = (const BYTE*)xorp;
	for (int y = 0; y < h; y++) for (int i = 0; i < w; i++)
	{
		int bit = 7 - (i & 7);
		int am = (a[y * stride + i / 8] >> bit) & 1, xm = (x[y * stride + i / 8] >> bit) & 1;
		uint32_t c;
		if (!am) c = xm ? 0xffffffff : 0xff000000;
		else c = xm ? 0xff808080 : 0;	// inverted pixels shown as grey
		px[y * w + i] = c;
	}
	char* css = jsw_make_cursor(w, h, hx, hy, px.data());
	HCURSOR hc = mk_cursor(css); free(css);
	return hc;
}
HCURSOR shim_cursor_from_rgba(int w, int h, int hx, int hy, const uint32_t* px)
{
	char* css = jsw_make_cursor(w, h, hx, hy, px);
	HCURSOR hc = mk_cursor(css); free(css);
	return hc;
}
WINBOOL WINAPI DestroyCursor(HCURSOR h) { return TRUE; }
HICON WINAPI LoadIconA(HINSTANCE inst, LPCSTR name) { return (HICON)mk_cursor("default"); }
WINBOOL WINAPI DestroyIcon(HICON h) { return TRUE; }
HICON WINAPI ExtractIconA(HINSTANCE inst, LPCSTR file, UINT idx) { return NULL; }

WINBOOL WINAPI GetCursorPos(LPPOINT p) { *p = g_cursor; return TRUE; }
WINBOOL WINAPI SetCursorPos(int x, int y)
{
	// browsers cannot warp the pointer: emulate with pointer lock (relative motion)
	g_cursor.x = x; g_cursor.y = y;
	EM_ASM({ PeterWin.warp($0, $1); }, x, y);
	return TRUE;
}

// ---------------- system metrics / colors
int WINAPI GetSystemMetrics(int i)
{
	switch (i)
	{
	case SM_CXSCREEN: case SM_CXFULLSCREEN: case SM_CXMAXIMIZED: case SM_CXVIRTUALSCREEN: return g_screenW;
	case SM_CYSCREEN: case SM_CYMAXIMIZED: case SM_CYVIRTUALSCREEN: return g_screenH;
	case SM_CYFULLSCREEN: return g_screenH - 19;
	case SM_CYCAPTION: return 19;
	case SM_CXBORDER: case SM_CYBORDER: return 1;
	case SM_CXDLGFRAME: case SM_CYDLGFRAME: return 3;
	case SM_CXFRAME: case SM_CYFRAME: return 4;
	case SM_CXEDGE: case SM_CYEDGE: return 2;
	case SM_CXVSCROLL: case SM_CYHSCROLL: case SM_CYVSCROLL: case SM_CXHSCROLL: return 16;
	case SM_CXICON: case SM_CYICON: return 32;
	case SM_CXCURSOR: case SM_CYCURSOR: return 32;
	case SM_CXSMICON: case SM_CYSMICON: return 16;
	case SM_CYMENU: return 19;
	case SM_CXMIN: return 112; case SM_CYMIN: return 27;
	case SM_CMOUSEBUTTONS: return 3;
	case SM_MOUSEPRESENT: return 1;
	case SM_CXDOUBLECLK: case SM_CYDOUBLECLK: return 4;
	}
	return 0;
}
DWORD WINAPI GetSysColor(int i)
{
	switch (i)
	{
	case COLOR_WINDOW: return RGB(255, 255, 255);
	case COLOR_WINDOWTEXT: case COLOR_BTNTEXT: case COLOR_MENUTEXT: case COLOR_CAPTIONTEXT: case COLOR_INFOTEXT: return RGB(0, 0, 0);
	case COLOR_BTNFACE: case COLOR_MENU: case COLOR_SCROLLBAR: return RGB(212, 208, 200);
	case COLOR_BTNSHADOW: return RGB(128, 128, 128);
	case COLOR_BTNHIGHLIGHT: return RGB(255, 255, 255);
	case COLOR_3DDKSHADOW: return RGB(64, 64, 64);
	case COLOR_3DLIGHT: return RGB(212, 208, 200);
	case COLOR_HIGHLIGHT: case COLOR_ACTIVECAPTION: return RGB(10, 36, 106);
	case COLOR_HIGHLIGHTTEXT: return RGB(255, 255, 255);
	case COLOR_GRAYTEXT: return RGB(128, 128, 128);
	case COLOR_INFOBK: return RGB(255, 255, 225);
	case COLOR_BACKGROUND: return RGB(58, 110, 165);
	}
	return RGB(212, 208, 200);
}
WINBOOL WINAPI SystemParametersInfoA(UINT a, UINT b, PVOID p, UINT f)
{
	if (a == SPI_GETWORKAREA) { *(RECT*)p = {0, 0, g_screenW, g_screenH}; return TRUE; }
	if (a == SPI_GETSCREENSAVEACTIVE) { *(BOOL*)p = FALSE; return TRUE; }
	return FALSE;
}

// ---------------- display modes
LONG WINAPI ChangeDisplaySettingsA(LPDEVMODEA dm, DWORD flags) { return DISP_CHANGE_SUCCESSFUL; }
WINBOOL WINAPI EnumDisplaySettingsA(LPCSTR dev, DWORD n, LPDEVMODEA dm)
{
	static const int modes[][2] = {{640, 480}, {800, 600}, {1024, 768}};
	if (n == ENUM_CURRENT_SETTINGS || n == ENUM_REGISTRY_SETTINGS) n = 99;
	int w, h;
	if (n == 99) { w = g_screenW; h = g_screenH; }
	else if (n < 3) { w = modes[n][0]; h = modes[n][1]; }
	else return FALSE;
	memset(dm, 0, sizeof(*dm));
	dm->dmSize = sizeof(*dm); dm->dmBitsPerPel = 32; dm->dmPelsWidth = w; dm->dmPelsHeight = h; dm->dmDisplayFrequency = 60;
	dm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
	return TRUE;
}

// ---------------- scrollbars
int WINAPI SetScrollInfo(HWND h, int bar, LPCSCROLLINFO si, WINBOOL redraw)
{
	Wnd* w = wnd_get(h); if (!w || bar != SB_CTL) return 0;
	if (si->fMask & SIF_RANGE) { w->rmin = si->nMin; w->rmax = si->nMax - (int)((si->fMask & SIF_PAGE) ? si->nPage : 0) + 1; }
	if (si->fMask & SIF_POS) w->pos = si->nPos;
	jsw_range(w->id, w->rmin, w->rmax, w->pos);
	return w->pos;
}
int WINAPI SetScrollPos(HWND h, int bar, int pos, WINBOOL redraw)
{
	Wnd* w = wnd_get(h); if (!w) return 0;
	int o = w->pos; w->pos = pos; jsw_range(w->id, w->rmin, w->rmax, w->pos); return o;
}

// ---------------- common controls
void WINAPI InitCommonControls(void) {}
HWND WINAPI CreateStatusWindowA(LONG style, LPCSTR text, HWND parent, UINT id)
{
	Wnd* p = wnd_get(parent); if (!p) return NULL;
	RECT c = p->clientRect();
	HWND h = CreateWindowExA(0, "msctls_statusbar32", text, (DWORD)style | WS_CHILD, 0, (c.bottom - c.top) - 20, c.right - c.left, 20, parent, (HMENU)(uintptr_t)id, NULL, NULL);
	return h;
}

} // extern "C"

void shim_set_screen(int w, int h) { g_screenW = w; g_screenH = h; jsw_screen(w, h); }
bool shim_quit_requested() { return g_quit; }

// ------------------------------------------------------------------ debugging aid
static void dump_wnd(Wnd* w, int depth)
{
	SHIM_LOG("%*s#%d %s kind=%d style=%08x ex=%08x rect=(%d,%d %dx%d) vis=%d canvas=%d gl=%d text='%s' id=%d paint=%d",
		depth * 2, "", w->id, w->cls.c_str(), w->kind, (unsigned)w->style, (unsigned)w->exstyle, (int)w->rc.left, (int)w->rc.top,
		(int)(w->rc.right - w->rc.left), (int)(w->rc.bottom - w->rc.top), w->visible, w->hasCanvas, w->glCanvas, w->text.c_str(), (int)(uintptr_t)w->menu, w->needPaint);
	for (auto c : w->children) dump_wnd(c, depth + 1);
}
extern "C" EMSCRIPTEN_KEEPALIVE void shim_dump()
{
	for (auto t : g_top) dump_wnd(t, 0);
	SHIM_LOG("focus=#%d active=#%d timers=%d queue=%d", g_focus ? g_focus->id : 0, g_active ? g_active->id : 0, (int)g_timers.size(), (int)g_queue.size());
}

void shim_ctl_invalidated(Wnd* w) { if (w->visible) apply_ctlcolor(w); }

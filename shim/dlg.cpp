// Win32 shim: message boxes, resource dialogs, common file dialogs, text helpers, program entry
#include "shim.h"
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

// ------------------------------------------------------------------ text conversion (Windows-1252 <-> UTF-8)
static const unsigned short cp1252_hi[32] = {
	0x20AC,0x0081,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0x008D,0x017D,0x008F,
	0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0x009D,0x017E,0x0178 };
std::string ansi_to_utf8(const char* s, int len)
{
	std::string r;
	if (!s) return r;
	if (len < 0) len = (int)strlen(s);
	for (int i = 0; i < len; i++)
	{
		unsigned c = (unsigned char)s[i];
		if (c >= 0x80 && c < 0xa0) c = cp1252_hi[c - 0x80];
		if (c < 0x80) r += (char)c;
		else if (c < 0x800) { r += (char)(0xc0 | (c >> 6)); r += (char)(0x80 | (c & 63)); }
		else { r += (char)(0xe0 | (c >> 12)); r += (char)(0x80 | ((c >> 6) & 63)); r += (char)(0x80 | (c & 63)); }
	}
	return r;
}
std::string utf8_to_ansi(const char* s)
{
	std::string r;
	const unsigned char* p = (const unsigned char*)s;
	while (*p)
	{
		unsigned c = *p++;
		if (c >= 0xc0)
		{
			int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
			c &= (0x3f >> n);
			for (int k = 0; k < n && *p; k++) c = (c << 6) | (*p++ & 63);
		}
		if (c < 0x80 || (c >= 0xa0 && c < 0x100)) r += (char)c;
		else
		{
			int f = '?';
			for (int i = 0; i < 32; i++) if (cp1252_hi[i] == c) { f = 0x80 + i; break; }
			r += (char)f;
		}
	}
	return r;
}

// ------------------------------------------------------------------ modal helpers
EM_JS(int, jsd_msgbox_open, (const char* title, const char* text, int type), { return PeterWin.msgBox(UTF8ToString(title), UTF8ToString(text), type); });
EM_JS(int, jsd_poll, (int h), { return PeterWin.pollModal(h); });
EM_JS(char*, jsd_result_text, (int h), { var s = PeterWin.modalText(h); return s == null ? 0 : stringToNewUTF8(s); });

extern void flush_surfaces();
static int wait_modal(int h)
{
	int r;
	while ((r = jsd_poll(h)) == 0) { flush_surfaces(); audio_pump(); emscripten_sleep(30); }
	return r;
}

extern "C" {

int WINAPI MessageBoxA(HWND h, LPCSTR text, LPCSTR title, UINT type)
{
	SHIM_LOG("[MessageBox] %s: %s", title ? title : "", text ? text : "");
	int m = jsd_msgbox_open(ansi_to_utf8(title ? title : "").c_str(), ansi_to_utf8(text ? text : "").c_str(), (int)type);
	return wait_modal(m);
}

// ---------------------------------------------------------------- resource dialogs
struct DlgState { DLGPROC proc; bool ended = false; INT_PTR result = 0; HWND hwnd = NULL; };
static std::vector<DlgState*> g_dlgs;
static DlgState* dlg_for(HWND h) { for (auto d : g_dlgs) if (d->hwnd == h) return d; return nullptr; }

static LRESULT CALLBACK dialog_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	DlgState* d = dlg_for(h);
	if (d && d->proc)
	{
		INT_PTR r = d->proc(h, msg, wp, lp);
		if (r) return r;
	}
	if (msg == WM_CLOSE) { if (d && d->proc) d->proc(h, WM_COMMAND, IDCANCEL, 0); return 0; }
	if (msg == WM_ERASEBKGND) return 1;
	return DefWindowProcA(h, msg, wp, lp);
}

static const WORD* skip_sz_or_ord(const WORD* p, std::string* out, int* ord)
{
	if (*p == 0xffff) { if (ord) *ord = p[1]; return p + 2; }
	if (ord) *ord = -1;
	std::wstring s; while (*p) s += (wchar_t)*p++;
	if (out) { out->clear(); for (wchar_t c : s) out->push_back(c < 256 ? (char)c : '?'); }
	return p + 1;
}
static const WORD* align4(const WORD* p) { return (const WORD*)(((uintptr_t)p + 3) & ~3); }

INT_PTR WINAPI DialogBoxParamA(HINSTANCE inst, LPCSTR name, HWND owner, DLGPROC proc, LPARAM lp)
{
	HRSRC r = FindResourceA(inst, name, (LPCSTR)RT_DIALOG);
	if (!r) { SHIM_LOG("[dlg] dialog resource not found"); return -1; }
	const BYTE* base = (const BYTE*)LockResource(LoadResource(inst, r));
	bool ex = ((const WORD*)base)[1] == 0xffff;
	DWORD style, exstyle; int nItems; short x, y, cx, cy;
	const WORD* p;
	if (ex)
	{
		const BYTE* q = base + 8;	// dlgVer, signature, helpID
		exstyle = *(const DWORD*)q; style = *(const DWORD*)(q + 4); nItems = *(const WORD*)(q + 8);
		x = *(const short*)(q + 10); y = *(const short*)(q + 12); cx = *(const short*)(q + 14); cy = *(const short*)(q + 16);
		p = (const WORD*)(q + 18);
	}
	else
	{
		style = *(const DWORD*)base; exstyle = *(const DWORD*)(base + 4); nItems = *(const WORD*)(base + 8);
		x = *(const short*)(base + 10); y = *(const short*)(base + 12); cx = *(const short*)(base + 14); cy = *(const short*)(base + 16);
		p = (const WORD*)(base + 18);
	}
	p = skip_sz_or_ord(p, nullptr, nullptr);	// menu
	p = skip_sz_or_ord(p, nullptr, nullptr);	// class
	std::string title; p = skip_sz_or_ord(p, &title, nullptr);
	int ptsize = 8; std::string face = "MS Sans Serif";
	if (style & DS_SETFONT)
	{
		ptsize = *p++;
		if (ex) p += 2;	// weight, italic+charset
		p = skip_sz_or_ord(p, &face, nullptr);
	}
	// dialog base units for an 8pt font ~ (6, 13)
	double bux = 6.0 * ptsize / 8.0, buy = 13.0 * ptsize / 8.0;
	auto DX = [&](int v) { return (int)(v * bux / 4.0 + 0.5); };
	auto DY = [&](int v) { return (int)(v * buy / 8.0 + 0.5); };

	static bool reg = false;
	if (!reg) { reg = true; WNDCLASSA wc{}; wc.lpfnWndProc = dialog_wndproc; wc.lpszClassName = "#32770"; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursorA(NULL, IDC_ARROW); RegisterClassA(&wc); }

	DlgState* st = new DlgState; st->proc = proc;
	g_dlgs.push_back(st);
	RECT rc = {0, 0, DX(cx), DY(cy)};
	DWORD wstyle = (style | WS_CAPTION) & ~(WS_CHILD | WS_VISIBLE);
	AdjustWindowRectEx(&rc, wstyle, FALSE, exstyle);
	int w = rc.right - rc.left, hgt = rc.bottom - rc.top;
	RECT orc; GetWindowRect(owner, &orc);
	int px = (orc.left + orc.right - w) / 2, py = (orc.top + orc.bottom - hgt) / 2;
	if (owner == NULL) { px = (g_screenW - w) / 2; py = (g_screenH - hgt) / 2; }
	HWND dlg = CreateWindowExA(exstyle, "#32770", title.c_str(), wstyle, px, py, w, hgt, NULL, NULL, inst, NULL);
	st->hwnd = dlg;
	LOGFONTA lf{}; lf.lfHeight = -(ptsize * 96 / 72); lf.lfWeight = FW_NORMAL; strcpy(lf.lfFaceName, "Tahoma");
	HFONT font = CreateFontA(lf.lfHeight, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0, 0, 0, "Tahoma");
	HWND firstFocus = NULL;
	for (int i = 0; i < nItems; i++)
	{
		p = align4(p);
		DWORD istyle, iex; short ix, iy, icx, icy; DWORD id;
		if (ex)
		{
			const BYTE* q = (const BYTE*)p;
			iex = *(const DWORD*)(q + 4); istyle = *(const DWORD*)(q + 8);
			ix = *(const short*)(q + 12); iy = *(const short*)(q + 14); icx = *(const short*)(q + 16); icy = *(const short*)(q + 18);
			id = *(const DWORD*)(q + 20); p = (const WORD*)(q + 24);
		}
		else
		{
			const BYTE* q = (const BYTE*)p;
			istyle = *(const DWORD*)q; iex = *(const DWORD*)(q + 4);
			ix = *(const short*)(q + 8); iy = *(const short*)(q + 10); icx = *(const short*)(q + 12); icy = *(const short*)(q + 14);
			id = *(const WORD*)(q + 16); p = (const WORD*)(q + 18);
		}
		std::string cls; int clsOrd; p = skip_sz_or_ord(p, &cls, &clsOrd);
		std::string text; int textOrd; p = skip_sz_or_ord(p, &text, &textOrd);
		WORD extra = *p++; p = (const WORD*)((const BYTE*)p + extra);
		if (clsOrd >= 0)
		{
			switch (clsOrd) { case 0x80: cls = "Button"; break; case 0x81: cls = "Edit"; break; case 0x82: cls = "Static"; break;
				case 0x83: cls = "ListBox"; break; case 0x84: cls = "ScrollBar"; break; case 0x85: cls = "ComboBox"; break; }
		}
		HWND c = CreateWindowExA(iex, cls.c_str(), text.c_str(), istyle | WS_CHILD, DX(ix), DY(iy), DX(icx), DY(icy), dlg, (HMENU)(uintptr_t)id, inst, NULL);
		if (c) { SendMessageA(c, WM_SETFONT, (WPARAM)font, 0); if (!firstFocus && (istyle & WS_TABSTOP)) firstFocus = c; }
	}
	if (owner) EnableWindow(owner, FALSE);
	ShowWindow(dlg, SW_SHOW);
	if (proc(dlg, WM_INITDIALOG, (WPARAM)firstFocus, lp) && firstFocus) SetFocus(firstFocus);
	// modal loop
	MSG m;
	while (!st->ended && IsWindow(dlg))
	{
		if (!GetMessageA(&m, NULL, 0, 0)) { PostQuitMessage((int)m.wParam); break; }
		if (m.message == WM_KEYDOWN && (m.wParam == VK_RETURN || m.wParam == VK_ESCAPE))
		{
			HWND f = GetFocus(); Wnd* fw = wnd_get(f);
			bool multiline = fw && fw->kind == CK_MEMO;
			if (!(multiline && m.wParam == VK_RETURN))
			{
				proc(dlg, WM_COMMAND, m.wParam == VK_RETURN ? IDOK : IDCANCEL, 0);
				continue;
			}
		}
		TranslateMessage(&m);
		DispatchMessageA(&m);
	}
	INT_PTR res = st->result;
	if (owner) EnableWindow(owner, TRUE);
	if (IsWindow(dlg)) DestroyWindow(dlg);
	g_dlgs.erase(std::remove(g_dlgs.begin(), g_dlgs.end(), st), g_dlgs.end());
	delete st;
	if (owner) SetForegroundWindow(owner);
	return res;
}
WINBOOL WINAPI EndDialog(HWND h, INT_PTR r)
{
	DlgState* d = dlg_for(h); if (!d) return FALSE;
	d->ended = true; d->result = r;
	return TRUE;
}

// ---------------------------------------------------------------- common file dialogs
EM_JS(int, jsd_file_open, (int save, const char* title, const char* dir, const char* files, const char* deflt), {
	return PeterWin.fileDialog(save, UTF8ToString(title), UTF8ToString(dir), UTF8ToString(files), UTF8ToString(deflt));
});

static std::vector<std::string> filter_patterns(const char* filter, int index)
{
	std::vector<std::string> pats;
	if (!filter) { pats.push_back("*"); return pats; }
	const char* p = filter; int n = 1;
	while (*p)
	{
		const char* desc = p; p += strlen(p) + 1;
		const char* pat = p; p += strlen(p) + 1;
		(void)desc;
		if (n == (index ? index : 1))
		{
			std::string s = pat; size_t a = 0;
			while (a <= s.size()) { size_t b = s.find(';', a); if (b == std::string::npos) b = s.size(); pats.push_back(s.substr(a, b - a)); a = b + 1; }
		}
		n++;
	}
	if (pats.empty()) pats.push_back("*");
	return pats;
}
#include <fnmatch.h>
static BOOL file_dialog(LPOPENFILENAMEA ofn, bool save)
{
	char cwd[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH, cwd);
	std::string dir = ofn->lpstrInitialDir && *ofn->lpstrInitialDir ? ofn->lpstrInitialDir : cwd;
	std::string initial = ofn->lpstrFile ? ofn->lpstrFile : "";
	if (!initial.empty() && initial.find('\\') != std::string::npos)
	{
		size_t k = initial.rfind('\\'); dir = initial.substr(0, k); initial = initial.substr(k + 1);
	}
	auto pats = filter_patterns(ofn->lpstrFilter, ofn->nFilterIndex);
	std::string list;
	DIR* d = opendir(win_to_unix(dir.c_str()).c_str());
	if (d)
	{
		struct dirent* e;
		while ((e = readdir(d)))
		{
			std::string n = e->d_name; if (n == "." || n == "..") continue;
			std::string full = win_to_unix(dir.c_str()) + "/" + n;
			struct stat st; if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) continue;
			bool ok = false; for (auto& pt : pats) if (pt == "*.*" || fnmatch(pt.c_str(), n.c_str(), FNM_CASEFOLD) == 0) ok = true;
			if (ok) { if (!list.empty()) list += '\n'; list += ansi_to_utf8(n.c_str()); }
		}
		closedir(d);
	}
	std::string title = ofn->lpstrTitle ? ofn->lpstrTitle : (save ? "Save As" : "Open");
	int h = jsd_file_open(save, ansi_to_utf8(title.c_str()).c_str(), ansi_to_utf8(dir.c_str()).c_str(), list.c_str(), ansi_to_utf8(initial.c_str()).c_str());
	int r = wait_modal(h);
	if (r != IDOK) return FALSE;
	char* t = jsd_result_text(h);
	if (!t) return FALSE;
	std::string name = utf8_to_ansi(t); free(t);
	if (name.empty()) return FALSE;
	if (name.find('.') == std::string::npos && ofn->lpstrDefExt && *ofn->lpstrDefExt) { name += "."; name += ofn->lpstrDefExt; }
	std::string full = name.find(':') != std::string::npos ? name : dir + (dir.back() == '\\' ? "" : "\\") + name;
	if (full.size() + 1 > ofn->nMaxFile) return FALSE;
	strcpy(ofn->lpstrFile, full.c_str());
	size_t k = full.rfind('\\');
	ofn->nFileOffset = (WORD)(k + 1);
	size_t e = full.rfind('.'); ofn->nFileExtension = (e != std::string::npos && e > k) ? (WORD)(e + 1) : 0;
	if (ofn->lpstrFileTitle && ofn->nMaxFileTitle) { strncpy(ofn->lpstrFileTitle, full.c_str() + k + 1, ofn->nMaxFileTitle); }
	return TRUE;
}
WINBOOL WINAPI GetOpenFileNameA(LPOPENFILENAMEA ofn) { return file_dialog(ofn, false); }
WINBOOL WINAPI GetSaveFileNameA(LPOPENFILENAMEA ofn) { return file_dialog(ofn, true); }
WINBOOL WINAPI PrintDlgA(LPPRINTDLGA p) { return FALSE; }
HWND WINAPI FindTextA(LPFINDREPLACEA f) { return NULL; }

} // extern "C"

// ------------------------------------------------------------------ D3D (DirectX) backends are not available in the browser
bool D3DX3Start(int) { return false; } void D3DX3TermD3() {} void D3DX3TermDD() {}
bool D3DX5Start(int) { return false; } void D3DX5TermD3() {} void D3DX5TermDD() {}
bool D3DX6Start(int) { return false; } void D3DX6TermD3() {} void D3DX6TermDD() {}
bool D3DX7Start(int) { return false; } void D3DX7TermD3() {} void D3DX7TermDD() {}
bool D3DX8Start(int) { return false; } void D3DX8TermD3() {}

// ------------------------------------------------------------------ program entry
extern "C" bool shim_load_image(const char* winpath, const char* unixImage);
void shim_set_cwd(const char* w);
void shim_set_screen(int w, int h);
void WinMainCRTStartup();
extern char DATAsize[];
extern "C" BYTE* shim_image();
extern "C" BYTE* shim_find_section(const char*, DWORD*);

int main()
{
	char* exe = (char*)EM_ASM_PTR({ return stringToNewUTF8(Module.peterExe || 'C:\\PETER\\PROGRAM.EXE'); });
	char* img = (char*)EM_ASM_PTR({ return stringToNewUTF8(Module.peterImage || ''); });
	int sw = EM_ASM_INT({ return Module.screenW || 1024; });
	int sh = EM_ASM_INT({ return Module.screenH || 768; });
	shim_set_screen(sw, sh);
	bool loaded = shim_load_image(exe, img);
	free(img);
	if (!loaded)
	{
		EM_ASM({ PeterWin.fatal('Could not load ' + UTF8ToString($0)); }, exe);
		return 1;
	}
	// locate the Peter program data (.petprg section) and pass its RVA/size the way Peter's
	// editor patches them into the DATAsize marker of the loader EXE
	{
		DWORD size = 0; BYTE* sec = shim_find_section(".petprg", &size);
		if (!sec) sec = shim_find_section(".rtexts", &size);
		if (!sec) { EM_ASM({ PeterWin.fatal('This EXE does not contain a Peter program.'); }); return 1; }
		((DWORD*)DATAsize)[0] = (DWORD)(sec - shim_image());
		((DWORD*)DATAsize)[1] = size;
		SHIM_LOG("[peter] program data at RVA %#x, %u bytes, sig %.4s", (unsigned)(sec - shim_image()), (unsigned)size, (const char*)sec);
	}
	std::string dir = exe; dir = dir.substr(0, dir.rfind('\\'));
	shim_set_cwd(dir.c_str());
	free(exe);
	WinMainCRTStartup();
	return 0;
}

// Win32 shim: kernel-ish services (memory, strings, time, module, resources, registry, misc)
#include "shim.h"
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <set>
#include <algorithm>

void shim_unimpl(const char* name)
{
	static std::set<std::string> seen;
	if (seen.insert(name).second) SHIM_LOG("[shim] unimplemented: %s", name);
}

extern "C" {

// ------------------------------------------------------------------ memory
LPVOID WINAPI VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot)
{
	if (addr && (type & MEM_COMMIT) && !(type & MEM_RESERVE)) return addr;	// commit inside reserved block
	void* p = nullptr;
	if (posix_memalign(&p, 4096, size ? size : 1) != 0) return nullptr;
	memset(p, 0, size);
	return p;
}
WINBOOL WINAPI VirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
	if (type & MEM_RELEASE) free(addr);
	return TRUE;
}
HANDLE WINAPI HeapCreate(DWORD opts, SIZE_T init, SIZE_T max) { return (HANDLE)0x48454150; }
WINBOOL WINAPI HeapDestroy(HANDLE h) { return TRUE; }
LPVOID WINAPI HeapAlloc(HANDLE h, DWORD flags, SIZE_T size)
{
	void* p = malloc(size ? size : 1);
	if (p && (flags & HEAP_ZERO_MEMORY)) memset(p, 0, size);
	return p;
}
LPVOID WINAPI HeapReAlloc(HANDLE h, DWORD flags, LPVOID p, SIZE_T size)
{
	size_t old = p ? malloc_usable_size(p) : 0;
	void* n = realloc(p, size ? size : 1);
	if (n && (flags & HEAP_ZERO_MEMORY) && size > old) memset((char*)n + old, 0, size - old);
	return n;
}
WINBOOL WINAPI HeapFree(HANDLE h, DWORD flags, LPVOID p) { free(p); return TRUE; }
SIZE_T WINAPI HeapSize(HANDLE h, DWORD flags, LPCVOID p) { return p ? malloc_usable_size((void*)p) : 0; }
HGLOBAL WINAPI GlobalFree(HGLOBAL h) { free(h); return NULL; }

// ------------------------------------------------------------------ errors
static DWORD g_lastError = 0;
VOID WINAPI SetLastError(DWORD e) { g_lastError = e; }
DWORD WINAPI GetLastError(void) { return g_lastError; }

// ------------------------------------------------------------------ sync (single threaded)
VOID WINAPI InitializeCriticalSection(LPCRITICAL_SECTION cs) {}
VOID WINAPI DeleteCriticalSection(LPCRITICAL_SECTION cs) {}
VOID WINAPI EnterCriticalSection(LPCRITICAL_SECTION cs) {}
VOID WINAPI LeaveCriticalSection(LPCRITICAL_SECTION cs) {}
DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms) { return WAIT_OBJECT_0; }

// ------------------------------------------------------------------ strings
int WINAPI lstrlenA(LPCSTR s) { return s ? (int)strlen(s) : 0; }
int WINAPI lstrlenW(LPCWSTR s) { if (!s) return 0; int n = 0; while (s[n]) n++; return n; }

// Windows-1252 upper/lower mapping for the high half
static unsigned char up1252(unsigned char c)
{
	if (c >= 'a' && c <= 'z') return c - 32;
	if (c == 0x9a || c == 0x9c || c == 0x9e) return c - 16;
	if (c == 0xff) return 0x9f;
	if (c >= 0xe0 && c != 0xf7) return c - 32;
	return c;
}
static unsigned char lo1252(unsigned char c)
{
	if (c >= 'A' && c <= 'Z') return c + 32;
	if (c == 0x8a || c == 0x8c || c == 0x8e) return c + 16;
	if (c == 0x9f) return 0xff;
	if (c >= 0xc0 && c <= 0xde && c != 0xd7) return c + 32;
	return c;
}
LPSTR WINAPI CharUpperA(LPSTR s)
{
	if (((uintptr_t)s >> 16) == 0) return (LPSTR)(uintptr_t)up1252((unsigned char)(uintptr_t)s);
	for (char* p = s; *p; p++) *p = (char)up1252((unsigned char)*p);
	return s;
}
LPSTR WINAPI CharLowerA(LPSTR s)
{
	if (((uintptr_t)s >> 16) == 0) return (LPSTR)(uintptr_t)lo1252((unsigned char)(uintptr_t)s);
	for (char* p = s; *p; p++) *p = (char)lo1252((unsigned char)*p);
	return s;
}
WINBOOL WINAPI CharToOemBuffA(LPCSTR src, LPSTR dst, DWORD n) { if (dst != src) memmove(dst, src, n); return TRUE; }
WINBOOL WINAPI OemToCharBuffA(LPCSTR src, LPSTR dst, DWORD n) { if (dst != src) memmove(dst, src, n); return TRUE; }

int WINAPI CompareStringA(LCID lcid, DWORD flags, PCNZCH a, int na, PCNZCH b, int nb)
{
	if (na < 0) na = (int)strlen(a);
	if (nb < 0) nb = (int)strlen(b);
	int n = na < nb ? na : nb;
	for (int i = 0; i < n; i++)
	{
		unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
		if (flags & NORM_IGNORECASE) { x = lo1252(x); y = lo1252(y); }
		if (x != y) return x < y ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
	}
	if (na == nb) return CSTR_EQUAL;
	return na < nb ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
}

// cp1252 high half -> unicode
static const unsigned short cp1252_hi[32] = {
	0x20AC,0x0081,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0x008D,0x017D,0x008F,
	0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0x009D,0x017E,0x0178 };
static unsigned short to_uni(unsigned char c) { return (c >= 0x80 && c < 0xa0) ? cp1252_hi[c - 0x80] : c; }
static int from_uni(unsigned int u)
{
	if (u < 0x80 || (u >= 0xa0 && u < 0x100)) return (int)u;
	for (int i = 0; i < 32; i++) if (cp1252_hi[i] == u) return 0x80 + i;
	return '?';
}

int WINAPI MultiByteToWideChar(UINT cp, DWORD flags, LPCCH src, int n, LPWSTR dst, int dn)
{
	if (n < 0) n = (int)strlen(src) + 1;
	if (dn == 0) return n;
	int m = n < dn ? n : dn;
	for (int i = 0; i < m; i++) dst[i] = to_uni((unsigned char)src[i]);
	return m;
}
int WINAPI WideCharToMultiByte(UINT cp, DWORD flags, LPCWCH src, int n, LPSTR dst, int dn, LPCCH def, LPBOOL used)
{
	if (n < 0) { n = 0; while (src[n]) n++; n++; }
	if (dn == 0) return n;
	int m = n < dn ? n : dn;
	for (int i = 0; i < m; i++) dst[i] = (char)from_uni(src[i]);
	return m;
}
UINT WINAPI GetACP(void) { return 1252; }
LANGID WINAPI GetUserDefaultLangID(void) { return 0x0409; }

// ------------------------------------------------------------------ time
static double t0 = emscripten_get_now();
DWORD WINAPI timeGetTime(void) { return (DWORD)(emscripten_get_now() - t0); }
WINBOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000; return TRUE; }
WINBOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER* c) { c->QuadPart = (LONGLONG)((emscripten_get_now() - t0) * 1000.0); return TRUE; }

static void tm_to_systime(const struct tm* t, int ms, SYSTEMTIME* st)
{
	st->wYear = t->tm_year + 1900; st->wMonth = t->tm_mon + 1; st->wDayOfWeek = t->tm_wday;
	st->wDay = t->tm_mday; st->wHour = t->tm_hour; st->wMinute = t->tm_min; st->wSecond = t->tm_sec;
	st->wMilliseconds = ms;
}
VOID WINAPI GetLocalTime(LPSYSTEMTIME st)
{
	double ms = EM_ASM_DOUBLE({ return Date.now(); });
	time_t s = (time_t)(ms / 1000); struct tm t; localtime_r(&s, &t);
	tm_to_systime(&t, (int)fmod(ms, 1000.0), st);
}
WINBOOL WINAPI SetLocalTime(const SYSTEMTIME* st) { return FALSE; }

// FILETIME = 100ns since 1601-01-01 UTC
static const long long EPOCH_DIFF = 11644473600LL;
WINBOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME* st, LPFILETIME ft)
{
	struct tm t{}; t.tm_year = st->wYear - 1900; t.tm_mon = st->wMonth - 1; t.tm_mday = st->wDay;
	t.tm_hour = st->wHour; t.tm_min = st->wMinute; t.tm_sec = st->wSecond;
	long long s = (long long)timegm(&t);
	unsigned long long v = (unsigned long long)(s + EPOCH_DIFF) * 10000000ULL + st->wMilliseconds * 10000ULL;
	ft->dwLowDateTime = (DWORD)v; ft->dwHighDateTime = (DWORD)(v >> 32);
	return TRUE;
}
WINBOOL WINAPI FileTimeToSystemTime(const FILETIME* ft, LPSYSTEMTIME st)
{
	unsigned long long v = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
	time_t s = (time_t)((long long)(v / 10000000ULL) - EPOCH_DIFF);
	struct tm t; gmtime_r(&s, &t);
	tm_to_systime(&t, (int)((v / 10000ULL) % 1000), st);
	return TRUE;
}
static long tz_offset_100ns()
{
	time_t now = time(nullptr); struct tm l; localtime_r(&now, &l);
	return l.tm_gmtoff;
}
WINBOOL WINAPI FileTimeToLocalFileTime(const FILETIME* ft, LPFILETIME lt)
{
	long long v = (long long)(((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime);
	v += (long long)tz_offset_100ns() * 10000000LL;
	lt->dwLowDateTime = (DWORD)v; lt->dwHighDateTime = (DWORD)((unsigned long long)v >> 32);
	return TRUE;
}
WINBOOL WINAPI LocalFileTimeToFileTime(const FILETIME* lt, LPFILETIME ft)
{
	long long v = (long long)(((unsigned long long)lt->dwHighDateTime << 32) | lt->dwLowDateTime);
	v -= (long long)tz_offset_100ns() * 10000000LL;
	ft->dwLowDateTime = (DWORD)v; ft->dwHighDateTime = (DWORD)((unsigned long long)v >> 32);
	return TRUE;
}

// ------------------------------------------------------------------ version / system
DWORD WINAPI GetVersion(void) { return 0x0A280105; }	// NT 5.1 (XP), build 2600
WINBOOL WINAPI GetVersionExA(LPOSVERSIONINFOA vi)
{
	vi->dwMajorVersion = 5; vi->dwMinorVersion = 1; vi->dwBuildNumber = 2600;
	vi->dwPlatformId = VER_PLATFORM_WIN32_NT; strcpy(vi->szCSDVersion, "");
	return TRUE;
}
UINT WINAPI GetWindowsDirectoryA(LPSTR buf, UINT n) { strncpy(buf, "C:\\WINDOWS", n); return 10; }
DWORD WINAPI GetTempPathA(DWORD n, LPSTR buf) { strncpy(buf, "C:\\TEMP\\", n); return 8; }
UINT WINAPI GetTempFileNameA(LPCSTR path, LPCSTR prefix, UINT unique, LPSTR out)
{
	static unsigned counter = 1;
	unsigned u = unique ? unique : (counter++ & 0xffff);
	char pre[4] = {0}; if (prefix) strncpy(pre, prefix, 3);
	snprintf(out, MAX_PATH, "%s%s%s%04X.TMP", path, (path[0] && path[strlen(path) - 1] != '\\') ? "\\" : "", pre, u);
	if (!unique)
	{
		HANDLE f = CreateFileA(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
	}
	return u;
}

// ------------------------------------------------------------------ module image
static BYTE* g_image = nullptr;
static DWORD g_imageSize = 0;
static std::string g_exeName = "C:\\PETER\\PROGRAM.EXE";
static std::string g_cmdLine;

// Program image sources:
//  - a Windows PE file (the original game EXE), mapped section by section, or
//  - a "PETPROG1" data container holding only the non-code sections the runtime
//    needs (.rsrc resources and the .petprg Peter program), as used by the web build.
struct SecRec { char name[9]; DWORD rva, size; };
static std::vector<SecRec> g_secs;
static DWORD g_resRva = 0;

static bool load_pe(const std::vector<BYTE>& raw)
{
	auto* dos = (IMAGE_DOS_HEADER*)raw.data();
	auto* nt = (IMAGE_NT_HEADERS32*)(raw.data() + dos->e_lfanew);
	g_imageSize = nt->OptionalHeader.SizeOfImage;
	if (posix_memalign((void**)&g_image, 65536, g_imageSize) != 0) return false;
	memset(g_image, 0, g_imageSize);
	auto* sec = IMAGE_FIRST_SECTION(nt);
	for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
	{
		DWORD n = sec->SizeOfRawData;
		if (sec->VirtualAddress + n > g_imageSize) n = g_imageSize - sec->VirtualAddress;
		if (sec->PointerToRawData + n > (DWORD)raw.size()) n = (DWORD)raw.size() - sec->PointerToRawData;
		memcpy(g_image + sec->VirtualAddress, raw.data() + sec->PointerToRawData, n);
		SecRec r{}; memcpy(r.name, sec->Name, 8); r.rva = sec->VirtualAddress; r.size = sec->SizeOfRawData;
		g_secs.push_back(r);
	}
	g_resRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress;
	return true;
}

static bool load_container(const std::vector<BYTE>& raw)
{
	// "PETPROG1" | u32 imageSize | u32 resourceRva | u32 count | count x (name[8] u32 rva u32 size bytes[size])
	size_t p = 8;
	auto u32 = [&](DWORD& v) { if (p + 4 > raw.size()) return false; memcpy(&v, raw.data() + p, 4); p += 4; return true; };
	DWORD count;
	if (!u32(g_imageSize) || !u32(g_resRva) || !u32(count)) return false;
	if (posix_memalign((void**)&g_image, 65536, g_imageSize) != 0) return false;
	memset(g_image, 0, g_imageSize);
	for (DWORD i = 0; i < count; i++)
	{
		SecRec r{};
		if (p + 8 > raw.size()) return false;
		memcpy(r.name, raw.data() + p, 8); p += 8;
		if (!u32(r.rva) || !u32(r.size)) return false;
		if (p + r.size > raw.size() || r.rva + r.size > g_imageSize) return false;
		memcpy(g_image + r.rva, raw.data() + p, r.size); p += r.size;
		g_secs.push_back(r);
	}
	return true;
}

bool shim_load_image(const char* winpath, const char* unixImage)
{
	g_exeName = winpath;
	g_cmdLine = std::string("\"") + winpath + "\"";
	std::string up = unixImage && *unixImage ? std::string(unixImage) : win_to_unix(winpath);
	FILE* f = fopen(up.c_str(), "rb");
	if (!f) { SHIM_LOG("cannot open %s", up.c_str()); return false; }
	fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
	std::vector<BYTE> raw(len); fread(raw.data(), 1, len, f); fclose(f);
	if (len >= 8 && memcmp(raw.data(), "PETPROG1", 8) == 0) return load_container(raw);
	if (len >= 2 && raw[0] == 'M' && raw[1] == 'Z') return load_pe(raw);
	SHIM_LOG("unknown program image format: %s", up.c_str());
	return false;
}
BYTE* shim_image() { return g_image; }

// locate a section of the mapped image
BYTE* shim_find_section(const char* name, DWORD* size)
{
	if (!g_image) return nullptr;
	for (auto& r : g_secs)
		if (strncmp(r.name, name, 8) == 0) { if (size) *size = r.size; return g_image + r.rva; }
	return nullptr;
}

HMODULE WINAPI GetModuleHandleA(LPCSTR name) { return name ? NULL : (HMODULE)g_image; }
DWORD WINAPI GetModuleFileNameA(HMODULE h, LPSTR buf, DWORD n)
{
	strncpy(buf, g_exeName.c_str(), n);
	return (DWORD)strlen(buf);
}
LPSTR WINAPI GetCommandLineA(void) { return (LPSTR)g_cmdLine.c_str(); }

// --------- PE resources
static IMAGE_RESOURCE_DIRECTORY* res_root()
{
	if (!g_image || !g_resRva) return nullptr;
	return (IMAGE_RESOURCE_DIRECTORY*)(g_image + g_resRva);
}
static bool res_name_eq(BYTE* base, IMAGE_RESOURCE_DIRECTORY_ENTRY* e, LPCSTR name)
{
	if (IS_INTRESOURCE(name)) return !e->NameIsString && e->Id == (WORD)(uintptr_t)name;
	if (!e->NameIsString) { if (name[0] == '#') return e->Id == (WORD)atoi(name + 1); return false; }
	auto* s = (IMAGE_RESOURCE_DIR_STRING_U*)(base + e->NameOffset);
	size_t n = strlen(name); if (n != s->Length) return false;
	for (size_t i = 0; i < n; i++) if (toupper((unsigned char)name[i]) != toupper(s->NameString[i])) return false;
	return true;
}
static IMAGE_RESOURCE_DIRECTORY_ENTRY* res_find(BYTE* base, IMAGE_RESOURCE_DIRECTORY* dir, LPCSTR name)
{
	auto* e = (IMAGE_RESOURCE_DIRECTORY_ENTRY*)(dir + 1);
	int n = dir->NumberOfNamedEntries + dir->NumberOfIdEntries;
	for (int i = 0; i < n; i++) if (name == nullptr || res_name_eq(base, e + i, name)) return e + i;
	return nullptr;
}
HRSRC WINAPI FindResourceA(HMODULE h, LPCSTR name, LPCSTR type)
{
	auto* root = res_root(); if (!root) return NULL;
	BYTE* base = (BYTE*)root;
	auto* e = res_find(base, root, type); if (!e || !e->DataIsDirectory) return NULL;
	auto* d2 = (IMAGE_RESOURCE_DIRECTORY*)(base + e->OffsetToDirectory);
	e = res_find(base, d2, name); if (!e || !e->DataIsDirectory) return NULL;
	auto* d3 = (IMAGE_RESOURCE_DIRECTORY*)(base + e->OffsetToDirectory);
	e = res_find(base, d3, nullptr); if (!e) return NULL;
	return (HRSRC)(base + e->OffsetToData);
}
HGLOBAL WINAPI LoadResource(HMODULE h, HRSRC r)
{
	if (!r) return NULL;
	auto* de = (IMAGE_RESOURCE_DATA_ENTRY*)r;
	return (HGLOBAL)(g_image + de->OffsetToData);
}
LPVOID WINAPI LockResource(HGLOBAL h) { return h; }
DWORD WINAPI SizeofResource(HMODULE h, HRSRC r) { return r ? ((IMAGE_RESOURCE_DATA_ENTRY*)r)->Size : 0; }
int WINAPI LoadStringA(HINSTANCE h, UINT id, LPSTR buf, int n)
{
	HRSRC r = FindResourceA(h, MAKEINTRESOURCEA((id >> 4) + 1), (LPCSTR)RT_STRING);
	if (!r || n <= 0) { if (n > 0) buf[0] = 0; return 0; }
	WCHAR* p = (WCHAR*)LoadResource(h, r);
	for (UINT i = 0; i < (id & 15); i++) p += *p + 1;
	int len = *p++;
	int m = len < n - 1 ? len : n - 1;
	WideCharToMultiByte(CP_ACP, 0, p, m, buf, m, NULL, NULL);
	buf[m] = 0;
	return m;
}

// ------------------------------------------------------------------ libraries
#define FAKE_GL_LIB ((HMODULE)0x0F1E0001)
#define FAKE_KERNEL ((HMODULE)0x0F1E0002)
HMODULE WINAPI LoadLibraryA(LPCSTR name)
{
	std::string n = name; for (auto& c : n) c = (char)toupper((unsigned char)c);
	if (n.find("OPENGL32") != std::string::npos) return FAKE_GL_LIB;
	if (n.find("KERNEL32") != std::string::npos) return FAKE_KERNEL;
	SHIM_LOG("[shim] LoadLibrary(%s) -> not available", name);
	return NULL;
}
WINBOOL WINAPI FreeLibrary(HMODULE h) { return TRUE; }
FARPROC WINAPI GetProcAddress(HMODULE h, LPCSTR name)
{
	if (h == FAKE_GL_LIB) { void* p = gl_get_proc(name); if (!p) SHIM_LOG("[gl] missing %s", name); return (FARPROC)p; }
	return NULL;
}

// ------------------------------------------------------------------ process
VOID WINAPI ExitProcess(UINT code)
{
	SHIM_LOG("ExitProcess(%u)", code);
	EM_ASM({ if (Module.onPeterExit) Module.onPeterExit($0); }, code);
	emscripten_force_exit((int)code);
	for (;;) emscripten_sleep(1000);
}
WINBOOL WINAPI CreateProcessA(LPCSTR a, LPSTR b, LPSECURITY_ATTRIBUTES c, LPSECURITY_ATTRIBUTES d, WINBOOL e, DWORD f, LPVOID g, LPCSTR h, LPSTARTUPINFOA i, LPPROCESS_INFORMATION j) { UNIMPL(); return FALSE; }
WINBOOL WINAPI GetExitCodeProcess(HANDLE h, LPDWORD c) { *c = 0; return FALSE; }

// ------------------------------------------------------------------ console (unsupported)
WINBOOL WINAPI AllocConsole(void) { return FALSE; }
WINBOOL WINAPI FreeConsole(void) { return TRUE; }
HANDLE WINAPI GetStdHandle(DWORD n) { return INVALID_HANDLE_VALUE; }
WINBOOL WINAPI SetConsoleTitleA(LPCSTR t) { return TRUE; }

// ------------------------------------------------------------------ COM (DirectPlay etc. unavailable)
HRESULT WINAPI CoInitialize(LPVOID p) { return S_OK; }
void WINAPI CoUninitialize(void) {}
HRESULT WINAPI CoCreateInstance(REFCLSID a, LPUNKNOWN b, DWORD c, REFIID d, LPVOID* e) { if (e) *e = NULL; return E_NOTIMPL; }
const GUID CLSID_DirectPlay = {0};
const GUID IID_IDirectPlay3A = {0};
const GUID IID_IDirect3DHALDevice = {0}, IID_IDirect3DMMXDevice = {0}, IID_IDirect3DRGBDevice = {0},
	IID_IDirect3DRampDevice = {0}, IID_IDirect3DRefDevice = {0}, IID_IDirect3DTnLHalDevice = {0};

// ------------------------------------------------------------------ serial port (unsupported)
WINBOOL WINAPI ClearCommError(HANDLE h, LPDWORD e, LPCOMSTAT s) { return FALSE; }
WINBOOL WINAPI EscapeCommFunction(HANDLE h, DWORD f) { return FALSE; }
WINBOOL WINAPI GetCommModemStatus(HANDLE h, LPDWORD s) { return FALSE; }
WINBOOL WINAPI GetCommState(HANDLE h, LPDCB d) { return FALSE; }
WINBOOL WINAPI PurgeComm(HANDLE h, DWORD f) { return FALSE; }
WINBOOL WINAPI SetCommMask(HANDLE h, DWORD m) { return FALSE; }
WINBOOL WINAPI SetCommState(HANDLE h, LPDCB d) { return FALSE; }
WINBOOL WINAPI SetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS t) { return FALSE; }
WINBOOL WINAPI SetupComm(HANDLE h, DWORD a, DWORD b) { return FALSE; }

// ------------------------------------------------------------------ atoms
UINT WINAPI GlobalGetAtomNameA(ATOM a, LPSTR buf, int n) { if (n > 0) buf[0] = 0; return 0; }

// ------------------------------------------------------------------ registry (in-memory)
struct RegKey { std::string path; };
static std::map<std::string, std::map<std::string, std::pair<DWORD, std::vector<BYTE>>>> g_reg;
static std::string reg_path(HKEY parent, LPCSTR sub)
{
	std::string base;
	if (parent == HKEY_LOCAL_MACHINE) base = "HKLM";
	else if (parent == HKEY_CURRENT_USER) base = "HKCU";
	else if (parent == HKEY_CLASSES_ROOT) base = "HKCR";
	else if (parent == HKEY_USERS) base = "HKU";
	else base = ((RegKey*)parent)->path;
	std::string s = base + "\\" + (sub ? sub : "");
	for (auto& c : s) c = (char)toupper((unsigned char)c);
	return s;
}
LONG WINAPI RegOpenKeyExA(HKEY h, LPCSTR sub, DWORD o, REGSAM s, PHKEY out)
{
	std::string p = reg_path(h, sub);
	if (!g_reg.count(p)) return ERROR_FILE_NOT_FOUND;
	*out = (HKEY)new RegKey{p};
	return ERROR_SUCCESS;
}
LONG WINAPI RegCreateKeyExA(HKEY h, LPCSTR sub, DWORD r, LPSTR cls, DWORD o, REGSAM s, LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp)
{
	std::string p = reg_path(h, sub);
	if (disp) *disp = g_reg.count(p) ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
	g_reg[p];
	*out = (HKEY)new RegKey{p};
	return ERROR_SUCCESS;
}
LONG WINAPI RegCloseKey(HKEY h) { if ((uintptr_t)h < 0x80000000u) delete (RegKey*)h; return ERROR_SUCCESS; }
LONG WINAPI RegQueryValueExA(HKEY h, LPCSTR name, LPDWORD r, LPDWORD type, LPBYTE data, LPDWORD size)
{
	auto& k = g_reg[((RegKey*)h)->path];
	std::string n = name ? name : ""; for (auto& c : n) c = (char)toupper((unsigned char)c);
	auto it = k.find(n); if (it == k.end()) return ERROR_FILE_NOT_FOUND;
	if (type) *type = it->second.first;
	DWORD need = (DWORD)it->second.second.size();
	if (data) { if (!size || *size < need) { if (size) *size = need; return ERROR_MORE_DATA; } memcpy(data, it->second.second.data(), need); }
	if (size) *size = need;
	return ERROR_SUCCESS;
}
LONG WINAPI RegSetValueExA(HKEY h, LPCSTR name, DWORD r, DWORD type, const BYTE* data, DWORD size)
{
	std::string n = name ? name : ""; for (auto& c : n) c = (char)toupper((unsigned char)c);
	g_reg[((RegKey*)h)->path][n] = {type, std::vector<BYTE>(data, data + size)};
	return ERROR_SUCCESS;
}
LONG WINAPI RegDeleteValueA(HKEY h, LPCSTR name)
{
	std::string n = name ? name : ""; for (auto& c : n) c = (char)toupper((unsigned char)c);
	g_reg[((RegKey*)h)->path].erase(n);
	return ERROR_SUCCESS;
}

// ------------------------------------------------------------------ INI files
static std::string trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return "";
	size_t b = s.find_last_not_of(" \t\r\n"); return s.substr(a, b - a + 1);
}
static bool ieq(const std::string& a, const std::string& b)
{
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
	return true;
}
static std::vector<std::string> ini_read(LPCSTR file)
{
	std::vector<std::string> lines;
	std::string p = win_to_unix(file && strchr(file, '\\') ? file : (std::string("C:\\WINDOWS\\") + (file ? file : "win.ini")).c_str());
	FILE* f = fopen(p.c_str(), "rb"); if (!f) return lines;
	char buf[4096]; while (fgets(buf, sizeof buf, f)) { std::string s = buf; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); lines.push_back(s); }
	fclose(f); return lines;
}
DWORD WINAPI GetPrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR def, LPSTR out, DWORD n, LPCSTR file)
{
	auto lines = ini_read(file);
	std::string cur; bool insec = false;
	for (auto& l : lines)
	{
		std::string t = trim(l);
		if (!t.empty() && t[0] == '[') { insec = ieq(trim(t.substr(1, t.find(']') - 1)), sec ? sec : ""); continue; }
		if (!insec) continue;
		size_t eq = t.find('='); if (eq == std::string::npos) continue;
		if (key && ieq(trim(t.substr(0, eq)), key))
		{
			std::string v = trim(t.substr(eq + 1));
			strncpy(out, v.c_str(), n); if (n) out[n - 1] = 0; return (DWORD)strlen(out);
		}
	}
	strncpy(out, def ? def : "", n); if (n) out[n - 1] = 0;
	return (DWORD)strlen(out);
}
WINBOOL WINAPI WritePrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR val, LPCSTR file)
{
	auto lines = ini_read(file);
	int secStart = -1, secEnd = (int)lines.size(), keyLine = -1;
	for (int i = 0; i < (int)lines.size(); i++)
	{
		std::string t = trim(lines[i]);
		if (!t.empty() && t[0] == '[')
		{
			if (secStart >= 0) { secEnd = i; break; }
			if (ieq(trim(t.substr(1, t.find(']') - 1)), sec)) secStart = i;
			continue;
		}
		if (secStart >= 0 && key)
		{
			size_t eq = t.find('=');
			if (eq != std::string::npos && ieq(trim(t.substr(0, eq)), key)) keyLine = i;
		}
	}
	if (secStart < 0) { if (!key) return TRUE; lines.push_back(std::string("[") + sec + "]"); secStart = (int)lines.size() - 1; secEnd = (int)lines.size(); }
	if (!key) { lines.erase(lines.begin() + secStart, lines.begin() + secEnd); }
	else if (keyLine >= 0) { if (val) lines[keyLine] = std::string(key) + "=" + val; else lines.erase(lines.begin() + keyLine); }
	else if (val) lines.insert(lines.begin() + secEnd, std::string(key) + "=" + val);
	std::string p = win_to_unix(file && strchr(file, '\\') ? file : (std::string("C:\\WINDOWS\\") + (file ? file : "win.ini")).c_str());
	FILE* f = fopen(p.c_str(), "wb"); if (!f) return FALSE;
	for (auto& l : lines) fprintf(f, "%s\r\n", l.c_str());
	fclose(f);
	EM_ASM({ if (Module.persistFS) Module.persistFS(); });
	return TRUE;
}

} // extern "C"

DWORD shim_ticks() { return timeGetTime(); }

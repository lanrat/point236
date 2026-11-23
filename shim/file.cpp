// Win32 shim: file system on top of Emscripten's virtual FS (C:\ -> /c/, case-insensitive by lowercasing)
#include "shim.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <utime.h>
#include <fnmatch.h>

static std::string g_cwd = "C:\\";

static std::string norm_win(const char* path)
{
	std::string p = path ? path : "";
	for (auto& c : p) if (c == '/') c = '\\';
	if (p.size() >= 2 && p[1] == ':') { if (p.size() == 2 || p[2] != '\\') p.insert(2, "\\"); }	// "C:x" -> "C:\x"
	else if (!p.empty() && p[0] == '\\') p = g_cwd.substr(0, 2) + p;
	else { std::string c = g_cwd; if (c.back() != '\\') c += '\\'; p = c + p; }
	// resolve . and ..
	std::vector<std::string> parts; size_t i = 3;
	std::string drive = p.substr(0, 2);
	while (i <= p.size())
	{
		size_t j = p.find('\\', i); if (j == std::string::npos) j = p.size();
		std::string s = p.substr(i, j - i);
		if (s == "..") { if (!parts.empty()) parts.pop_back(); }
		else if (!s.empty() && s != ".") parts.push_back(s);
		i = j + 1;
	}
	std::string r = drive + "\\";
	for (size_t k = 0; k < parts.size(); k++) { if (k) r += "\\"; r += parts[k]; }
	return r;
}

std::string win_to_unix(const char* path)
{
	std::string w = norm_win(path);
	std::string u = "/";
	u += (char)tolower((unsigned char)w[0]);
	for (size_t i = 2; i < w.size(); i++) u += (w[i] == '\\') ? '/' : (char)tolower((unsigned char)w[i]);
	if (u.size() > 1 && u.back() == '/') u.pop_back();
	return u;
}

struct FileH { uint32_t magic = 0x46494c45; int fd = -1; std::string path; bool write = false; };
static FileH* fh(HANDLE h) { return checked<FileH>(h, 0x46494c45); }

static void persist_soon() { EM_ASM({ if (Module.persistFS) Module.persistFS(); }); }

static DWORD errno_to_win()
{
	switch (errno) { case ENOENT: return ERROR_FILE_NOT_FOUND; case EEXIST: return ERROR_FILE_EXISTS; case EACCES: return ERROR_ACCESS_DENIED; default: return ERROR_GEN_FAILURE; }
}

extern "C" {

HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
	std::string up = win_to_unix(name);
	int fl = 0;
	if ((access & GENERIC_WRITE) && (access & GENERIC_READ)) fl = O_RDWR;
	else if (access & GENERIC_WRITE) fl = O_WRONLY;
	else fl = O_RDONLY;
	switch (disp)
	{
	case CREATE_NEW: fl |= O_CREAT | O_EXCL; break;
	case CREATE_ALWAYS: fl |= O_CREAT | O_TRUNC; break;
	case OPEN_ALWAYS: fl |= O_CREAT; break;
	case TRUNCATE_EXISTING: fl |= O_TRUNC; break;
	default: break;
	}
	struct stat st;
	if (stat(up.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
	{
		// directory handle (used for SetFileTime on directories)
		FileH* f = new FileH; f->path = up; f->fd = -2; return (HANDLE)f;
	}
	int fd = open(up.c_str(), fl, 0666);
	if (fd < 0) { SetLastError(errno_to_win()); return INVALID_HANDLE_VALUE; }
	FileH* f = new FileH; f->fd = fd; f->path = up; f->write = (access & GENERIC_WRITE) != 0;
	if (disp == CREATE_ALWAYS || disp == OPEN_ALWAYS) SetLastError(0);
	return (HANDLE)f;
}
WINBOOL WINAPI CloseHandle(HANDLE h)
{
	FileH* f = fh(h); if (!f) return TRUE;
	if (f->fd >= 0) close(f->fd);
	if (f->write && f->path.rfind("/c/temp/", 0) != 0) persist_soon();
	f->magic = 0; delete f; return TRUE;
}
WINBOOL WINAPI ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED o)
{
	FileH* f = fh(h); if (!f || f->fd < 0) { if (got) *got = 0; return FALSE; }
	ssize_t r = read(f->fd, buf, n);
	if (got) *got = r > 0 ? (DWORD)r : 0;
	return r >= 0;
}
WINBOOL WINAPI WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED o)
{
	FileH* f = fh(h); if (!f || f->fd < 0) { if (put) *put = 0; return FALSE; }
	ssize_t r = write(f->fd, buf, n);
	if (put) *put = r > 0 ? (DWORD)r : 0;
	return r >= 0;
}
DWORD WINAPI SetFilePointer(HANDLE h, LONG dist, PLONG high, DWORD method)
{
	FileH* f = fh(h); if (!f || f->fd < 0) return INVALID_SET_FILE_POINTER;
	long long d = dist; if (high) d = ((long long)*high << 32) | (DWORD)dist;
	int wh = method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END;
	off_t r = lseek(f->fd, (off_t)d, wh);
	if (r < 0) return INVALID_SET_FILE_POINTER;
	if (high) *high = (LONG)((long long)r >> 32);
	return (DWORD)r;
}
WINBOOL WINAPI SetEndOfFile(HANDLE h)
{
	FileH* f = fh(h); if (!f || f->fd < 0) return FALSE;
	off_t pos = lseek(f->fd, 0, SEEK_CUR);
	return ftruncate(f->fd, pos) == 0;
}
DWORD WINAPI GetFileSize(HANDLE h, LPDWORD high)
{
	FileH* f = fh(h); if (!f || f->fd < 0) return INVALID_FILE_SIZE;
	struct stat st; fstat(f->fd, &st);
	if (high) *high = 0;
	return (DWORD)st.st_size;
}
DWORD WINAPI GetFileType(HANDLE h) { return fh(h) ? FILE_TYPE_DISK : FILE_TYPE_UNKNOWN; }

static void unix_to_ft(time_t t, FILETIME* ft)
{
	unsigned long long v = ((unsigned long long)t + 11644473600ULL) * 10000000ULL;
	ft->dwLowDateTime = (DWORD)v; ft->dwHighDateTime = (DWORD)(v >> 32);
}
static time_t ft_to_unix(const FILETIME* ft)
{
	unsigned long long v = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
	return (time_t)(v / 10000000ULL - 11644473600ULL);
}
WINBOOL WINAPI GetFileTime(HANDLE h, LPFILETIME c, LPFILETIME a, LPFILETIME w)
{
	FileH* f = fh(h); if (!f) return FALSE;
	struct stat st; if (stat(f->path.c_str(), &st) != 0) return FALSE;
	if (c) unix_to_ft(st.st_ctime, c);
	if (a) unix_to_ft(st.st_atime, a);
	if (w) unix_to_ft(st.st_mtime, w);
	return TRUE;
}
WINBOOL WINAPI SetFileTime(HANDLE h, const FILETIME* c, const FILETIME* a, const FILETIME* w)
{
	FileH* f = fh(h); if (!f) return FALSE;
	struct stat st; if (stat(f->path.c_str(), &st) != 0) return FALSE;
	struct utimbuf ub; ub.actime = a ? ft_to_unix(a) : st.st_atime; ub.modtime = w ? ft_to_unix(w) : st.st_mtime;
	utime(f->path.c_str(), &ub);
	return TRUE;
}

DWORD WINAPI GetFileAttributesA(LPCSTR name)
{
	struct stat st; if (stat(win_to_unix(name).c_str(), &st) != 0) return INVALID_FILE_ATTRIBUTES;
	return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
}
WINBOOL WINAPI SetFileAttributesA(LPCSTR name, DWORD a) { return GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES; }
WINBOOL WINAPI DeleteFileA(LPCSTR name) { std::string u = win_to_unix(name); bool ok = unlink(u.c_str()) == 0; if (u.rfind("/c/temp/", 0) != 0) persist_soon(); return ok; }
WINBOOL WINAPI MoveFileA(LPCSTR a, LPCSTR b) { bool ok = rename(win_to_unix(a).c_str(), win_to_unix(b).c_str()) == 0; persist_soon(); return ok; }
WINBOOL WINAPI CopyFileA(LPCSTR a, LPCSTR b, WINBOOL failIfExists)
{
	std::string ua = win_to_unix(a), ub = win_to_unix(b);
	struct stat st; if (failIfExists && stat(ub.c_str(), &st) == 0) return FALSE;
	FILE* fi = fopen(ua.c_str(), "rb"); if (!fi) return FALSE;
	FILE* fo = fopen(ub.c_str(), "wb"); if (!fo) { fclose(fi); return FALSE; }
	char buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, fi)) > 0) fwrite(buf, 1, n, fo);
	fclose(fi); fclose(fo); persist_soon(); return TRUE;
}
WINBOOL WINAPI CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa) { bool ok = mkdir(win_to_unix(name).c_str(), 0777) == 0; persist_soon(); return ok; }
WINBOOL WINAPI RemoveDirectoryA(LPCSTR name) { return rmdir(win_to_unix(name).c_str()) == 0; }
DWORD WINAPI GetCurrentDirectoryA(DWORD n, LPSTR buf)
{
	if (n <= g_cwd.size()) return (DWORD)g_cwd.size() + 1;
	strcpy(buf, g_cwd.c_str()); return (DWORD)g_cwd.size();
}
WINBOOL WINAPI SetCurrentDirectoryA(LPCSTR name)
{
	std::string w = norm_win(name);
	struct stat st; if (stat(win_to_unix(w.c_str()).c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return FALSE;
	g_cwd = w; return TRUE;
}
DWORD WINAPI GetFullPathNameA(LPCSTR name, DWORD n, LPSTR buf, LPSTR* filepart)
{
	std::string w = norm_win(name);
	if (n <= w.size()) return (DWORD)w.size() + 1;
	strcpy(buf, w.c_str());
	if (filepart) { char* p = strrchr(buf, '\\'); *filepart = p ? p + 1 : buf; }
	return (DWORD)w.size();
}

// ---------------- find files
struct FindH { uint32_t magic = 0x46494e44; std::string dir, pat; DIR* d = nullptr; };
static bool find_next(FindH* f, LPWIN32_FIND_DATAA fd)
{
	struct dirent* e;
	while ((e = readdir(f->d)) != nullptr)
	{
		std::string n = e->d_name;
		std::string lp = f->pat;
		if (fnmatch(lp.c_str(), n.c_str(), FNM_CASEFOLD) != 0 && !(lp == "*.*" && true)) continue;
		if (lp == "*.*" || fnmatch(lp.c_str(), n.c_str(), FNM_CASEFOLD) == 0)
		{
			memset(fd, 0, sizeof(*fd));
			std::string full = f->dir + "/" + n;
			struct stat st; stat(full.c_str(), &st);
			fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
			fd->nFileSizeLow = (DWORD)st.st_size;
			unix_to_ft(st.st_mtime, &fd->ftLastWriteTime);
			unix_to_ft(st.st_ctime, &fd->ftCreationTime);
			unix_to_ft(st.st_atime, &fd->ftLastAccessTime);
			strncpy(fd->cFileName, n.c_str(), MAX_PATH - 1);
			strncpy(fd->cAlternateFileName, n.c_str(), 13);
			return true;
		}
	}
	return false;
}
HANDLE WINAPI FindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA fd)
{
	std::string w = norm_win(name);
	size_t k = w.rfind('\\');
	std::string dir = w.substr(0, k), pat = w.substr(k + 1);
	if (dir.size() == 2) dir += "\\";
	FindH* f = new FindH; f->dir = win_to_unix(dir.c_str()); f->pat = pat;
	for (auto& c : f->pat) c = (char)tolower((unsigned char)c);
	f->d = opendir(f->dir.c_str());
	if (!f->d || !find_next(f, fd)) { if (f->d) closedir(f->d); delete f; SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
	return (HANDLE)f;
}
WINBOOL WINAPI FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA fd)
{
	FindH* f = checked<FindH>(h, 0x46494e44); if (!f) return FALSE;
	if (!find_next(f, fd)) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
	return TRUE;
}
WINBOOL WINAPI FindClose(HANDLE h)
{
	FindH* f = checked<FindH>(h, 0x46494e44); if (!f) return FALSE;
	if (f->d) closedir(f->d); f->magic = 0; delete f; return TRUE;
}

// ---------------- file mapping (read whole file)
struct MapH { uint32_t magic = 0x4d415048; std::vector<BYTE> data; };
HANDLE WINAPI CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name)
{
	FileH* f = fh(file); if (!f || f->fd < 0) return NULL;
	MapH* m = new MapH;
	struct stat st; fstat(f->fd, &st);
	m->data.resize(st.st_size);
	off_t cur = lseek(f->fd, 0, SEEK_CUR); lseek(f->fd, 0, SEEK_SET);
	read(f->fd, m->data.data(), st.st_size); lseek(f->fd, cur, SEEK_SET);
	return (HANDLE)m;
}
LPVOID WINAPI MapViewOfFile(HANDLE h, DWORD access, DWORD hi, DWORD lo, SIZE_T n)
{
	MapH* m = checked<MapH>(h, 0x4d415048); if (!m) return NULL;
	return m->data.data() + lo;
}
WINBOOL WINAPI UnmapViewOfFile(LPCVOID p) { return TRUE; }

// ---------------- drives
WINBOOL WINAPI GetDiskFreeSpaceA(LPCSTR root, LPDWORD spc, LPDWORD bps, LPDWORD freec, LPDWORD totalc)
{
	if (spc) *spc = 8; if (bps) *bps = 512; if (freec) *freec = 0x40000; if (totalc) *totalc = 0x80000;
	return TRUE;
}
UINT WINAPI GetDriveTypeA(LPCSTR root) { return (root && toupper(root[0]) == 'C') ? DRIVE_FIXED : DRIVE_NO_ROOT_DIR; }
DWORD WINAPI GetLogicalDrives(void) { return 1 << 2; }
WINBOOL WINAPI GetVolumeInformationA(LPCSTR root, LPSTR name, DWORD nn, LPDWORD serial, LPDWORD maxlen, LPDWORD flags, LPSTR fs, DWORD fsn)
{
	if (name && nn) strncpy(name, "BROWSER", nn);
	if (serial) *serial = 0x12345678;
	if (maxlen) *maxlen = 255;
	if (flags) *flags = 0;
	if (fs && fsn) strncpy(fs, "FAT32", fsn);
	return TRUE;
}
WINBOOL WINAPI SetVolumeLabelA(LPCSTR root, LPCSTR name) { return FALSE; }

} // extern "C"

void shim_set_cwd(const char* w) { g_cwd = norm_win(w); }

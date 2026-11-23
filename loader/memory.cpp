// wasm port: Peter's region-based memory manager replaced by the C allocator.
// (Global C++ objects are constructed before main() under Emscripten, so the
//  allocator must work without MemInit().)
#include "main.h"
#include <stdlib.h>

bool MemoryOK = true;

bool MemInit() { MemoryOK = true; return true; }
void MemTerm() { MemoryOK = false; }

void MemErr()
{
	::MessageBox(NULL, "Out of memory", "Error", MB_OK | MB_ICONSTOP);
	Exit(EXITCODE_MEMERR);
}

// Peter's original allocator rounded small blocks up to power-of-two sizes, and the
// runtime relies on that slack (small overruns go unnoticed there), so do the same.
static inline size_t PadSize(int size)
{
	size_t s = size > 8 ? (size_t)size : 8;
	if (s <= 65536) { size_t p = 8; while (p < s) p <<= 1; return p; }
	return s + 256;
}

void* _fastcall MemGet(int size)
{
	void* p = malloc(PadSize(size));
	if (p == NULL) MemErr();
	return p;
}

void _fastcall MemFree(void* adr)
{
	free(adr);
}

void* _fastcall MemSize(void* adr, int size)
{
	if (adr == NULL)
	{
		if (size <= 0) return NULL;
		return MemGet(size);
	}
	if (size <= 0)
	{
		MemFree(adr);
		return NULL;
	}
	void* p = realloc(adr, PadSize(size));
	if (p == NULL) MemErr();
	return p;
}

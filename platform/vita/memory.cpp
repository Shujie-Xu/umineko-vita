#include "platform/vita/memory.hpp"

#include <SDL2/SDL_error.h>
#include <SDL2/SDL_stdinc.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>

// Budget measured from the 2026-10-08 exit core dump (ATTRIBUTE2=12):
//   Newlib heap    USER_RW          192 MiB  (platform.cpp, fixed at boot)
//   ram_mempool    USER_RW_UNCACHE  136 MiB  (free user RAM minus SDL's 12 MiB)
//   cdram_mempool  USER_CDRAM_RW     96 MiB  (+16 MiB sceGxm parameter buffer)
//   phycont_mempool PHYCONT_NC       12 MiB  (+14 MiB kept for AvPlayer, gl_init.c)
// No user RAM is left idle: the heap can only grow by shrinking vitaGL's RAM
// pool one for one. vitaGL already spills GPU data into newlib when its pools
// are full (use_extra_mem); this gives newlib the opposite route. SDL's
// allocations (SDL_mixer's whole-file chunk decodes, SDL_image and SDL_gpu
// surfaces) try newlib first, unchanged, and only a request newlib cannot place
// in one contiguous block is served by vglMalloc/vglCalloc from vitaGL's RAM,
// PHYCONT and finally CDRAM pools. Those are uncached for the CPU, which costs
// little for sequentially mixed audio and turns a fatal "Out of memory" into a
// slower load. vitaGL's own GC thread already frees from these mspaces while
// the render thread allocates, so they are used here from other threads too.
// C++ operator new/delete get the same treatment below: the engine reads whole
// files with new[] (FileIO::readFile), and a failed new ends the game through
// Loader.cpp's new-handler.
namespace {
uintptr_t newlibBase;
uintptr_t newlibEnd;
std::atomic<size_t> spilled{0};
std::atomic<bool> spillReported{false};
std::atomic<int> softThread{0}; // SceUID with a SoftAllocations, or 0

// Only consulted once newlib has failed, so it costs nothing otherwise.
bool soft() {
	const int thread = softThread.load();
	return thread && thread == sceKernelGetThreadId();
}

bool inNewlib(const void *ptr) {
	const auto address = reinterpret_cast<uintptr_t>(ptr);
	return address >= newlibBase && address < newlibEnd;
}

// vitaGL's pools exist only after SDL created the GL context. Without the
// heap's bounds, frees could not be routed, so nothing may spill either.
bool poolsReady() {
	return newlibEnd && (vglMemTotal(VGL_MEM_RAM) || vglMemTotal(VGL_MEM_VRAM));
}

void *accountSpill(void *ptr, size_t size) {
	// vglMalloc retries newlib first; only pool blocks are counted.
	if (!ptr || inNewlib(ptr))
		return ptr;
	spilled += vglMallocUsableSize(ptr);
	if (!spillReported.exchange(true)) {
		std::fprintf(stderr, "Memory: newlib heap could not place %u bytes; using vitaGL's pools\n", static_cast<unsigned>(size));
		VitaMemory::logBudget("first spill");
	}
	return ptr;
}

void *SDLCALL allocate(size_t size) {
	if (void *ptr = std::malloc(size))
		return ptr;
	if (!size || !poolsReady() || soft())
		return nullptr;
	return accountSpill(vglMalloc(size), size);
}

void *SDLCALL allocateZeroed(size_t count, size_t size) {
	if (void *ptr = std::calloc(count, size))
		return ptr;
	if (!count || !size || size > SIZE_MAX / count || !poolsReady() || soft())
		return nullptr;
	return accountSpill(vglCalloc(count, size), count * size);
}

void SDLCALL release(void *ptr) {
	if (!ptr)
		return;
	// Before installSdlAllocator() ran, every block came from newlib.
	if (!newlibEnd || inNewlib(ptr)) {
		std::free(ptr);
		return;
	}
	spilled -= vglMallocUsableSize(ptr);
	vglFree(ptr);
}

void *SDLCALL reallocate(void *ptr, size_t size) {
	if (!ptr)
		return allocate(size);
	if (inNewlib(ptr)) {
		if (void *result = std::realloc(ptr, size))
			return result;
		if (!size || !poolsReady() || soft())
			return nullptr;
		void *result = accountSpill(vglMalloc(size), size);
		if (result) {
			std::memcpy(result, ptr, std::min(malloc_usable_size(ptr), size));
			std::free(ptr);
		}
		return result;
	}
	if (!size) {
		release(ptr);
		return nullptr;
	}
	// A pool block moves back to newlib when it fits there again. On failure
	// the original block stays valid, as realloc requires.
	const size_t old = vglMallocUsableSize(ptr);
	void *result = allocate(size);
	if (result) {
		std::memcpy(result, ptr, std::min(old, size));
		release(ptr);
	}
	return result;
}
}

namespace VitaMemory {
void installSdlAllocator() {
	void *probe = std::malloc(1);
	SceKernelMemBlockInfo info{};
	info.size = sizeof(info);
	const bool found = probe && sceKernelGetMemBlockInfoByAddr(probe, &info) >= 0;
	std::free(probe);
	// Without the heap's bounds release() could not route frees; keep SDL's
	// default allocator rather than guess.
	if (!found || !info.mappedBase || !info.mappedSize) {
		std::fprintf(stderr, "Memory: newlib heap bounds unavailable; SDL keeps the default allocator\n");
		return;
	}
	newlibBase = reinterpret_cast<uintptr_t>(info.mappedBase);
	newlibEnd = newlibBase + info.mappedSize;
	if (SDL_SetMemoryFunctions(allocate, allocateZeroed, reallocate, release) != 0)
		std::fprintf(stderr, "Memory: SDL_SetMemoryFunctions failed: %s\n", SDL_GetError());
}

void logBudget(const char *when) {
	SceKernelFreeMemorySizeInfo kernel{};
	kernel.size = sizeof(kernel);
	sceKernelGetFreeMemorySize(&kernel);
	const struct mallinfo heap = mallinfo();
	const auto kib = [](size_t bytes) { return static_cast<unsigned long>(bytes / 1024); };
	std::fprintf(stderr,
	             "Memory[%s]: newlib %lu KiB (in use %lu, free in arena %lu, untouched %lu); "
	             "kernel free user %lu, CDRAM %lu, PHYCONT %lu KiB; "
	             "vitaGL free/total RAM %lu/%lu, CDRAM %lu/%lu, PHYCONT %lu/%lu KiB; spilled to vitaGL pools %lu KiB\n",
	             when, kib(newlibEnd - newlibBase), kib(heap.uordblks), kib(heap.fordblks),
	             kib(newlibEnd - newlibBase > heap.arena ? newlibEnd - newlibBase - heap.arena : 0),
	             kib(kernel.size_user), kib(kernel.size_cdram), kib(kernel.size_phycont),
	             kib(vglMemFree(VGL_MEM_RAM)), kib(vglMemTotal(VGL_MEM_RAM)),
	             kib(vglMemFree(VGL_MEM_VRAM)), kib(vglMemTotal(VGL_MEM_VRAM)),
	             kib(vglMemFree(VGL_MEM_SLOW)), kib(vglMemTotal(VGL_MEM_SLOW)), kib(spilled.load()));
}

size_t spilledBytes() { return spilled.load(); }

SoftAllocations::SoftAllocations() { softThread = sceKernelGetThreadId(); }
SoftAllocations::~SoftAllocations() { softThread = 0; }
bool softAllocations() { return soft(); }
}

// Replaceable global allocation functions ([new.delete.single/array]). newlib
// first; a block it cannot place comes from vitaGL's pools before the
// new-handler (Loader.cpp: dialog, then std::terminate) is consulted. Deletes
// route by address, so blocks from either source, and blocks allocated during
// static initialisation, are all released correctly. Aligned (align_val_t)
// forms are left to libstdc++: they never spill and free through newlib.
void *operator new(std::size_t size) {
	if (!size)
		size = 1;
	for (;;) {
		if (void *ptr = std::malloc(size))
			return ptr;
		if (soft())
			throw std::bad_alloc(); // Optional work: its caller skips it.
		if (poolsReady())
			if (void *ptr = accountSpill(vglMalloc(size), size))
				return ptr;
		std::new_handler handler = std::get_new_handler();
		if (!handler)
			throw std::bad_alloc();
		handler();
	}
}

void *operator new[](std::size_t size) { return ::operator new(size); }

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
	try {
		return ::operator new(size);
	} catch (...) {
		return nullptr;
	}
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
	return ::operator new(size, std::nothrow);
}

void operator delete(void *ptr) noexcept { release(ptr); }
void operator delete[](void *ptr) noexcept { release(ptr); }
void operator delete(void *ptr, std::size_t) noexcept { release(ptr); }
void operator delete[](void *ptr, std::size_t) noexcept { release(ptr); }
void operator delete(void *ptr, const std::nothrow_t &) noexcept { release(ptr); }
void operator delete[](void *ptr, const std::nothrow_t &) noexcept { release(ptr); }

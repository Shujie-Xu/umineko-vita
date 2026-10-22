#pragma once
#include <cstddef>

// Process memory budget (see memory.cpp). With ATTRIBUTE2=12 the user budget is
// split once at boot: the fixed 192 MiB newlib heap, then vitaGL takes all but
// 12 MiB of the rest as its uncached RAM pool (~136 MiB), plus all free CDRAM
// and all PHYCONT except the AvPlayer reserve. vitaGL can spill textures into
// newlib, but newlib could not spill into vitaGL's pools; this closes that gap
// for SDL-owned buffers (SDL, SDL_mixer chunk decodes, SDL_image surfaces) and,
// through replaced global operator new/delete, for every C++ allocation.
namespace VitaMemory {
// Must run before SDL_Init. Pointers allocated earlier remain valid.
void installSdlAllocator();
// One stderr line with the kernel's free figures and vitaGL's pool usage.
void logBudget(const char *when);
// Bytes of SDL allocations currently served from vitaGL's pools.
size_t spilledBytes();
// While one exists, allocations by the thread that created it that newlib
// cannot place fail (SDL: nullptr, operator new: std::bad_alloc) instead of
// spilling into vitaGL's pools or reaching the fatal new-handler. For
// optional work (the lookahead prefetch); one such thread at a time.
class SoftAllocations {
public:
	SoftAllocations();
	~SoftAllocations();
	SoftAllocations(const SoftAllocations &) = delete;
	SoftAllocations &operator=(const SoftAllocations &) = delete;
};
// Whether the calling thread holds a SoftAllocations.
bool softAllocations();
}

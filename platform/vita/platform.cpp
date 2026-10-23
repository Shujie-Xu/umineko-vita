#include "platform/vita/platform.hpp"
#include "platform/vita/assets.hpp"
#include "platform/vita/memory.hpp"
#include "platform/vita/startup_image.hpp"
#ifdef ONS_VITA_FIOS
#include "platform/vita/sony_io.hpp"
#endif

#include <psp2/ctrl.h>
#include <psp2/power.h>
#ifdef ONS_VITA_FILE_LOGGING
#include <psp2/kernel/threadmgr.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef ONS_VITA_GAME_ROOT
#define ONS_VITA_GAME_ROOT "ux0:data/umineko/"
#endif

#ifndef ONS_VITA_DATA_ROOT
#define ONS_VITA_DATA_ROOT "ux0:data/umineko-native/"
#endif

// Reserve the remainder of the application budget for vitaGL/GXM allocations.
// vitaGL's RAM pool takes all but 12 MiB of what is left (136 MiB measured), so
// a larger heap shrinks that pool one for one; see memory.cpp for the budget.
extern "C" {
int _newlib_heap_size_user = 192 * 1024 * 1024;
}

namespace VitaPlatform {
const char *gameRoot() { return ONS_VITA_GAME_ROOT; }
const char *dataRoot() { return ONS_VITA_DATA_ROOT; }
const char *saveRoot() { return ONS_VITA_GAME_ROOT "save/"; }

// The release data's images are prepared at half their PC size. This is fixed
// rather than read from render_scale.txt: without that file (old or partial
// installs) every render target was four times larger and the game ran out of
// memory at the title screen.
float renderScale() {
	return 0.5f;
}

bool initialize() {
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

	// Native preferences belong here; game state belongs to the installation.
	// Android and native use the same upstream save format and directory.
	if (mkdir("ux0:data", 0777) != 0 && errno != EEXIST)
		return false;
	if (mkdir(dataRoot(), 0777) != 0 && errno != EEXIST)
		return false;
#ifdef ONS_VITA_FILE_LOGGING
	// Opt-in diagnostic builds only: release launches must not create/truncate
	// log files or write to storage for every warning emitted by a dependency.
	std::freopen(ONS_VITA_DATA_ROOT "out.txt", "w", stdout);
	std::freopen(ONS_VITA_DATA_ROOT "err.txt", "w", stderr);
	setvbuf(stdout, nullptr, _IOLBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
#endif
	// Before SDL_Init, so SDL's allocations can overflow into vitaGL's pools.
	VitaMemory::installSdlAllocator();
	VitaMemory::logBudget("boot");
	if (chdir(gameRoot()) != 0) {
		std::fprintf(stderr, "Cannot open native game root %s: %s\n", gameRoot(), std::strerror(errno));
		return false;
	}
	showStartupImage();
#ifdef ONS_VITA_FIOS
	VitaIO::initialize(gameRoot());
#endif
	VitaAssets::initializeImageSizes();
	return true;
}

#ifdef ONS_VITA_FILE_LOGGING
void logThreadPlacement(const char *label, int threadId) {
	SceKernelThreadInfo info{};
	info.size        = sizeof(info);
	const int result = sceKernelGetThreadInfo(threadId, &info);
	if (result < 0) {
		std::fprintf(stderr, "Threads: %s: no thread info (0x%08x)\n", label, static_cast<unsigned>(result));
		return;
	}
	std::fprintf(stderr,
	             "Threads: %s (%s): priority %d (initial %d), affinity 0x%x (initial 0x%x), CPU %d (last ran on %d), "
	             "moved between CPUs %d times, ran %llu ms\n",
	             label, info.name, info.currentPriority, info.initPriority, static_cast<unsigned>(info.currentCpuAffinityMask),
	             static_cast<unsigned>(info.initCpuAffinityMask), info.currentCpuId, info.lastExecutedCpuId,
	             info.changeCpuCount, static_cast<unsigned long long>(info.runClocks / 1000));
}
#endif
}

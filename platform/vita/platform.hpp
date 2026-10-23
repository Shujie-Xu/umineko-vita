#pragma once

// Native Vita process services. SDL remains responsible for audio, input,
// the window and the GL context, just as in the desktop engine.
namespace VitaPlatform {
bool initialize();
const char *gameRoot();
const char *dataRoot();
const char *saveRoot();
// Asset-preparation scale of the release data: 0.5.
float renderScale();
#ifdef ONS_VITA_FILE_LOGGING
// Diagnostic builds: one stderr line on where a thread runs (priority,
// affinity, current and last CPU, migrations), to check CPU contention.
void logThreadPlacement(const char *label, int threadId);
#endif
}

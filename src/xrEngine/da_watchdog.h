// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// The hang watchdog: a thread that wakes twice a second and, when no frame has been started (the main loop) nor drawn
// (the loading screen's own presenter) nor a shader compiled for 15 s, writes the main thread's stack into the log and
// a hang report beside the crash reports (CrashReporter::WriteHang), once per freeze. A debugger or an error being
// reported keep it quiet (the main loop runs on while the window is minimised). dar_watchdog 0 turns it off.
void da_watchdog_start();
void da_watchdog_stop();
// A frame is on its way: the main loop's, or the loading screen presenter's.
ENGINE_API void da_watchdog_beat();
// dar_watchdog (saved): 0 keeps the watchdog quiet.
extern int g_dar_watchdog;

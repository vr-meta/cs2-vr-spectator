#pragma once

// cs2-vr-spectator: what the OpenXR side's own translation units share with each other.
//
// NOT a public interface. MirvVrXr.h is what the rest of the hook and the patched
// advancedfx files call; this is one layer in from that, and nothing outside
// src/AfxHookSource2/MirvVrXr*.cpp should include it.
//
// It exists because MirvVrXr.cpp had grown to 6300 lines - one anonymous namespace over
// eighty-odd mutable globals - and the parts of it that do not touch those globals were
// being read past rather than read. Splitting those out needs somewhere for the handful of
// symbols that genuinely do cross the seam, and the discipline is that this file stays
// small: a function that needs ten more entries here is a function that has not been cut
// along a seam.

namespace MirvVrXrInternal {

// ---------------------------------------------------------------------------------
// Dispatching work onto the engine thread
// ---------------------------------------------------------------------------------

// Run a console command, on the ENGINE thread, `delayFrames` frames from now.
//
// The only safe way for any other thread to make the game do something. Called from the
// pipe thread, and from the controller handling, for the same reason: a console command
// dispatched from a thread the engine does not know about has corrupted the command
// buffer before.
void QueueCommand(const char * cmd, int delayFrames = 0);

// ---------------------------------------------------------------------------------
// The console pipe (MirvVrXrPipe.cpp)
// ---------------------------------------------------------------------------------

// Engine thread, once per frame. Reads AFXVR_PIPE the first time and opens the pipe
// unless it says 0; does nothing on every later call.
void EngineThread_Pipe();

// Open and close it by hand, which is what mirv_vr_pipe does. Both are idempotent.
void PipeStart();
void PipeShutdown();

// Whether the pipe is open, for mirv_vr_pipe's report.
bool PipeIsOpen();

// Force the answer, so that mirv_vr_pipe 0|1 overrides the environment. Does not itself
// open or close anything.
void PipeSetWanted(int wanted);

} // namespace MirvVrXrInternal

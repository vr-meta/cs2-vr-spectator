#include "stdafx.h"

#include "MirvVrXrInternal.h"

#include "../shared/AfxConsole.h"

#include <windows.h>

// ConvertSidToStringSidW and ConvertStringSecurityDescriptorToSecurityDescriptorW: the
// pipe is created with an access list of its own rather than the default one.
#include <sddl.h>

#include <stdlib.h>
#include <string>
#include <vector>

namespace MirvVrXrInternal {
namespace {

// A console for a launch that has none.
//
// The window in a worn session is 2528x2780 clamped onto a 2560x1600 display, and Panorama
// lays the console out for the full 2780 rows, so its input line sits below the bottom of
// the screen. Everything the operator can change has had to be a key, a controller gesture
// or a config re-read with PgDn then PgUp. This is the remote console that was missing -
// and the channel the launcher in docs/07-release-plan.md will use for layout sliders that
// apply live.
//
// Line-based, local only, one client at a time. Each line goes through the same
// QueueCommand the controller buttons use, so it is dispatched on the engine thread, in
// order, with everything else - never from this thread, which is not the engine's.
//
// The pipe is created with an access list naming this process's own user and SYSTEM, and
// nobody else. A pipe that runs console commands inside a game should not be open to every
// account on the machine, and a pipe's default access list is more generous than that.
// This is not a security boundary against code already running as the user - such code can
// inject a DLL of its own - it is a boundary against other accounts.
const wchar_t * const kPipeName = L"\\\\.\\pipe\\cs2vr";
const size_t kPipeMaxLine = 512;

HANDLE g_PipeStop = NULL;
HANDLE g_PipeThread = NULL;
int g_PipeWanted = -1;   // -1 not read yet, 0 off, 1 on

bool BuildPipeSecurity(SECURITY_ATTRIBUTES & sa, PSECURITY_DESCRIPTOR & sd) {
    sd = NULL;

    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    if (0 == size) { CloseHandle(token); return false; }

    std::vector<unsigned char> buffer(size);
    bool read = 0 != GetTokenInformation(token, TokenUser, &buffer[0], size, &size);
    CloseHandle(token);
    if (!read) return false;

    LPWSTR sidText = NULL;
    if (!ConvertSidToStringSidW(((TOKEN_USER*)&buffer[0])->User.Sid, &sidText)) return false;

    wchar_t sddl[256];
    swprintf_s(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;%s)", sidText);
    LocalFree(sidText);

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL)) {
        return false;
    }

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    return true;
}

void PipeLine(std::string line) {
    while (!line.empty() && ('\r' == line[line.size() - 1] || ' ' == line[line.size() - 1])) {
        line.erase(line.size() - 1);
    }
    if (line.empty()) return;
    if (line.size() > kPipeMaxLine) {
        advancedfx::Warning("AFXVR: pipe: a line of %u characters, ignored.\n", (unsigned)line.size());
        return;
    }

    // Echoed before it runs, because console.log is the only record of what a worn session
    // was told to do and by whom.
    advancedfx::Message("AFXVR: pipe: %s\n", line.c_str());
    QueueCommand(line.c_str());
}

DWORD WINAPI PipeThread(LPVOID) {
    SECURITY_ATTRIBUTES sa = {};
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!BuildPipeSecurity(sa, sd)) {
        advancedfx::Warning("AFXVR: pipe: its access rules could not be built; not opening it.\n");
        return 0;
    }

    HANDLE ioEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ioEvent) { LocalFree(sd); return 0; }

    while (WAIT_TIMEOUT == WaitForSingleObject(g_PipeStop, 0)) {
        HANDLE pipe = CreateNamedPipeW(kPipeName,
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 0, (DWORD)(kPipeMaxLine * 8), 0, &sa);
        if (INVALID_HANDLE_VALUE == pipe) {
            advancedfx::Warning("AFXVR: pipe: could not create %ls (error %lu). Another CS2?\n",
                kPipeName, GetLastError());
            break;
        }

        OVERLAPPED ov = {};
        ov.hEvent = ioEvent;
        ResetEvent(ioEvent);

        bool connected = 0 != ConnectNamedPipe(pipe, &ov);
        if (!connected) {
            DWORD err = GetLastError();
            if (ERROR_PIPE_CONNECTED == err) {
                connected = true;
            } else if (ERROR_IO_PENDING == err) {
                HANDLE waits[2] = { g_PipeStop, ioEvent };
                if (WAIT_OBJECT_0 == WaitForMultipleObjects(2, waits, FALSE, INFINITE)) {
                    CancelIo(pipe);
                    CloseHandle(pipe);
                    break;
                }
                DWORD n = 0;
                connected = 0 != GetOverlappedResult(pipe, &ov, &n, FALSE);
            }
        }
        if (!connected) { CloseHandle(pipe); continue; }

        std::string pending;
        for (;;) {
            char buf[256];
            OVERLAPPED rov = {};
            rov.hEvent = ioEvent;
            ResetEvent(ioEvent);

            DWORD n = 0;
            bool ok = 0 != ReadFile(pipe, buf, sizeof(buf), &n, &rov);
            if (!ok) {
                if (ERROR_IO_PENDING != GetLastError()) break;
                HANDLE waits[2] = { g_PipeStop, ioEvent };
                if (WAIT_OBJECT_0 == WaitForMultipleObjects(2, waits, FALSE, INFINITE)) {
                    CancelIo(pipe);
                    break;
                }
                if (!GetOverlappedResult(pipe, &rov, &n, FALSE)) break;
            }
            if (0 == n) break;

            pending.append(buf, n);
            size_t nl;
            while (std::string::npos != (nl = pending.find('\n'))) {
                PipeLine(pending.substr(0, nl));
                pending.erase(0, nl + 1);
            }
            // A client that never sends a newline must not grow this without limit.
            if (pending.size() > kPipeMaxLine * 8) pending.clear();
        }

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    CloseHandle(ioEvent);
    LocalFree(sd);
    return 0;
}

} // namespace

void PipeStart() {
    if (g_PipeThread) return;

    g_PipeStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_PipeStop) return;

    g_PipeThread = CreateThread(NULL, 0, PipeThread, NULL, 0, NULL);
    if (!g_PipeThread) {
        CloseHandle(g_PipeStop);
        g_PipeStop = NULL;
        return;
    }

    advancedfx::Message(
        "AFXVR: console pipe open at %ls, for this user only.\n"
        "AFXVR: scripts/send-command.ps1 writes to it, and so can anything else that opens a\n"
        "AFXVR: named pipe. mirv_vr_pipe 0 closes it.\n",
        kPipeName);
}

void PipeShutdown() {
    if (!g_PipeThread) return;

    // Both waits in the thread include the stop event, so it comes out of a blocking
    // connect or read by itself rather than being killed mid-syscall.
    SetEvent(g_PipeStop);
    if (WAIT_OBJECT_0 != WaitForSingleObject(g_PipeThread, 2000)) {
        advancedfx::Warning("AFXVR: pipe: its thread did not stop; leaving it open.\n");
        return;
    }

    CloseHandle(g_PipeThread); g_PipeThread = NULL;
    CloseHandle(g_PipeStop);   g_PipeStop = NULL;
    advancedfx::Message("AFXVR: console pipe closed.\n");
}

void EngineThread_Pipe() {
    if (-1 != g_PipeWanted) return;

    // On unless the environment says otherwise, because the launcher will depend on it and
    // because a worn session with no console has nowhere else to be told anything.
    char value[16] = "";
    g_PipeWanted = (0 < GetEnvironmentVariableA("AFXVR_PIPE", value, sizeof(value))
        && 0 == atoi(value)) ? 0 : 1;
    if (1 == g_PipeWanted) PipeStart();
}

// Asked by mirv_vr_pipe, which is in MirvVrXr.cpp with the other console commands and has
// no business knowing that the answer is a thread handle.
bool PipeIsOpen() {
    return NULL != g_PipeThread;
}

// mirv_vr_pipe 0|1 overriding the environment. Separate from PipeStart and PipeShutdown
// because the command decides what to open or close for itself, and then has to stop
// EngineThread_Pipe from reading AFXVR_PIPE over the top of that decision on a later frame.
void PipeSetWanted(int wanted) {
    g_PipeWanted = wanted;
}

} // namespace MirvVrXrInternal

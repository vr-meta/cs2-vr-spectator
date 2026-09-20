// cs2vr.exe - starts Counter-Strike 2 with the VR hook in it.
//
//   cs2vr                 the same as `cs2vr menu`
//   cs2vr menu            start CS2 and stop at its own menu, shown in the headset
//   cs2vr watch <demo>    start CS2 straight into a demo
//   cs2vr play <map>      an offline game against bots
//   cs2vr check           say what was found and what is wrong, and start nothing
//
// It replaces scripts/start-vr.ps1 and HLAE.exe for people who are not developing this.
// Every decision it makes is in LauncherLogic.h, where it is tested; this file is the part
// that touches the machine: the registry, the process list, the file system, and putting
// one DLL into one process that this program itself has just created.
//
// The boundary, in code rather than in a README: there is no way to start the game from
// here without -insecure. Own demos and offline play only.

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>

#include <stdio.h>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>

#include "LauncherLogic.h"

#ifndef CS2VR_VERSION
#define CS2VR_VERSION "0.0.0-dev"
#endif
#ifndef CS2VR_CS2_BUILD
#define CS2VR_CS2_BUILD "2000908"
#endif

static_assert(sizeof(void *) == 8, "cs2vr must be built for x64: it writes pointers into a 64-bit game.");

using namespace Cs2VrLauncher;

namespace {

// --- strings ---------------------------------------------------------------------------

std::wstring Widen(const std::string & s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring & w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

void Say(const char * prefix, const std::string & text) {
    printf("%s%s\n", prefix, text.c_str());
    fflush(stdout);
}

std::string LastErrorText(DWORD code) {
    wchar_t * buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, (LPWSTR)&buffer, 0, nullptr);
    std::string text = buffer ? Narrow(buffer) : std::string();
    if (buffer) LocalFree(buffer);
    while (!text.empty() && ('\n' == text.back() || '\r' == text.back() || ' ' == text.back())) text.pop_back();
    return "error " + std::to_string(code) + (text.empty() ? "" : " (" + text + ")");
}

// --- the file system -------------------------------------------------------------------

bool FileExists(const std::string & path) {
    DWORD a = GetFileAttributesW(Widen(path).c_str());
    return INVALID_FILE_ATTRIBUTES != a && 0 == (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirectoryExists(const std::string & path) {
    DWORD a = GetFileAttributesW(Widen(path).c_str());
    return INVALID_FILE_ATTRIBUTES != a && 0 != (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadTextFile(const std::string & path, std::string & out) {
    std::ifstream f(Widen(path).c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string ExeDirectory() {
    wchar_t buffer[32768];
    DWORD n = GetModuleFileNameW(nullptr, buffer, 32768);
    std::wstring path(buffer, n);
    size_t slash = path.find_last_of(L"\\/");
    return Narrow(std::wstring::npos == slash ? L"." : path.substr(0, slash));
}

std::string FileNameOf(const std::string & path) {
    size_t slash = path.find_last_of("\\/");
    return std::string::npos == slash ? path : path.substr(slash + 1);
}

bool IsAbsolutePath(const std::string & path) {
    return (path.size() >= 3 && ':' == path[1] && ('\\' == path[2] || '/' == path[2]))
        || (path.size() >= 2 && '\\' == path[0] && '\\' == path[1]);
}

// --- the registry ----------------------------------------------------------------------

bool RegString(HKEY root, const wchar_t * key, const wchar_t * value, std::string & out) {
    wchar_t buffer[4096];
    DWORD bytes = sizeof(buffer);
    LSTATUS r = RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buffer, &bytes);
    if (ERROR_SUCCESS != r) return false;
    out = Narrow(buffer);
    return true;
}

std::vector<std::string> RegValueNames(HKEY root, const wchar_t * key) {
    std::vector<std::string> names;
    HKEY h = nullptr;
    if (ERROR_SUCCESS != RegOpenKeyExW(root, key, 0, KEY_READ, &h)) return names;
    for (DWORD i = 0;; i++) {
        wchar_t name[4096];
        DWORD length = 4096;
        if (ERROR_SUCCESS != RegEnumValueW(h, i, name, &length, nullptr, nullptr, nullptr, nullptr)) break;
        names.push_back(Narrow(std::wstring(name, length)));
    }
    RegCloseKey(h);
    return names;
}

// --- processes -------------------------------------------------------------------------

bool AnyProcessNamed(const std::vector<std::wstring> & names) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (INVALID_HANDLE_VALUE == snap) return false;
    PROCESSENTRY32W entry = { sizeof(entry) };
    bool found = false;
    if (Process32FirstW(snap, &entry)) {
        do {
            for (size_t i = 0; i < names.size() && !found; i++) {
                if (0 == _wcsicmp(entry.szExeFile, names[i].c_str())) found = true;
            }
        } while (!found && Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

// --- finding the game ------------------------------------------------------------------

bool FindCs2(const std::string & overrideRoot, Cs2Paths & out, std::string & how) {
    if (!overrideRoot.empty()) {
        // --cs2 names the game's own folder (the one that contains "game").
        Cs2Paths p = Cs2PathsIn("", "");
        p.root = overrideRoot;
        p.workingDir = JoinPath(JoinPath(JoinPath(p.root, "game"), "bin"), "win64");
        p.exe = JoinPath(p.workingDir, "cs2.exe");
        p.csgo = JoinPath(JoinPath(p.root, "game"), "csgo");
        p.steamInf = JoinPath(p.csgo, "steam.inf");
        p.cfgDir = JoinPath(JoinPath(p.csgo, "cfg"), "cs2vr");
        p.consoleLog = JoinPath(p.csgo, "console.log");
        if (FileExists(p.exe)) { out = p; how = "--cs2"; return true; }
        return false;
    }

    std::string steam;
    if (!RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", steam)
        && !RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", steam)) {
        return false;
    }
    for (size_t i = 0; i < steam.size(); i++) if ('/' == steam[i]) steam[i] = '\\';

    std::vector<std::string> libraries(1, steam);
    std::string vdf;
    if (ReadTextFile(JoinPath(JoinPath(steam, "steamapps"), "libraryfolders.vdf"), vdf)
        || ReadTextFile(JoinPath(JoinPath(steam, "config"), "libraryfolders.vdf"), vdf)) {
        std::vector<std::string> listed = VdfValues(vdf, "path");
        libraries.insert(libraries.end(), listed.begin(), listed.end());
    }

    for (size_t i = 0; i < libraries.size(); i++) {
        std::string manifest;
        if (!ReadTextFile(JoinPath(JoinPath(libraries[i], "steamapps"), "appmanifest_730.acf"), manifest)) continue;
        std::vector<std::string> dir = VdfValues(manifest, "installdir");
        if (dir.empty()) continue;
        Cs2Paths p = Cs2PathsIn(libraries[i], dir[0]);
        if (FileExists(p.exe)) { out = p; how = "Steam library " + libraries[i]; return true; }
    }
    return false;
}

// --- configs ---------------------------------------------------------------------------

// Our configs go into cfg\cs2vr\ and nowhere else, on every launch, so the game always
// runs the ones that came with this release and nothing of ours lies loose in Valve's tree.
int DeployConfigs(const std::string & from, const std::string & to) {
    CreateDirectoryW(Widen(to).c_str(), nullptr);
    if (!DirectoryExists(to)) return -1;

    int copied = 0;
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW(Widen(JoinPath(from, "*.cfg")).c_str(), &found);
    if (INVALID_HANDLE_VALUE == h) return 0;
    do {
        std::string name = Narrow(found.cFileName);
        if (CopyFileW(Widen(JoinPath(from, name)).c_str(), Widen(JoinPath(to, name)).c_str(), FALSE)) copied++;
    } while (FindNextFileW(h, &found));
    FindClose(h);
    return copied;
}

// A demo somewhere else on the disk is made reachable from the game's own directory,
// because that is the only place `playdemo` is known to look. A hard link when it can be
// one (same volume, no copy of a few hundred megabytes), a copy otherwise.
bool StageDemo(const std::string & demo, const Cs2Paths & cs2, std::string & playName, std::string & note) {
    if (!IsAbsolutePath(demo)) { playName = demo; return true; }
    if (!FileExists(demo)) { note = "Demo not found: " + demo; return false; }

    std::string stagingDir = JoinPath(cs2.csgo, "cs2vr_demos");
    CreateDirectoryW(Widen(stagingDir).c_str(), nullptr);
    std::string name = FileNameOf(demo);
    std::string target = JoinPath(stagingDir, name);

    DeleteFileW(Widen(target).c_str());
    if (CreateHardLinkW(Widen(target).c_str(), Widen(demo).c_str(), nullptr)) {
        note = "linked into game\\csgo\\cs2vr_demos";
    } else if (CopyFileW(Widen(demo).c_str(), Widen(target).c_str(), FALSE)) {
        note = "copied into game\\csgo\\cs2vr_demos";
    } else {
        note = "Could not make the demo reachable from the game's folder: " + LastErrorText(GetLastError());
        return false;
    }
    playName = "cs2vr_demos/" + name;
    return true;
}

// --- the child's environment -----------------------------------------------------------

struct CaseInsensitiveLess {
    bool operator()(const std::wstring & a, const std::wstring & b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
};

std::vector<wchar_t> BuildEnvironment(const std::map<std::wstring, std::wstring, CaseInsensitiveLess> & changes,
                                      const std::wstring & prependPath) {
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> env;

    wchar_t * block = GetEnvironmentStringsW();
    for (wchar_t * p = block; p && *p; p += wcslen(p) + 1) {
        std::wstring entry(p);
        size_t eq = entry.find(L'=', 1); // entries like "=C:=C:\" start with '='
        if (std::wstring::npos == eq) continue;
        env[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    if (block) FreeEnvironmentStringsW(block);

    for (auto it = changes.begin(); it != changes.end(); ++it) env[it->first] = it->second;
    if (!prependPath.empty()) env[L"PATH"] = prependPath + L";" + env[L"PATH"];

    std::vector<wchar_t> out;
    for (auto it = env.begin(); it != env.end(); ++it) {
        std::wstring entry = it->first + L"=" + it->second;
        out.insert(out.end(), entry.begin(), entry.end());
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

// --- putting the hook into the game ----------------------------------------------------

// Run one kernel32 function, taking one wide string, inside the (still suspended) game.
//
// kernel32 is mapped at the same address in every process of a boot, so the address of
// LoadLibraryW here is its address there. The string has to live in the game's memory, so
// it is written there first. This is the oldest and plainest way to load a DLL into a
// process one has just created, and it is all this program does to the game.
bool RemoteCall(HANDLE process, const char * function, const std::wstring & argument, DWORD & result, std::string & why) {
    FARPROC address = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), function);
    if (!address) { why = std::string("kernel32 has no ") + function; return false; }

    SIZE_T bytes = (argument.size() + 1) * sizeof(wchar_t);
    void * remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { why = "VirtualAllocEx: " + LastErrorText(GetLastError()); return false; }

    bool ok = false;
    if (!WriteProcessMemory(process, remote, argument.c_str(), bytes, nullptr)) {
        why = "WriteProcessMemory: " + LastErrorText(GetLastError());
    } else {
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, (LPTHREAD_START_ROUTINE)address, remote, 0, nullptr);
        if (!thread) {
            why = "CreateRemoteThread: " + LastErrorText(GetLastError());
        } else {
            if (WAIT_OBJECT_0 != WaitForSingleObject(thread, 60000)) {
                why = std::string(function) + " did not return within a minute";
            } else {
                GetExitCodeThread(thread, &result);
                ok = true;
            }
            CloseHandle(thread);
        }
    }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

// --- reading the game's log back -------------------------------------------------------

// The hook talks through console.log and nothing else. Print its lines as they appear so
// the person launching sees the same thing a bug report needs, and stop when the session
// is up, the game is gone, or it has plainly taken too long.
void FollowLog(const std::string & path, HANDLE process, int seconds) {
    size_t seen = 0;
    for (int i = 0; i < seconds; i++) {
        if (WAIT_OBJECT_0 == WaitForSingleObject(process, 1000)) { Say("", "CS2 has exited."); return; }

        std::string text;
        if (!ReadTextFile(path, text) || text.size() <= seen) continue;
        std::string fresh = text.substr(seen);
        size_t lastNewline = fresh.rfind('\n');
        if (std::string::npos == lastNewline) continue;
        fresh.resize(lastNewline + 1);
        seen += fresh.size();

        std::istringstream lines(fresh);
        std::string line;
        bool done = false;
        while (std::getline(lines, line)) {
            size_t at = line.find("AFXVR:");
            if (std::string::npos == at) continue;
            while (!line.empty() && '\r' == line.back()) line.pop_back();
            Say("  ", line.substr(at));
            if (std::string::npos != line.find("submitting frames to the headset")) done = true;
            if (std::string::npos != line.find("showing the menu")) done = true;
        }
        if (done) { Say("", "The headset has a picture. Put it on."); return; }
    }
    Say("", "Still nothing from the hook. The lines above, and game\\csgo\\console.log, say why.");
}

// --- the command line ------------------------------------------------------------------

struct Arguments {
    std::string command = "menu";
    std::string demo;
    std::string cs2Root;
    std::string runtime;
    LaunchOptions options;
    bool keepSteamVr = false;
    bool autostart = true;
    bool follow = true;
    bool help = false;
};

bool ParseArguments(int argc, wchar_t ** argv, Arguments & a, std::string & error) {
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        std::string arg = Narrow(argv[i]);
        auto value = [&](std::string & into) {
            if (i + 1 >= argc) { error = arg + " needs a value."; return false; }
            into = Narrow(argv[++i]);
            return true;
        };
        std::string text;
        if ("--help" == arg || "-h" == arg || "/?" == arg) a.help = true;
        else if ("--width" == arg)  { if (!value(text)) return false; a.options.width = atoi(text.c_str()); }
        else if ("--height" == arg) { if (!value(text)) return false; a.options.height = atoi(text.c_str()); }
        else if ("--fps" == arg)    { if (!value(text)) return false; a.options.fpsMax = atoi(text.c_str()); }
        else if ("--cs2" == arg)    { if (!value(a.cs2Root)) return false; }
        else if ("--runtime" == arg){ if (!value(a.runtime)) return false; }
        else if ("--game-args" == arg) { if (!value(a.options.extra)) return false; }
        else if ("--keep-steamvr" == arg) a.keepSteamVr = true;
        else if ("--no-autostart" == arg) a.autostart = false;
        else if ("--no-follow" == arg) a.follow = false;
        else if ("--bots" == arg)   { if (!value(text)) return false; a.options.bots = atoi(text.c_str()); }
        else if (!arg.empty() && '-' == arg[0]) { error = "Unknown option " + arg + "."; return false; }
        else if (0 == positional) { a.command = arg; positional++; }
        else if (1 == positional && "watch" == a.command) { a.demo = arg; positional++; }
        else if (1 == positional && "play" == a.command) { a.options.map = arg; positional++; }
        else { error = "Unexpected argument " + arg + "."; return false; }
    }
    if ("watch" == a.command && a.demo.empty()) { error = "watch needs a demo: cs2vr watch <file.dem>"; return false; }
    if ("play" == a.command && !IsPlainMapName(a.options.map)) {
        error = a.options.map.empty() ? "play needs a map: cs2vr play de_inferno"
                                      : "That does not look like a map name: " + a.options.map;
        return false;
    }
    if ("menu" != a.command && "watch" != a.command && "play" != a.command && "check" != a.command
        && "selftest" != a.command) {
        error = "Unknown command " + a.command + ".";
        return false;
    }
    return true;
}

// The one step of a launch that cannot be tried by looking: loading a DLL into a process
// that has been created and not yet run. Done here against Windows' own cmd.exe and a
// Windows DLL, so that "does this machine let it happen at all" - antivirus is the usual
// answer when it does not - has an answer that does not involve starting the game.
//
// Two questions, because there are two ways it goes wrong. Can a DLL be put into a fresh
// process at all? And when that DLL needs OTHER DLLs lying beside it, are they found? The
// second is the subtle one: Windows looks for a DLL's dependencies next to the PROGRAM, not
// next to the DLL, so the hook's OpenEXR and C++ runtime libraries are invisible from
// inside cs2.exe unless the launcher says where they are. HLAE's own injector comments
// that SetDllDirectoryW alone was not enough for it; here the hook's folder also goes on
// the child's PATH. This tries exactly that arrangement with one of the hook's own
// libraries that has dependencies of its own - and none of the hook's code, which has no
// business running inside cmd.exe.

// Create cmd.exe suspended, optionally with the hook's folder made findable, load one DLL
// into it, end it. Returns whether the DLL loaded; `why` is set when the attempt itself
// could not be made.
bool TryLoadInFreshProcess(const std::wstring & dll, const std::wstring & searchDir, bool & loaded, std::string & why) {
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring target = std::wstring(system) + L"\\cmd.exe";
    std::wstring commandLine = L"\"" + target + L"\" /c exit";
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    std::map<std::wstring, std::wstring, CaseInsensitiveLess> noChanges;
    std::vector<wchar_t> environment = BuildEnvironment(noChanges, searchDir);

    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION child = {};
    if (!CreateProcessW(target.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        environment.data(), nullptr, &startup, &child)) {
        why = "could not create the test process: " + LastErrorText(GetLastError());
        return false;
    }

    DWORD result = 0;
    bool ran = true;
    if (!searchDir.empty()) ran = RemoteCall(child.hProcess, "SetDllDirectoryW", searchDir, result, why);
    if (ran) ran = RemoteCall(child.hProcess, "LoadLibraryW", dll, result, why);
    loaded = ran && 0 != result;

    TerminateProcess(child.hProcess, 0);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return ran;
}

int SelfTest(const std::string & hookDir) {
    std::string why;
    bool loaded = false;

    // 1. Can anything be loaded into a fresh process on this machine?
    if (!TryLoadInFreshProcess(L"version.dll", L"", loaded, why)) { Say("FAIL  ", "The DLL-loading step did not run: " + why); return 1; }
    if (!loaded) { Say("FAIL  ", "The step ran, but a fresh process would not load even a Windows DLL."); return 1; }
    Say("ok    ", "A DLL can be loaded into a process that has been created and not yet run.");

    // 2. Are a DLL's own dependencies found when they lie beside it and not beside the program?
    const std::string probe = JoinPath(hookDir, "OpenEXR-3_3.dll");
    if (!FileExists(probe)) {
        Say("note  ", "hook\\x64 is not next to this program, so the dependency search could not be tried.");
        return 0;
    }

    bool bare = false, arranged = false;
    if (!TryLoadInFreshProcess(Widen(probe), L"", bare, why)
        || !TryLoadInFreshProcess(Widen(probe), Widen(hookDir), arranged, why)) {
        Say("FAIL  ", "The dependency test did not run: " + why);
        return 1;
    }

    Say(bare ? "note  " : "ok    ", bare
        ? "One of the hook's libraries loaded even WITHOUT being told where its dependencies are -"
          " something on this machine already has them on PATH, so this test proves less than it should."
        : "Left alone, Windows does not find a library's dependencies beside it (as expected).");
    if (!arranged) {
        Say("FAIL  ", "Even with hook\\x64 on the search path, " + probe + " would not load. A file is missing"
                      " from hook\\x64, or something is keeping these DLLs from loading (antivirus).");
        return 1;
    }
    Say("ok    ", "With the launcher's arrangement they are found. The same step puts the hook into CS2.");
    return 0;
}

void PrintHelp() {
    printf(
        "cs2vr %s - Counter-Strike 2 in a VR headset (made for CS2 build %s)\n"
        "\n"
        "  cs2vr                  start CS2 and stop at its menu, shown in the headset\n"
        "  cs2vr watch <demo>     start CS2 straight into a demo\n"
        "  cs2vr play <map>       an offline game against bots, e.g. cs2vr play de_inferno\n"
        "  cs2vr check            say what was found and what is wrong; start nothing\n"
        "  cs2vr selftest         try the DLL-loading step on a harmless process, not the game\n"
        "\n"
        "  --bots N              how many bots `play` adds (default 6)\n"
        "  --width N --height N   per-eye size; the game window IS the eye (default 2528 x 2780)\n"
        "  --fps N                frame cap, never off (default 90)\n"
        "  --runtime meta|steamvr which OpenXR runtime the game gets (default: Meta's if its\n"
        "                         service is running, else the machine's active one)\n"
        "  --cs2 <folder>         the game's folder, if Steam does not know where it is\n"
        "  --keep-steamvr         start even though SteamVR is running\n"
        "  --no-autostart         do not start the VR session by itself (F9 in the game does)\n"
        "  --no-follow            do not print the hook's log lines after starting\n"
        "  --game-args \"...\"      appended to CS2's command line\n"
        "\n"
        "The game is always started with -insecure: this loads a DLL into it. Own demos and\n"
        "offline play only - never matchmaking, never a VAC-protected server.\n",
        CS2VR_VERSION, CS2VR_CS2_BUILD);
}

} // namespace

int wmain(int argc, wchar_t ** argv) {
    Arguments args;
    std::string error;
    if (!ParseArguments(argc, argv, args, error)) { Say("", error); Say("", "cs2vr --help lists the options."); return 2; }
    if (args.help) { PrintHelp(); return 0; }

    printf("cs2vr %s (made for CS2 build %s)\n\n", CS2VR_VERSION, CS2VR_CS2_BUILD);

    // --- what is on this machine ---
    const std::string here = ExeDirectory();
    const std::string hookDir = JoinPath(JoinPath(here, "hook"), "x64");

    if ("selftest" == args.command) return SelfTest(hookDir);

    const std::string hookDll = JoinPath(hookDir, "AfxHookSource2.dll");
    const std::string cfgSource = JoinPath(here, "cfg");

    Cs2Paths cs2;
    std::string foundHow;
    Machine machine;
    machine.cs2Found = FindCs2(args.cs2Root, cs2, foundHow);
    machine.hookPresent = FileExists(hookDll);
    machine.cs2Running = AnyProcessNamed({ L"cs2.exe" });
    machine.steamVrRunning = AnyProcessNamed({ L"vrserver.exe", L"vrmonitor.exe", L"vrcompositor.exe" });
    machine.metaServiceRunning = AnyProcessNamed({ L"OVRServer_x64.exe" });

    Say("hook      ", machine.hookPresent ? hookDll : "NOT FOUND at " + hookDll);
    Say("CS2       ", machine.cs2Found ? cs2.exe + "  (" + foundHow + ")" : "NOT FOUND");

    if (machine.cs2Found) {
        std::string inf;
        ReadTextFile(cs2.steamInf, inf);
        BuildCheck build = CheckBuild(inf, CS2VR_CS2_BUILD);
        Say(kBuildMatches == build.verdict ? "build     " : "build  !  ", build.message);
    }

    std::vector<std::string> runtimes = RegValueNames(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes");
    std::string activeRuntime;
    RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", activeRuntime);
    int chosen = ChooseRuntime(runtimes, activeRuntime, machine.metaServiceRunning, args.runtime);
    std::string runtimeJson = chosen >= 0 ? runtimes[(size_t)chosen] : std::string();
    if (!args.runtime.empty() && chosen < 0 && FileExists(args.runtime)) runtimeJson = args.runtime; // a manifest, named outright
    Say("OpenXR    ", runtimeJson.empty()
        ? (activeRuntime.empty() ? "no runtime registered on this machine" : "the machine's active runtime: " + activeRuntime)
        : runtimeJson + "  (for the game only)");
    Say("Link      ", machine.metaServiceRunning ? "Meta's VR service is running" : "Meta's VR service is NOT running");
    Say("SteamVR   ", machine.steamVrRunning ? "RUNNING" : "not running");
    printf("\n");

    std::vector<Problem> problems = Preflight(machine, args.keepSteamVr);
    bool blocked = false;
    for (size_t i = 0; i < problems.size(); i++) {
        Say(problems[i].blocking ? "STOP  " : "note  ", problems[i].text);
        blocked = blocked || problems[i].blocking;
    }
    if ("check" == args.command) return blocked ? 1 : 0;
    if (blocked) return 1;

    // --- prepare ---
    int deployed = DeployConfigs(cfgSource, cs2.cfgDir);
    if (deployed <= 0) {
        Say("STOP  ", "Could not put the configs into " + cs2.cfgDir + " (from " + cfgSource + "). Without them"
                      " the game starts with no VR settings and no key layout.");
        return 1;
    }
    Say("", std::to_string(deployed) + " configs -> " + cs2.cfgDir);

    if ("watch" == args.command) {
        std::string note;
        if (!StageDemo(args.demo, cs2, args.options.demo, note)) { Say("STOP  ", note); return 1; }
        Say("", "demo: " + args.options.demo + (note.empty() ? "" : "  (" + note + ")"));
    }

    // A fresh log, so what is read back afterwards belongs to this launch.
    DeleteFileW(Widen(cs2.consoleLog).c_str());

    std::map<std::wstring, std::wstring, CaseInsensitiveLess> env;
    if (!runtimeJson.empty()) env[L"XR_RUNTIME_JSON"] = Widen(runtimeJson);
    env[L"AFXVR_AUTOSTART"] = args.autostart ? L"1" : L"0";
    // The hook's own DLLs (OpenEXR, the C++ runtime, the OpenXR loader) sit beside it, and
    // the game's folder is not where Windows looks for them.
    std::vector<wchar_t> environment = BuildEnvironment(env, Widen(hookDir));

    std::string gameArgs = BuildGameArgs(args.options);
    std::wstring commandLine = Widen(QuoteArg(cs2.exe) + " " + gameArgs);
    Say("", "cs2.exe " + gameArgs);
    printf("\n");

    // --- start, suspended; put the hook in; let it run ---
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION child = {};
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    // Its own process group and no share in this console: otherwise Ctrl+C pressed here, to
    // stop following the log, goes to the game as well and takes it down mid-session.
    if (!CreateProcessW(Widen(cs2.exe).c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS,
                        environment.data(), Widen(cs2.workingDir).c_str(), &startup, &child)) {
        Say("STOP  ", "Could not start cs2.exe: " + LastErrorText(GetLastError()));
        return 1;
    }

    std::string why;
    DWORD result = 0;
    bool injected = RemoteCall(child.hProcess, "SetDllDirectoryW", Widen(hookDir), result, why)
                 && RemoteCall(child.hProcess, "LoadLibraryW", Widen(hookDll), result, why);
    if (injected && 0 == result) {
        injected = false;
        why = "the game could not load the hook (LoadLibraryW returned nothing). A missing DLL beside it,"
              " or antivirus holding it back, are the usual reasons.";
    }
    if (!injected) {
        // It has not run a single instruction of its own; ending it costs nothing.
        TerminateProcess(child.hProcess, 1);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        Say("STOP  ", "The hook did not go in: " + why);
        return 1;
    }

    ResumeThread(child.hThread);
    Say("", "CS2 is starting with the hook in it.");
    if (args.follow) FollowLog(cs2.consoleLog, child.hProcess, 240);

    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return 0;
}

#pragma once

// Everything the launcher decides, with nothing in it that needs Windows, Steam or a
// headset - so it is tested on any machine in seconds, the way MirvVrMath.h is. main.cpp
// does the registry, the processes and the injection; it asks this file what to make of
// what it found.

#include <string>
#include <vector>
#include <string.h>
#include <stdlib.h>

namespace Cs2VrLauncher {

// ---------------------------------------------------------------------------------
// Valve's text formats
// ---------------------------------------------------------------------------------

// Every value stored under `key` in a VDF/ACF document, in order of appearance.
//
// libraryfolders.vdf and appmanifest_*.acf are the same format: quoted keys and quoted
// values separated by whitespace, nested in braces, backslashes escaped. Nothing here
// cares about the nesting - a key is matched wherever it appears - because the two
// questions asked of these files ("which paths?", "which installdir?") do not need it,
// and a parser that understands less has less to get wrong when Valve adds a field.
inline std::vector<std::string> VdfValues(const std::string & text, const char * key) {
    std::vector<std::string> found;

    // Tokenise into quoted strings; braces and anything unquoted are skipped.
    std::vector<std::string> tokens;
    std::vector<bool> startsLine; // whether a token is the first on its line
    bool lineHasToken = false;

    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if ('\n' == c) { lineHasToken = false; continue; }
        if ('/' == c && i + 1 < text.size() && '/' == text[i + 1]) {
            while (i < text.size() && '\n' != text[i]) i++;
            lineHasToken = false;
            continue;
        }
        if ('"' != c) continue;

        std::string token;
        for (i++; i < text.size() && '"' != text[i]; i++) {
            if ('\\' == text[i] && i + 1 < text.size()) {
                char next = text[i + 1];
                if ('\\' == next || '"' == next) { token += next; i++; continue; }
                if ('n' == next) { token += '\n'; i++; continue; }
                if ('t' == next) { token += '\t'; i++; continue; }
            }
            token += text[i];
        }
        tokens.push_back(token);
        startsLine.push_back(!lineHasToken);
        lineHasToken = true;
    }

    // A value is the token after a key ON THE SAME LINE. A key that opens a block has
    // nothing after it on its line, and the next line's first token is a key, not a value.
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
        if (!startsLine[i]) continue;
        if (startsLine[i + 1]) continue;
#ifdef _WIN32
        bool same = 0 == _stricmp(tokens[i].c_str(), key);
#else
        bool same = 0 == strcasecmp(tokens[i].c_str(), key);
#endif
        if (same) found.push_back(tokens[i + 1]);
    }
    return found;
}

// The value of `key` in steam.inf, which is plain `Key=Value` lines. Empty if absent.
inline std::string InfValue(const std::string & text, const char * key) {
    size_t keyLength = strlen(key);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (std::string::npos == end) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;

        while (!line.empty() && ('\r' == line.back() || ' ' == line.back() || '\t' == line.back())) line.pop_back();
        if (line.size() > keyLength && '=' == line[keyLength] && 0 == line.compare(0, keyLength, key)) {
            return line.substr(keyLength + 1);
        }
    }
    return std::string();
}

// ---------------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------------

inline std::string JoinPath(const std::string & a, const std::string & b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a[a.size() - 1];
    bool hasSeparator = ('\\' == last || '/' == last);
    return hasSeparator ? a + b : a + "\\" + b;
}

// Where things live inside one Steam library, given the installdir from the manifest.
struct Cs2Paths {
    std::string root;        // ...\steamapps\common\<installdir>
    std::string exe;         // ...\game\bin\win64\cs2.exe
    std::string workingDir;  // ...\game\bin\win64
    std::string csgo;        // ...\game\csgo
    std::string steamInf;    // ...\game\csgo\steam.inf
    std::string cfgDir;      // ...\game\csgo\cfg\cs2vr   <- we write here, and into
                             //    ...\game\csgo\cs2vr_demos when a demo has to be staged.
                             //    Those two and nothing else in Valve's tree; the uninstall
                             //    note in the install skill has to keep naming both.
    std::string consoleLog;  // ...\game\csgo\console.log
};

inline Cs2Paths Cs2PathsIn(const std::string & library, const std::string & installDir) {
    Cs2Paths p;
    p.root = JoinPath(JoinPath(JoinPath(library, "steamapps"), "common"), installDir);
    p.workingDir = JoinPath(JoinPath(JoinPath(p.root, "game"), "bin"), "win64");
    p.exe = JoinPath(p.workingDir, "cs2.exe");
    p.csgo = JoinPath(JoinPath(p.root, "game"), "csgo");
    p.steamInf = JoinPath(p.csgo, "steam.inf");
    p.cfgDir = JoinPath(JoinPath(p.csgo, "cfg"), "cs2vr");
    p.consoleLog = JoinPath(p.csgo, "console.log");
    return p;
}

// ---------------------------------------------------------------------------------
// The CS2 build
// ---------------------------------------------------------------------------------

enum BuildVerdict { kBuildMatches, kBuildDiffers, kBuildUnknown };

struct BuildCheck {
    BuildVerdict verdict;
    std::string installed;   // ClientVersion from steam.inf, empty if unreadable
    std::string message;     // one sentence for a person
};

// The hook writes into CS2's memory at offsets measured on one build. On another build it
// refuses to write and the headset shows a camera that ignores the head - which looks
// broken rather than out of date. So the difference is said in words, before launching.
inline BuildCheck CheckBuild(const std::string & steamInfText, const std::string & madeFor) {
    BuildCheck c;
    c.installed = InfValue(steamInfText, "ClientVersion");
    if (c.installed.empty()) {
        c.verdict = kBuildUnknown;
        c.message = "Could not read CS2's build number (steam.inf). This release was made for build "
                    + madeFor + ".";
    } else if (c.installed == madeFor) {
        c.verdict = kBuildMatches;
        c.message = "CS2 build " + c.installed + ", the one this release was made for.";
    } else {
        c.verdict = kBuildDiffers;
        c.message = "CS2 is build " + c.installed + " but this release was made for build " + madeFor
                    + ". The game has probably updated. The hook will refuse to move the camera if the"
                      " game's memory layout changed; look for a newer release.";
    }
    return c;
}

// ---------------------------------------------------------------------------------
// The OpenXR runtime
// ---------------------------------------------------------------------------------

// Which runtime manifest to hand the game, out of the ones registered on the machine.
//
// A Quest over Link is served by Meta's runtime. SteamVR can drive it too, but only as a
// client of Meta's, and with both up the second one to ask is never scheduled - the game
// freezes. So with Meta's service running, Meta's runtime is chosen even when the registry
// says SteamVR is the active one. Otherwise the active runtime is respected.
//
// Returns an index into `available`, or -1 to leave the choice to the OpenXR loader.
inline int ChooseRuntime(const std::vector<std::string> & available, const std::string & active,
                         bool metaServiceRunning, const std::string & wanted) {
    auto contains = [](const std::string & haystack, const char * needle) {
        std::string lower;
        for (size_t i = 0; i < haystack.size(); i++) {
            char c = haystack[i];
            lower += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        return std::string::npos != lower.find(needle);
    };

    if (!wanted.empty()) {
        const char * needle = ("meta" == wanted || "oculus" == wanted) ? "oculus"
                            : ("steamvr" == wanted) ? "steam" : nullptr;
        if (needle) {
            for (size_t i = 0; i < available.size(); i++) if (contains(available[i], needle)) return (int)i;
        }
        return -1;
    }

    if (metaServiceRunning) {
        for (size_t i = 0; i < available.size(); i++) if (contains(available[i], "oculus")) return (int)i;
    }
    for (size_t i = 0; i < available.size(); i++) if (available[i] == active) return (int)i;
    return -1;
}

// ---------------------------------------------------------------------------------
// The command line
// ---------------------------------------------------------------------------------

struct LaunchOptions {
    int width = 2528;            // per eye: submission copies the back buffer, so the
    int height = 2780;           // window size IS the eye size
    int fpsMax = 90;
    std::string demo;            // empty: stop at CS2's own menu
    std::string map;             // an offline game with bots on this map, instead of a demo
    int bots = 6;
    std::string extra;           // appended verbatim, for whoever is developing
};

// A map name is typed by a person and ends up on a command line that the game splits on
// spaces and semicolons. Letters, digits, underscore, dash and slash (workshop maps) are
// all a map name needs; anything else is refused rather than quoted.
inline bool IsPlainMapName(const std::string & name) {
    if (name.empty() || name.size() > 96) return false;
    for (size_t i = 0; i < name.size(); i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
               || '_' == c || '-' == c || '/' == c;
        if (!ok) return false;
    }
    return true;
}

// Quote one argument the way CommandLineToArgvW will undo.
inline std::string QuoteArg(const std::string & arg) {
    if (!arg.empty() && std::string::npos == arg.find_first_of(" \t\"")) return arg;
    std::string out = "\"";
    size_t backslashes = 0;
    for (size_t i = 0; i < arg.size(); i++) {
        char c = arg[i];
        if ('\\' == c) { backslashes++; continue; }
        if ('"' == c) { out.append(backslashes * 2 + 1, '\\'); out += '"'; backslashes = 0; continue; }
        out.append(backslashes, '\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

// The arguments CS2 is started with. Not a set of defaults: each one is here because
// leaving it out cost a session.
//
//   -insecure          this injects a DLL. It is never optional and there is no way to
//                      build a command line without it.
//   -windowed -noborder -w -h   stated every time: CS2 remembers the last size in its own
//                      video settings, and a launch that says nothing inherits it.
//   +fps_max           an uncapped game and a VR compositor fight over the GPU until both
//                      stall ("QueuePresentAndWait looped ... without a present event").
//   -condebug          console.log is the only console a worn launch has.
//   +exec cs2vr/vr     our configs live in cfg\cs2vr\ and nowhere else.
inline std::string BuildGameArgs(const LaunchOptions & o) {
    std::string a = "-insecure -novid -condebug -allow_third_party_software +con_enable 1"
                    " -windowed -noborder";
    a += " -w " + std::to_string(o.width > 0 ? o.width : 2528);
    a += " -h " + std::to_string(o.height > 0 ? o.height : 2780);
    a += " +fps_max " + std::to_string(o.fpsMax > 0 ? o.fpsMax : 90);
    a += " +exec cs2vr/vr";
    if (!o.demo.empty()) {
        a += " +playdemo " + QuoteArg(o.demo);
    } else if (IsPlainMapName(o.map)) {
        // An offline casual game against bots. sv_lan keeps it off the internet; there is
        // no console in a worn launch to type these into, which is why they are here.
        int bots = o.bots < 0 ? 0 : (o.bots > 20 ? 20 : o.bots);
        a += " +game_type 0 +game_mode 0 +sv_lan 1 +bot_quota " + std::to_string(bots);
        a += " +map " + o.map;
    }
    if (!o.extra.empty()) a += " " + o.extra;
    return a;
}

// ---------------------------------------------------------------------------------
// May we start?
// ---------------------------------------------------------------------------------

struct Machine {
    bool cs2Running = false;
    bool steamVrRunning = false;
    bool metaServiceRunning = false;
    bool hookPresent = false;
    bool cs2Found = false;
};

struct Problem {
    bool blocking;
    std::string text;
};

// What is wrong with starting now, each with the reason it matters - the reasons are the
// support channel. Blocking problems stop the launch; the rest are said and passed.
inline std::vector<Problem> Preflight(const Machine & m, bool keepSteamVr) {
    std::vector<Problem> p;
    if (!m.hookPresent) {
        p.push_back({ true, "hook\\x64\\AfxHookSource2.dll is not next to this program. Unpack the whole"
                            " release and keep its folders as they are: the hook finds its shaders from"
                            " its own path." });
    }
    if (!m.cs2Found) {
        p.push_back({ true, "Counter-Strike 2 was not found in any Steam library. Pass --cs2 <folder> if"
                            " it is somewhere Steam does not know about." });
    }
    if (m.cs2Running) {
        p.push_back({ true, "CS2 is already running. Close it first: the hook has to be in the game from"
                            " its first instruction, and this launch must be the one with -insecure." });
    }
    if (m.steamVrRunning) {
        p.push_back({ !keepSteamVr, "SteamVR is running. On a Quest over Link it holds the headset, and a"
                                    " second VR program beside it is never given a frame - the game"
                                    " freezes. Close SteamVR (or pass --keep-steamvr if you mean it)." });
    }
    if (!m.metaServiceRunning && !m.steamVrRunning) {
        p.push_back({ false, "No VR runtime seems to be running. Put the headset on Link first; without"
                             " it the game starts normally and the headset stays dark." });
    }
    return p;
}

} // namespace Cs2VrLauncher

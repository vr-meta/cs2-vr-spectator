// Tests for tools/launcher/LauncherLogic.h - the launcher's decisions, with no Windows,
// no Steam and no headset in them. Same harness and the same reason as test-mirv-vr-math:
// the part that can be checked at a desk should be, because the rest needs a Quest.

#include <string.h>

#include "check.h"
#include "../tools/launcher/LauncherLogic.h"

using namespace Cs2VrLauncher;

// The shape Steam writes, tabs and all, with a library that has CS2 and one that has not.
static const char * kLibraryFolders =
    "\"libraryfolders\"\n"
    "{\n"
    "\t\"0\"\n"
    "\t{\n"
    "\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
    "\t\t\"label\"\t\t\"\"\n"
    "\t\t\"contentid\"\t\t\"1234567890123456789\"\n"
    "\t\t\"apps\"\n"
    "\t\t{\n"
    "\t\t\t\"228980\"\t\t\"123456\"\n"
    "\t\t}\n"
    "\t}\n"
    "\t\"1\"\n"
    "\t{\n"
    "\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n"
    "\t\t\"label\"\t\t\"\"\n"
    "\t\t\"apps\"\n"
    "\t\t{\n"
    "\t\t\t\"730\"\t\t\"71589381210\"\n"
    "\t\t}\n"
    "\t}\n"
    "}\n";

static const char * kAppManifest =
    "\"AppState\"\n"
    "{\n"
    "\t\"appid\"\t\t\"730\"\n"
    "\t\"name\"\t\t\"Counter-Strike 2\"\n"
    "\t\"installdir\"\t\t\"Counter-Strike Global Offensive\"\n"
    "\t\"UserConfig\"\n"
    "\t{\n"
    "\t\t\"language\"\t\t\"english\"\n"
    "\t}\n"
    "}\n";

static void TestVdfValues() {
    check::Case("Steam's library list gives up its paths, unescaped");

    std::vector<std::string> paths = VdfValues(kLibraryFolders, "path");
    CHECK(2 == paths.size());
    if (2 == paths.size()) {
        CHECK_STR(paths[0].c_str(), "C:\\Program Files (x86)\\Steam");
        CHECK_STR(paths[1].c_str(), "D:\\SteamLibrary");
    }

    // The manifest's install directory is NOT the game's name, and that is the point of
    // reading it: CS2 still lives in a folder called Global Offensive.
    std::vector<std::string> dir = VdfValues(kAppManifest, "installdir");
    CHECK(1 == dir.size());
    if (1 == dir.size()) CHECK_STR(dir[0].c_str(), "Counter-Strike Global Offensive");

    // A key that opens a block has no value; the first key inside it is not its value.
    CHECK(VdfValues(kLibraryFolders, "apps").empty());
    CHECK(VdfValues(kAppManifest, "UserConfig").empty());
    CHECK(VdfValues(kLibraryFolders, "libraryfolders").empty());

    // An empty value is still a value.
    std::vector<std::string> labels = VdfValues(kLibraryFolders, "label");
    CHECK(2 == labels.size());
    if (2 == labels.size()) CHECK_STR(labels[0].c_str(), "");

    // Keys are matched whatever their case; Valve has not been consistent about it.
    CHECK(2 == VdfValues(kLibraryFolders, "PATH").size());

    // Comments, a quote inside a value, and a file that stops mid-string.
    CHECK_STR(VdfValues("// \"path\" \"nope\"\n\"path\" \"yes\"\n", "path")[0].c_str(), "yes");
    CHECK_STR(VdfValues("\"name\" \"say \\\"hi\\\"\"\n", "name")[0].c_str(), "say \"hi\"");
    CHECK(VdfValues("\"path\" \"C:\\\\unterminated", "path").size() <= 1);
    CHECK(VdfValues("", "path").empty());
}

static void TestInfValue() {
    check::Case("steam.inf says which build the game is");

    const char * inf = "ClientVersion=2000908\r\nServerVersion=2000908\r\nPatchVersion=1.41.8.1\r\nappID=730\r\n";
    CHECK_STR(InfValue(inf, "ClientVersion").c_str(), "2000908");
    CHECK_STR(InfValue(inf, "PatchVersion").c_str(), "1.41.8.1");
    CHECK_STR(InfValue(inf, "appID").c_str(), "730");

    // A key that is only the beginning of another key is not that key.
    CHECK_STR(InfValue(inf, "Client").c_str(), "");
    CHECK_STR(InfValue(inf, "Missing").c_str(), "");
    CHECK_STR(InfValue("", "ClientVersion").c_str(), "");

    // No trailing newline on the last line.
    CHECK_STR(InfValue("A=1\nClientVersion=42", "ClientVersion").c_str(), "42");
}

static void TestPaths() {
    check::Case("everything inside a Steam library is found from two strings");

    Cs2Paths p = Cs2PathsIn("D:\\SteamLibrary", "Counter-Strike Global Offensive");
    CHECK_STR(p.exe.c_str(),
        "D:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\cs2.exe");
    CHECK_STR(p.steamInf.c_str(),
        "D:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\game\\csgo\\steam.inf");
    // The one directory this project writes to inside Valve's tree, and it is its own.
    CHECK_STR(p.cfgDir.c_str(),
        "D:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\game\\csgo\\cfg\\cs2vr");

    // A library path that already ends in a separator does not get a second one.
    CHECK_STR(Cs2PathsIn("E:\\", "X").root.c_str(), "E:\\steamapps\\common\\X");
    CHECK_STR(JoinPath("", "a").c_str(), "a");
    CHECK_STR(JoinPath("a", "").c_str(), "a");
}

static void TestCheckBuild() {
    check::Case("a game update is said in words, before anything is launched");

    BuildCheck same = CheckBuild("ClientVersion=2000908\n", "2000908");
    CHECK(kBuildMatches == same.verdict);

    BuildCheck other = CheckBuild("ClientVersion=2000915\n", "2000908");
    CHECK(kBuildDiffers == other.verdict);
    CHECK_STR(other.installed.c_str(), "2000915");
    // Both numbers are in the sentence: that is what somebody pastes into an issue.
    CHECK(std::string::npos != other.message.find("2000915"));
    CHECK(std::string::npos != other.message.find("2000908"));

    BuildCheck unknown = CheckBuild("", "2000908");
    CHECK(kBuildUnknown == unknown.verdict);
    CHECK(std::string::npos != unknown.message.find("2000908"));
}

static void TestChooseRuntime() {
    check::Case("a Quest on Link gets Meta's runtime even when SteamVR is the default");

    std::vector<std::string> both;
    both.push_back("D:\\SteamLibrary\\steamapps\\common\\SteamVR\\steamxr_win64.json");
    both.push_back("C:\\Program Files\\Meta Horizon\\Support\\oculus-runtime\\oculus_openxr_64.json");
    const std::string steamActive = both[0];

    // The case that froze a session: registry says SteamVR, headset is on Link.
    CHECK(1 == ChooseRuntime(both, steamActive, true, ""));
    // No Meta service: respect what the machine is set to.
    CHECK(0 == ChooseRuntime(both, steamActive, false, ""));
    // Asked for by name, whatever is running.
    CHECK(0 == ChooseRuntime(both, steamActive, true, "steamvr"));
    CHECK(1 == ChooseRuntime(both, steamActive, false, "meta"));
    CHECK(1 == ChooseRuntime(both, steamActive, false, "oculus"));
    // Asked for something that is not installed: leave it to the loader, do not guess.
    std::vector<std::string> onlySteam(1, both[0]);
    CHECK(-1 == ChooseRuntime(onlySteam, steamActive, false, "meta"));
    // Meta's service is up but its runtime is not registered: fall back to the active one.
    CHECK(0 == ChooseRuntime(onlySteam, steamActive, true, ""));
    // Nothing registered at all.
    CHECK(-1 == ChooseRuntime(std::vector<std::string>(), "", true, ""));
}

static void TestBuildGameArgs() {
    check::Case("there is no command line without -insecure, a size and a frame cap");

    LaunchOptions menu;
    std::string a = BuildGameArgs(menu);
    CHECK(std::string::npos != a.find("-insecure"));
    CHECK(std::string::npos != a.find("-w 2528"));
    CHECK(std::string::npos != a.find("-h 2780"));
    CHECK(std::string::npos != a.find("+fps_max 90"));
    CHECK(std::string::npos != a.find("-condebug"));
    CHECK(std::string::npos != a.find("+exec cs2vr/vr"));
    CHECK(std::string::npos == a.find("+playdemo"));

    // Nonsense sizes and caps do not reach the game; an uncapped or unsized launch is how
    // two separate evenings were lost.
    LaunchOptions bad; bad.width = 0; bad.height = -5; bad.fpsMax = 0;
    std::string b = BuildGameArgs(bad);
    CHECK(std::string::npos != b.find("-w 2528"));
    CHECK(std::string::npos != b.find("-h 2780"));
    CHECK(std::string::npos != b.find("+fps_max 90"));

    LaunchOptions demo; demo.demo = "my demos\\final map.dem"; demo.width = 2064; demo.height = 2208;
    std::string d = BuildGameArgs(demo);
    CHECK(std::string::npos != d.find("+playdemo \"my demos\\final map.dem\""));
    CHECK(std::string::npos != d.find("-w 2064"));

    // An offline game with bots: there is no console in a worn launch to type this into.
    LaunchOptions play; play.map = "de_inferno";
    std::string p = BuildGameArgs(play);
    CHECK(std::string::npos != p.find("+sv_lan 1"));
    CHECK(std::string::npos != p.find("+bot_quota 6"));
    CHECK(std::string::npos != p.find("+map de_inferno"));
    CHECK(std::string::npos == p.find("+playdemo"));
    // +map has to come last of these: the settings before it are what the map loads with.
    CHECK(p.find("+sv_lan 1") < p.find("+map de_inferno"));

    // A demo wins over a map if somebody manages to give both.
    LaunchOptions both; both.map = "de_inferno"; both.demo = "x.dem";
    CHECK(std::string::npos == BuildGameArgs(both).find("+map"));

    // A "map name" with a semicolon or a space in it is a second command, not a map.
    CHECK(IsPlainMapName("de_inferno"));
    CHECK(IsPlainMapName("workshop/123456789/aim_map-v2"));
    CHECK(!IsPlainMapName(""));
    CHECK(!IsPlainMapName("de_inferno;quit"));
    CHECK(!IsPlainMapName("de_inferno +connect 1.2.3.4"));
    CHECK(!IsPlainMapName("\"de_inferno\""));
    LaunchOptions sly; sly.map = "de_dust2;connect 1.2.3.4";
    CHECK(std::string::npos == BuildGameArgs(sly).find("connect"));
    CHECK(std::string::npos == BuildGameArgs(sly).find("+map"));

    LaunchOptions crowd; crowd.map = "de_nuke"; crowd.bots = 500;
    CHECK(std::string::npos != BuildGameArgs(crowd).find("+bot_quota 20"));

    // Whatever a developer appends, -insecure is still there and still first.
    LaunchOptions extra; extra.extra = "-vulkan";
    std::string e = BuildGameArgs(extra);
    CHECK(0 == e.find("-insecure"));
    CHECK(std::string::npos != e.find(" -vulkan"));
}

static void TestQuoteArg() {
    check::Case("an argument survives the trip through a Windows command line");

    CHECK_STR(QuoteArg("plain.dem").c_str(), "plain.dem");
    CHECK_STR(QuoteArg("two words.dem").c_str(), "\"two words.dem\"");
    CHECK_STR(QuoteArg("").c_str(), "\"\"");
    // A trailing backslash inside quotes has to be doubled or it eats the closing quote.
    CHECK_STR(QuoteArg("C:\\my demos\\").c_str(), "\"C:\\my demos\\\\\"");
    CHECK_STR(QuoteArg("say \"hi\"").c_str(), "\"say \\\"hi\\\"\"");
}

static void TestPreflight() {
    check::Case("each reason not to start is said, with why it matters");

    Machine ready;
    ready.hookPresent = true; ready.cs2Found = true; ready.metaServiceRunning = true;
    CHECK(Preflight(ready, false).empty());

    Machine steamVr = ready; steamVr.steamVrRunning = true;
    std::vector<Problem> p = Preflight(steamVr, false);
    CHECK(1 == p.size());
    if (1 == p.size()) CHECK(p[0].blocking);
    // ...unless somebody insists, and then it is still said.
    p = Preflight(steamVr, true);
    CHECK(1 == p.size());
    if (1 == p.size()) CHECK(!p[0].blocking);

    Machine running = ready; running.cs2Running = true;
    p = Preflight(running, false);
    CHECK(1 == p.size() && p[0].blocking);

    Machine noHook = ready; noHook.hookPresent = false;
    p = Preflight(noHook, false);
    CHECK(1 == p.size() && p[0].blocking);

    Machine noGame = ready; noGame.cs2Found = false;
    p = Preflight(noGame, false);
    CHECK(1 == p.size() && p[0].blocking);

    // No runtime at all is a warning, not a refusal: the menu is still worth reaching.
    Machine dark = ready; dark.metaServiceRunning = false;
    p = Preflight(dark, false);
    CHECK(1 == p.size() && !p[0].blocking);
}

static void RunTests() {
    TestVdfValues();
    TestInfValue();
    TestPaths();
    TestCheckBuild();
    TestChooseRuntime();
    TestBuildGameArgs();
    TestQuoteArg();
    TestPreflight();
}

CHECK_MAIN()

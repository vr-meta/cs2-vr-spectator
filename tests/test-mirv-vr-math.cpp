// Tests for the pure half of the VR module.
//
// These do not need CS2, a headset, or Windows. That is the point: the expensive parts of
// this project can only be checked by putting a headset on, so anything that can be
// pinned down here should be.

#include "check.h"

#include "../src/AfxHookSource2/MirvVrMath.h"

#include <string.h>

using namespace AfxVrMath;

// ---------------------------------------------------------------------------------

static void TestAngleVectors() {
    check::Case("AngleVectors matches Source's convention");

    float forward[3], right[3], up[3];

    // Looking down +X with no pitch or roll.
    {
        float angles[3] = { 0.0f, 0.0f, 0.0f };
        AngleVectors(angles, forward, right, up);
        CHECK_NEAR(forward[0], 1.0, 1e-6);
        CHECK_NEAR(forward[1], 0.0, 1e-6);
        CHECK_NEAR(forward[2], 0.0, 1e-6);
        // Source's "right" is -Y at zero yaw. An eye offset of +right therefore moves the
        // camera towards -Y, and getting this backwards swaps the eyes -- which reads as
        // depth turned inside out and is very hard to diagnose from inside a headset.
        CHECK_NEAR(right[0], 0.0, 1e-6);
        CHECK_NEAR(right[1], -1.0, 1e-6);
        CHECK_NEAR(right[2], 0.0, 1e-6);
        CHECK_NEAR(up[2], 1.0, 1e-6);
    }

    // Yaw is counter-clockwise seen from above: +90 degrees faces +Y.
    {
        float angles[3] = { 0.0f, 90.0f, 0.0f };
        AngleVectors(angles, forward, right, up);
        CHECK_NEAR(forward[0], 0.0, 1e-6);
        CHECK_NEAR(forward[1], 1.0, 1e-6);
        CHECK_NEAR(right[0], 1.0, 1e-6);
        CHECK_NEAR(right[1], 0.0, 1e-6);
    }

    // Positive pitch looks *down* in Source. This sign trips everyone.
    {
        float angles[3] = { 90.0f, 0.0f, 0.0f };
        AngleVectors(angles, forward, right, up);
        CHECK_NEAR(forward[2], -1.0, 1e-6);
    }

    // The three vectors stay a right-handed orthonormal frame at an awkward attitude.
    {
        float angles[3] = { 23.0f, -147.0f, 11.0f };
        AngleVectors(angles, forward, right, up);

        float lengths[3] = { 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < 3; i++) {
            lengths[0] += forward[i] * forward[i];
            lengths[1] += right[i] * right[i];
            lengths[2] += up[i] * up[i];
        }
        CHECK_NEAR(lengths[0], 1.0, 1e-5);
        CHECK_NEAR(lengths[1], 1.0, 1e-5);
        CHECK_NEAR(lengths[2], 1.0, 1e-5);

        float fr = 0.0f, fu = 0.0f, ru = 0.0f;
        for (int i = 0; i < 3; i++) {
            fr += forward[i] * right[i];
            fu += forward[i] * up[i];
            ru += right[i] * up[i];
        }
        CHECK_NEAR(fr, 0.0, 1e-5);
        CHECK_NEAR(fu, 0.0, 1e-5);
        CHECK_NEAR(ru, 0.0, 1e-5);
    }

    // Null outputs are allowed; the eye code only ever wants two of the three.
    {
        float angles[3] = { 5.0f, 5.0f, 5.0f };
        AngleVectors(angles, forward, 0, 0);
        CHECK(forward[0] != 0.0f);
    }
}

static void TestEyeSeparation() {
    check::Case("eyes separate across the gaze, not along a world axis");

    // This is the property the stereo pair depends on. An interpupillary distance of
    // 63 mm is 2.5 units, and whichever way the viewer faces, the two eyes must end up
    // 2.5 units apart on a line perpendicular to where they are looking.
    const float ipd = 2.5f;
    const float yaws[] = { 0.0f, 37.0f, 90.0f, 180.0f, -123.0f };

    for (int k = 0; k < (int)(sizeof(yaws) / sizeof(yaws[0])); k++) {
        float angles[3] = { 12.0f, yaws[k], 0.0f };
        float forward[3], right[3], up[3];
        AngleVectors(angles, forward, right, up);

        float left[3], rightEye[3];
        for (int i = 0; i < 3; i++) {
            left[i]     = -0.5f * ipd * right[i];
            rightEye[i] = +0.5f * ipd * right[i];
        }

        float separation = 0.0f, alongGaze = 0.0f;
        for (int i = 0; i < 3; i++) {
            float d = rightEye[i] - left[i];
            separation += d * d;
            alongGaze += d * forward[i];
        }
        CHECK_NEAR(sqrt(separation), ipd, 1e-4);
        CHECK_NEAR(alongGaze, 0.0, 1e-4);
    }
}

static void TestMoveInViewPlane() {
    check::Case("stick movement goes where the viewer is looking");

    float d[3];

    // Facing +X: "forward" is +X, "right" is -Y.
    MoveInViewPlane(0.0f, 0.0f, 10.0f, 0.0f, d);
    CHECK_NEAR(d[0], 10.0, 1e-5);
    CHECK_NEAR(d[1], 0.0, 1e-5);
    CHECK_NEAR(d[2], 0.0, 1e-5);

    MoveInViewPlane(0.0f, 10.0f, 0.0f, 0.0f, d);
    CHECK_NEAR(d[0], 0.0, 1e-5);
    CHECK_NEAR(d[1], -10.0, 1e-5);

    // Turned a quarter turn, "forward" follows.
    MoveInViewPlane(90.0f, 0.0f, 10.0f, 0.0f, d);
    CHECK_NEAR(d[0], 0.0, 1e-5);
    CHECK_NEAR(d[1], 10.0, 1e-5);

    // Vertical is vertical regardless of facing -- pushing up must not depend on where
    // the viewer's nose happens to point.
    MoveInViewPlane(215.0f, 0.0f, 0.0f, 7.0f, d);
    CHECK_NEAR(d[0], 0.0, 1e-5);
    CHECK_NEAR(d[1], 0.0, 1e-5);
    CHECK_NEAR(d[2], 7.0, 1e-5);

    // Moving forward then back returns to where you started, at any heading.
    for (float yaw = -180.0f; yaw < 180.0f; yaw += 31.0f) {
        float a[3], b[3];
        MoveInViewPlane(yaw, 3.0f, 5.0f, 1.0f, a);
        MoveInViewPlane(yaw, -3.0f, -5.0f, -1.0f, b);
        for (int i = 0; i < 3; i++) CHECK_NEAR(a[i] + b[i], 0.0, 1e-5);
    }
}

static void TestNormalizeDegrees() {
    check::Case("angles fold into (-180, 180]");

    CHECK_NEAR(NormalizeDegrees(0.0f), 0.0, 1e-5);
    CHECK_NEAR(NormalizeDegrees(180.0f), 180.0, 1e-5);
    CHECK_NEAR(NormalizeDegrees(181.0f), -179.0, 1e-4);
    CHECK_NEAR(NormalizeDegrees(-180.0f), 180.0, 1e-4);
    CHECK_NEAR(NormalizeDegrees(720.0f + 45.0f), 45.0, 1e-3);
    CHECK_NEAR(NormalizeDegrees(-720.0f - 45.0f), -45.0, 1e-3);
}

static void TestDeadzone() {
    check::Case("deadzone rescales so the stick still reaches full throw");

    const float dz = 0.18f;

    CHECK_NEAR(ApplyDeadzone(0.0f, dz), 0.0, 1e-6);
    CHECK_NEAR(ApplyDeadzone(0.1f, dz), 0.0, 1e-6);
    CHECK_NEAR(ApplyDeadzone(-0.1f, dz), 0.0, 1e-6);

    // Just past the edge the output starts from zero, not from the deadzone value: the
    // stick must not jump when it wakes up.
    CHECK_NEAR(ApplyDeadzone(dz + 0.0001f, dz), 0.0, 1e-3);

    // And full deflection still means full speed.
    CHECK_NEAR(ApplyDeadzone(1.0f, dz), 1.0, 1e-6);
    CHECK_NEAR(ApplyDeadzone(-1.0f, dz), -1.0, 1e-6);

    // Monotonic and odd.
    float previous = -2.0f;
    for (float v = -1.0f; v <= 1.0f; v += 0.05f) {
        float shaped = ApplyDeadzone(v, dz);
        CHECK(shaped >= previous - 1e-6f);
        previous = shaped;
        CHECK_NEAR(shaped, -ApplyDeadzone(-v, dz), 1e-5);
    }

    // A nonsensical deadzone must not produce a division by zero or a sign flip.
    CHECK_NEAR(ApplyDeadzone(0.5f, 0.0f), 0.5, 1e-6);
    CHECK(ApplyDeadzone(1.0f, 5.0f) > 0.0f);
    CHECK_NEAR(ApplyDeadzone(0.5f, -1.0f), 0.5, 1e-6);
}

static void TestResponseCurve() {
    check::Case("response curve keeps the ends and softens the middle");

    CHECK_NEAR(ApplyResponseCurve(0.0f, 2.0f), 0.0, 1e-6);
    CHECK_NEAR(ApplyResponseCurve(1.0f, 2.0f), 1.0, 1e-6);
    CHECK_NEAR(ApplyResponseCurve(-1.0f, 2.0f), -1.0, 1e-6);

    // Half throw with a square curve is a quarter speed -- that is the whole point.
    CHECK_NEAR(ApplyResponseCurve(0.5f, 2.0f), 0.25, 1e-6);
    CHECK_NEAR(ApplyResponseCurve(-0.5f, 2.0f), -0.25, 1e-6);

    // Linear is a pass-through.
    CHECK_NEAR(ApplyResponseCurve(0.37f, 1.0f), 0.37, 1e-6);

    // A rubbish exponent leaves the value alone rather than producing NaN.
    CHECK_NEAR(ApplyResponseCurve(0.37f, 0.0f), 0.37, 1e-6);
    CHECK_NEAR(ApplyResponseCurve(0.37f, -3.0f), 0.37, 1e-6);
}

static void TestSnapTurn() {
    check::Case("snap turn fires once per push");

    SnapTurnState state;

    // Below the threshold: nothing.
    CHECK_NEAR(SnapTurn(state, 0.5f, 30.0f), 0.0, 1e-6);

    // Over it: one step. Push right, turn right, which is negative yaw in Source.
    CHECK_NEAR(SnapTurn(state, 0.9f, 30.0f), -30.0, 1e-6);

    // Holding it over does not keep turning -- this is the bug snap turning exists to
    // avoid, and it is invisible until someone holds the stick.
    CHECK_NEAR(SnapTurn(state, 0.9f, 30.0f), 0.0, 1e-6);
    CHECK_NEAR(SnapTurn(state, 1.0f, 30.0f), 0.0, 1e-6);

    // Coming back only part way is not enough to re-arm; hysteresis keeps a shaky hand
    // from double-firing.
    CHECK_NEAR(SnapTurn(state, 0.5f, 30.0f), 0.0, 1e-6);
    CHECK(!state.armed);

    // Released, then pushed the other way.
    CHECK_NEAR(SnapTurn(state, 0.0f, 30.0f), 0.0, 1e-6);
    CHECK(state.armed);
    CHECK_NEAR(SnapTurn(state, -0.9f, 30.0f), 30.0, 1e-6);
}

static void TestCheckView() {
    check::Case("a plausible camera passes and rubbish does not");

    // A real reading, from the mirage demo.
    {
        float origin[3] = { -1234.5f, 567.25f, 96.0f };
        float angles[3] = { 12.5f, -45.0f, 0.0f };
        ViewCheck r = CheckView(origin, angles, 90.0f);
        CHECK(r.ok);
        CHECK(r.why == 0);
    }

    // The centre of the map with a default field of view.
    {
        float origin[3] = { 0.0f, 0.0f, 0.0f };
        float angles[3] = { 0.0f, 0.0f, 0.0f };
        CHECK(CheckView(origin, angles, 90.0f).ok);
    }

    // What a moved offset actually looks like: a pointer read as two floats. The low half
    // of a 64-bit address is a large-magnitude float and the high half is tiny, so the
    // field of view is what catches this -- which is why it is checked at all.
    {
        float origin[3] = { 0.0f, 0.0f, 0.0f };
        float angles[3] = { 0.0f, 0.0f, 0.0f };
        unsigned int lowHalf = 0x12345678u;
        float asFloat;
        memcpy(&asFloat, &lowHalf, sizeof(asFloat));
        ViewCheck r = CheckView(origin, angles, asFloat);
        CHECK(!r.ok);
        CHECK(r.why != 0);
    }

    // Uninitialised memory, the other common shape.
    {
        float origin[3] = { 0.0f, 0.0f, 0.0f };
        float angles[3] = { 0.0f, 0.0f, 0.0f };
        CHECK(!CheckView(origin, angles, 0.0f).ok);
        CHECK(!CheckView(origin, angles, 1e30f).ok);
        CHECK(!CheckView(origin, angles, -90.0f).ok);
    }

    // Outside any map.
    {
        float origin[3] = { 1e9f, 0.0f, 0.0f };
        float angles[3] = { 0.0f, 0.0f, 0.0f };
        ViewCheck r = CheckView(origin, angles, 90.0f);
        CHECK(!r.ok);
    }

    // Not-a-number, which compares false against everything and would otherwise slip
    // through every range test written the obvious way.
    {
        // Built from its bits rather than from 0.0/0.0, which some compilers fold and
        // others refuse outright.
        float nan;
        unsigned int nanBits = 0x7fc00000u;
        memcpy(&nan, &nanBits, sizeof(nan));

        float origin[3] = { nan, 0.0f, 0.0f };
        float angles[3] = { 0.0f, 0.0f, 0.0f };
        CHECK(!CheckView(origin, angles, 90.0f).ok);

        float origin2[3] = { 0.0f, 0.0f, 0.0f };
        float angles2[3] = { 0.0f, nan, 0.0f };
        CHECK(!CheckView(origin2, angles2, 90.0f).ok);
        CHECK(!CheckView(origin2, angles, nan).ok);
    }

    // Angles at the edge of what the engine produces are still fine: pitch is clamped to
    // +-90 but yaw can arrive unwrapped.
    {
        float origin[3] = { 100.0f, 100.0f, 100.0f };
        float angles[3] = { -89.0f, 359.0f, 0.0f };
        CHECK(CheckView(origin, angles, 106.0f).ok);
    }
}

static void TestSteamInfValue() {
    check::Case("steam.inf parses the way the file is actually written");

    const char * inf =
        "ClientVersion=2000908\r\n"
        "ServerVersion=2000908\r\n"
        "PatchVersion=1.41.8.1\r\n"
        "ProductName=cs2\r\n"
        "appID=730\r\n";

    char value[64];

    CHECK(SteamInfValue(inf, "ClientVersion", value, sizeof(value)));
    CHECK_STR(value, "2000908");

    CHECK(SteamInfValue(inf, "PatchVersion", value, sizeof(value)));
    CHECK_STR(value, "1.41.8.1");

    CHECK(SteamInfValue(inf, "appID", value, sizeof(value)));
    CHECK_STR(value, "730");

    // The last line, with no trailing newline.
    const char * noNewline = "ProductName=cs2";
    CHECK(SteamInfValue(noNewline, "ProductName", value, sizeof(value)));
    CHECK_STR(value, "cs2");

    // A key that is a prefix of another must not match it. "Version=" would otherwise be
    // answered by "ClientVersion=", which is the kind of thing that reports the wrong
    // build number and is believed.
    CHECK(!SteamInfValue(inf, "Version", value, sizeof(value)));
    CHECK(!SteamInfValue(inf, "Client", value, sizeof(value)));
    CHECK(!SteamInfValue(inf, "Nonsense", value, sizeof(value)));

    // Unix line endings, in case the file is ever rewritten by a tool.
    const char * unixInf = "ClientVersion=2000908\nProductName=cs2\n";
    CHECK(SteamInfValue(unixInf, "ClientVersion", value, sizeof(value)));
    CHECK_STR(value, "2000908");

    // Too small a buffer fails rather than truncating into a plausible-looking answer.
    char tiny[4];
    CHECK(!SteamInfValue(inf, "ClientVersion", tiny, sizeof(tiny)));

    // Degenerate input.
    CHECK(!SteamInfValue(0, "ClientVersion", value, sizeof(value)));
    CHECK(!SteamInfValue(inf, "", value, sizeof(value)));
    CHECK(!SteamInfValue(inf, "ClientVersion", value, 0));
}

static void TestSteamInfPath() {
    check::Case("steam.inf is found relative to the running executable");

    char path[512];

    CHECK(SteamInfPathFromExe(
        "D:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\cs2.exe",
        path, sizeof(path)));
    CHECK_STR(path,
        "D:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\game\\csgo\\steam.inf");

    // Forward slashes, which is what some launchers hand over.
    CHECK(SteamInfPathFromExe("C:/Games/cs2/game/bin/win64/cs2.exe", path, sizeof(path)));
    CHECK_STR(path, "C:/Games/cs2/game\\csgo\\steam.inf");

    // Not deep enough to be a CS2 install: refuse rather than invent a path.
    CHECK(!SteamInfPathFromExe("cs2.exe", path, sizeof(path)));
    CHECK(!SteamInfPathFromExe("C:\\cs2.exe", path, sizeof(path)));
    CHECK(!SteamInfPathFromExe("", path, sizeof(path)));
    CHECK(!SteamInfPathFromExe(0, path, sizeof(path)));

    char tiny[8];
    CHECK(!SteamInfPathFromExe("C:/Games/cs2/game/bin/win64/cs2.exe", tiny, sizeof(tiny)));
}

// ---------------------------------------------------------------------------------

static void RunTests() {
    TestAngleVectors();
    TestEyeSeparation();
    TestMoveInViewPlane();
    TestNormalizeDegrees();
    TestDeadzone();
    TestResponseCurve();
    TestSnapTurn();
    TestCheckView();
    TestSteamInfValue();
    TestSteamInfPath();
}

CHECK_MAIN()

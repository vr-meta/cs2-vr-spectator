// Tests for the pure half of the VR module.
//
// These do not need CS2, a headset, or Windows. That is the point: the expensive parts of
// this project can only be checked by putting a headset on, so anything that can be
// pinned down here should be.

#include "check.h"

#include "../src/AfxHookSource2/MirvVrMath.h"

#include <string.h>
#include <wchar.h>

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

static void TestPathRelativeToFile() {
    check::Case("things next to the hook are found without knowing where the hook is");

    char out[512];

    // The shape the release has: openxr_loader.dll beside the hook DLL.
    CHECK(PathRelativeToFile("C:/Program Files/cs2vr/hook/AfxHookSource2.dll", 0,
        "\\openxr_loader.dll", out, sizeof(out)));
    CHECK_STR(out, "C:/Program Files/cs2vr/hook\\openxr_loader.dll");

    // The shape the development tree has: the hook one level down, in the x64 directory.
    CHECK(PathRelativeToFile("D:\\Dev\\hlae\\x64\\AfxHookSource2.dll", 1,
        "\\openxr_loader.dll", out, sizeof(out)));
    CHECK_STR(out, "D:\\Dev\\hlae\\openxr_loader.dll");

    // Wide, which is what LoadLibraryW needs and the only version that survives a folder
    // name with a character outside ASCII in it.
    wchar_t wide[512];
    CHECK(PathRelativeToFile(L"C:\\Users\\Paul\\cs2vr\\hook\\AfxHookSource2.dll", 0,
        L"\\openxr_loader.dll", wide, 512));
    CHECK(0 == wcscmp(wide, L"C:\\Users\\Paul\\cs2vr\\hook\\openxr_loader.dll"));

    // Not deep enough, or not a path at all: refuse rather than invent one. A relative
    // "openxr_loader.dll" is what LoadLibrary would search the process directories for,
    // and that is the fallback the caller adds deliberately - not something this should
    // produce by accident.
    CHECK(!PathRelativeToFile("AfxHookSource2.dll", 0, "\\x.dll", out, sizeof(out)));
    CHECK(!PathRelativeToFile("C:\\a.dll", 1, "\\x.dll", out, sizeof(out)));
    CHECK(!PathRelativeToFile("", 0, "\\x.dll", out, sizeof(out)));
    CHECK(!PathRelativeToFile((const char *)0, 0, "\\x.dll", out, sizeof(out)));
    CHECK(!PathRelativeToFile("C:\\a\\b.dll", 0, (const char *)0, out, sizeof(out)));
    CHECK(!PathRelativeToFile("C:\\a\\b.dll", -1, "\\x.dll", out, sizeof(out)));

    // Too small to hold the answer: say so, do not write half of it.
    char tiny[8];
    CHECK(!PathRelativeToFile("C:\\a\\b\\c.dll", 0, "\\openxr_loader.dll", tiny, sizeof(tiny)));
    CHECK_STR(tiny, "");
}

// ---------------------------------------------------------------------------------

static void TestQuatToSourceAngles(); // defined below, after the helpers it needs
static void TestComposeSourceAngles();
static void TestShouldRestoreBaseView();
static void TestYawThenPitchQuat();
static void TestRoomOffsetToWorld();
static void TestRegionPlacementRoundTrip();
static void TestRayQuadHit();
static void TestDecideMode();
static void TestBodyTurnStep();
static void TestTakeWholeUnits();
static void TestStickToGameFrame();
static void TestCountGainEstimator();
static void TestAimServo();

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
    TestPathRelativeToFile();
    TestQuatToSourceAngles();
    TestComposeSourceAngles();
    TestShouldRestoreBaseView();
    TestYawThenPitchQuat();
    TestRoomOffsetToWorld();
    TestRegionPlacementRoundTrip();
    TestRayQuadHit();
    TestDecideMode();
    TestBodyTurnStep();
    TestTakeWholeUnits();
    TestStickToGameFrame();
    TestCountGainEstimator();
    TestAimServo();
}

CHECK_MAIN()

// ---------------------------------------------------------------------------------

// Build a quaternion for a rotation of `degrees` about an axis, in OpenXR's frame.
static void AxisAngle(float ax, float ay, float az, float degrees,
                      float & qx, float & qy, float & qz, float & qw) {
    float r = (float)(degrees * 3.14159265358979323846 / 180.0);
    float s = sinf(0.5f * r);
    qx = ax * s; qy = ay * s; qz = az * s; qw = cosf(0.5f * r);
}

static void QuatMultiply(float ax, float ay, float az, float aw,
                         float bx, float by, float bz, float bw,
                         float & ox, float & oy, float & oz, float & ow) {
    ow = aw * bw - ax * bx - ay * by - az * bz;
    ox = aw * bx + ax * bw + ay * bz - az * by;
    oy = aw * by - ax * bz + ay * bw + az * bx;
    oz = aw * bz + ax * by - ay * bx + az * bw;
}

static void TestQuatToSourceAngles() {
    check::Case("a quaternion converts to Source angles that describe the same rotation");

    // The check that does not simply restate the assumption: take the angles this function
    // produces, run them through Source's own AngleVectors, and compare the resulting
    // basis against rotating the basis vectors by the quaternion directly. If the two
    // agree, the conversion describes the same rotation whatever convention either side
    // happens to use internally.
    //
    // It matters because two eyes with different orientations no longer cancel a shared
    // error between them. A wrong conversion then shows as one horizontal line in the
    // world appearing at two different angles in the two eyes - which is what it did.
    struct Case { float axisX, axisY, axisZ, degrees; const char * what; };
    const Case cases[] = {
        { 0, 1, 0,   0.0f, "identity" },
        { 0, 1, 0,  30.0f, "yaw left" },
        { 0, 1, 0, -30.0f, "yaw right" },
        { 1, 0, 0,  25.0f, "pitch up" },
        { 1, 0, 0, -25.0f, "pitch down" },
        { 0, 0, 1,  20.0f, "roll" },
        { 0, 0, 1, -20.0f, "roll the other way" },
    };

    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        float qx, qy, qz, qw;
        AxisAngle(cases[i].axisX, cases[i].axisY, cases[i].axisZ, cases[i].degrees, qx, qy, qz, qw);

        float pitch, yaw, roll;
        QuatToSourceAngles(qx, qy, qz, qw, pitch, yaw, roll);

        float angles[3] = { pitch, yaw, roll };
        float fromAngles[3][3];
        AngleVectors(angles, fromAngles[0], fromAngles[1], fromAngles[2]);

        // The same three directions, obtained by rotating OpenXR's basis and mapping into
        // Source's world. Forward is -Z, right is +X, up is +Y.
        const float basis[3][3] = { { 0, 0, -1 }, { 1, 0, 0 }, { 0, 1, 0 } };
        for (int v = 0; v < 3; v++) {
            float rx, ry, rz;
            QuatRotate(qx, qy, qz, qw, basis[v][0], basis[v][1], basis[v][2], rx, ry, rz);
            float sx, sy, sz;
            XrDirectionToSource(rx, ry, rz, sx, sy, sz);

            CHECK_NEAR(fromAngles[v][0], sx, 2e-3);
            CHECK_NEAR(fromAngles[v][1], sy, 2e-3);
            CHECK_NEAR(fromAngles[v][2], sz, 2e-3);
        }
    }

    // And the case that actually bit: a head that is pitched, with a small extra yaw
    // applied about its OWN up axis - which is what pointing an eye at its frustum centre
    // does. If the conversion is wrong, the roll that comes out differs between an eye
    // turned one way and an eye turned the other, and the two images rotate apart.
    {
        float hx, hy, hz, hw;
        AxisAngle(1, 0, 0, -25.0f, hx, hy, hz, hw); // head pitched down

        float rollLeft = 0.0f, rollRight = 0.0f;
        for (int side = 0; side < 2; side++) {
            float ox, oy, oz, ow;
            AxisAngle(0, 1, 0, side ? -7.0f : 7.0f, ox, oy, oz, ow);

            float qx, qy, qz, qw;
            QuatMultiply(hx, hy, hz, hw, ox, oy, oz, ow, qx, qy, qz, qw);

            float pitch, yaw, roll;
            QuatToSourceAngles(qx, qy, qz, qw, pitch, yaw, roll);

            float angles[3] = { pitch, yaw, roll };
            float f[3], r[3], u[3];
            AngleVectors(angles, f, r, u);

            float fx, fy, fz;
            QuatRotate(qx, qy, qz, qw, 0.0f, 0.0f, -1.0f, fx, fy, fz);
            float sx, sy, sz;
            XrDirectionToSource(fx, fy, fz, sx, sy, sz);

            CHECK_NEAR(f[0], sx, 2e-3);
            CHECK_NEAR(f[1], sy, 2e-3);
            CHECK_NEAR(f[2], sz, 2e-3);

            (side ? rollRight : rollLeft) = roll;
        }

        // The two eyes are turned by equal and opposite amounts about the same axis, so
        // whatever roll the conversion reports must be equal and opposite too. Any other
        // answer is a relative roll between the eyes, and a relative roll is precisely
        // what cannot be fused.
        CHECK_NEAR(rollLeft, -rollRight, 1e-3);
    }
}

// ---------------------------------------------------------------------------------

static void Dot3(const float a[3], const float b[3], float & out) {
    out = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void TestComposeSourceAngles() {
    check::Case("two rotations compose rigidly, which adding Euler angles does not");

    // 1. The matrix built from a set of angles is a rotation, not a reflection.
    //
    //    AngleVectors hands back `right`, and Source's right is -Y at zero yaw, so the
    //    basis (forward, right, up) has determinant -1. Composing two of those produces a
    //    mirror image that is orthonormal, plausible-looking, and wrong. The column flip
    //    inside SourceAnglesToRotation is the whole of the fix, so it is worth an
    //    assertion rather than a comment.
    {
        const float angles[][3] = {
            { 0, 0, 0 }, { 20, 50, 0 }, { -35, 170, 12 }, { 5, -95, -40 }, { 60, 33, 88 },
        };
        for (int i = 0; i < 5; i++) {
            float m[9];
            SourceAnglesToRotation(angles[i], m);

            // Orthonormal columns.
            for (int c = 0; c < 3; c++) {
                float len = m[0 * 3 + c] * m[0 * 3 + c]
                          + m[1 * 3 + c] * m[1 * 3 + c]
                          + m[2 * 3 + c] * m[2 * 3 + c];
                CHECK_NEAR(len, 1.0, 1e-5);
            }
            // Determinant +1: a rotation, with no flip left in it.
            float det =
                  m[0] * (m[4] * m[8] - m[5] * m[7])
                - m[1] * (m[3] * m[8] - m[5] * m[6])
                + m[2] * (m[3] * m[7] - m[4] * m[6]);
            CHECK_NEAR(det, 1.0, 1e-5);
        }
    }

    // 2. Angles -> matrix -> angles is the identity away from the poles.
    {
        for (int p = -80; p <= 80; p += 40) {
            for (int y = -170; y <= 170; y += 85) {
                for (int r = -60; r <= 60; r += 60) {
                    float in[3] = { (float)p, (float)y, (float)r };
                    float m[9], out[3];
                    SourceAnglesToRotation(in, m);
                    RotationToSourceAngles(m, out);
                    CHECK_NEAR(NormalizeDegrees(out[0] - in[0]), 0.0, 2e-3);
                    CHECK_NEAR(NormalizeDegrees(out[1] - in[1]), 0.0, 2e-3);
                    CHECK_NEAR(NormalizeDegrees(out[2] - in[2]), 0.0, 2e-3);
                }
            }
        }
    }

    // 3. Either side alone changes nothing.
    {
        const float base[3] = { 17.0f, -122.0f, 8.0f };
        const float zero[3] = { 0.0f, 0.0f, 0.0f };
        float out[3];

        ComposeSourceAngles(base, zero, out);
        CHECK_NEAR(NormalizeDegrees(out[0] - base[0]), 0.0, 2e-3);
        CHECK_NEAR(NormalizeDegrees(out[1] - base[1]), 0.0, 2e-3);
        CHECK_NEAR(NormalizeDegrees(out[2] - base[2]), 0.0, 2e-3);

        ComposeSourceAngles(zero, base, out);
        CHECK_NEAR(NormalizeDegrees(out[0] - base[0]), 0.0, 2e-3);
        CHECK_NEAR(NormalizeDegrees(out[1] - base[1]), 0.0, 2e-3);
        CHECK_NEAR(NormalizeDegrees(out[2] - base[2]), 0.0, 2e-3);
    }

    // 4. The property that matters in the headset. Turn the head by theta about its own
    //    up axis, on top of a base camera that is pitched and rolled: the result must be
    //    the base's own basis turned by exactly theta about the base's up. The head's up
    //    must come out unchanged - that is what "the horizon does not move when you turn"
    //    means, and it is what the compositor is told happened.
    {
        const float base[3] = { 20.0f, 50.0f, -11.0f };
        const float theta = 30.0f;
        const float head[3] = { 0.0f, theta, 0.0f };

        float fB[3], rB[3], uB[3];
        AngleVectors(base, fB, rB, uB);

        float out[3], fO[3], rO[3], uO[3];
        ComposeSourceAngles(base, head, out);
        AngleVectors(out, fO, rO, uO);

        const double d = 3.14159265358979323846 / 180.0;
        float c = (float)cos(theta * d), s = (float)sin(theta * d);

        for (int i = 0; i < 3; i++) {
            // Source yaw turns left, and left is -right.
            CHECK_NEAR(fO[i], c * fB[i] - s * rB[i], 2e-4);
            CHECK_NEAR(rO[i], c * rB[i] + s * fB[i], 2e-4);
            CHECK_NEAR(uO[i], uB[i], 2e-4);
        }

        // 5. The bug this replaces, stated as a number. Adding the Euler angles gives a
        //    different rotation: the view is turned about the WORLD's up axis instead of
        //    the head's, so the horizon tilts and the picture shears against a head that
        //    the runtime was told made a clean yaw.
        float sum[3] = { base[0] + head[0], base[1] + head[1], base[2] + head[2] };
        float fS[3], rS[3], uS[3];
        AngleVectors(sum, fS, rS, uS);

        float agreement;
        Dot3(uS, uB, agreement);
        CHECK(agreement < 0.999f);   // the up axis moved, and it should not have

        Dot3(fS, fO, agreement);
        CHECK(agreement < 0.9999f);  // and so did where the eyes are pointed

        // ... and with a level base camera the two agree exactly, which is why this went
        // unnoticed for as long as it did: on a flat demo camera addition is composition.
        const float level[3] = { 0.0f, 50.0f, 0.0f };
        float composed[3];
        ComposeSourceAngles(level, head, composed);
        CHECK_NEAR(NormalizeDegrees(composed[0] - 0.0f), 0.0, 2e-3);
        CHECK_NEAR(NormalizeDegrees(composed[1] - (level[1] + theta)), 0.0, 2e-3);
        CHECK_NEAR(NormalizeDegrees(composed[2] - 0.0f), 0.0, 2e-3);
    }

    // 6. A base of pure yaw is the mode this project ships: whatever the head does, the
    //    answer is the head's own angles with the base yaw added. Worth pinning, because
    //    it is the claim that makes the cheap path and the general path interchangeable.
    {
        const float heads[][3] = {
            { 25, 10, 0 }, { -40, -150, 7 }, { 0, 0, 33 }, { 70, 95, -20 },
        };
        for (int i = 0; i < 4; i++) {
            const float base[3] = { 0.0f, 123.0f, 0.0f };
            float out[3];
            ComposeSourceAngles(base, heads[i], out);
            CHECK_NEAR(NormalizeDegrees(out[0] - heads[i][0]), 0.0, 2e-3);
            CHECK_NEAR(NormalizeDegrees(out[1] - (heads[i][1] + base[1])), 0.0, 2e-3);
            CHECK_NEAR(NormalizeDegrees(out[2] - heads[i][2]), 0.0, 2e-3);
        }
    }

    // 7. Straight up and straight down do not produce NaN, and the matrix they build is
    //    still the right one even though the angles that describe it are not unique.
    {
        const float poles[][3] = { { 90, 40, 0 }, { -90, -75, 25 } };
        for (int i = 0; i < 2; i++) {
            float m[9], out[3], m2[9];
            SourceAnglesToRotation(poles[i], m);
            RotationToSourceAngles(m, out);
            CHECK(out[0] == out[0] && out[1] == out[1] && out[2] == out[2]);
            SourceAnglesToRotation(out, m2);
            for (int k = 0; k < 9; k++) CHECK_NEAR(m2[k], m[k], 2e-4);
        }
    }
}

// ---------------------------------------------------------------------------------

static AfxVrMath::ViewTriple MakeView(float x, float y, float z,
                                      float p, float yw, float r, float fov) {
    AfxVrMath::ViewTriple v;
    v.origin[0] = x; v.origin[1] = y; v.origin[2] = z;
    v.angles[0] = p; v.angles[1] = yw; v.angles[2] = r;
    v.fov = fov;
    return v;
}

static void TestShouldRestoreBaseView() {
    check::Case("the base camera is put back only when nobody else has written one");

    const AfxVrMath::ViewTriple written = MakeView(-99.73f, 6.63f, 52.64f, 3.0f, -120.0f, 0.0f, 108.0f);

    // Paused: the engine does not recompute the view, so the struct still holds the last
    // thing the pass loop wrote. Putting the base back is what stops the eye offset being
    // read as the game's camera and accumulating - the reason this exists at all.
    CHECK(true == AfxVrMath::ShouldRestoreBaseView(true, true, written, written));

    // Playing: the engine has written a fresh camera. Restoring over it is what froze the
    // viewer at the point the session started, for the whole session, while the world went
    // on without them. One changed component is enough to know.
    {
        AfxVrMath::ViewTriple moved = written;
        moved.origin[0] += 0.01f;
        CHECK(false == AfxVrMath::ShouldRestoreBaseView(true, true, written, moved));
    }
    {
        AfxVrMath::ViewTriple turned = written;
        turned.angles[1] += 0.01f;
        CHECK(false == AfxVrMath::ShouldRestoreBaseView(true, true, written, turned));
    }
    {
        AfxVrMath::ViewTriple zoomed = written;
        zoomed.fov += 0.5f;
        CHECK(false == AfxVrMath::ShouldRestoreBaseView(true, true, written, zoomed));
    }

    // Every component, one at a time, so a comparison that quietly skips one is caught.
    for (int i = 0; i < 3; i++) {
        AfxVrMath::ViewTriple v = written;
        v.origin[i] = written.origin[i] + 1.0f;
        CHECK(false == AfxVrMath::ShouldRestoreBaseView(true, true, written, v));

        v = written;
        v.angles[i] = written.angles[i] + 1.0f;
        CHECK(false == AfxVrMath::ShouldRestoreBaseView(true, true, written, v));
    }

    // Nothing was written this frame, so there is nothing to undo.
    CHECK(false == AfxVrMath::ShouldRestoreBaseView(false, true, written, written));

    // Nothing has ever been written - the first frame of a session. Restoring then would
    // put a base that has not been read yet over whatever the engine has just computed.
    CHECK(false == AfxVrMath::ShouldRestoreBaseView(true, false, written, written));

    // Exact equality is the point: these are floats copied verbatim, never arithmetic, so
    // the smallest representable difference means somebody else wrote it.
    {
        AfxVrMath::ViewTriple nudged = written;
        nudged.origin[2] = nextafterf(written.origin[2], 1e9f);
        CHECK(nudged.origin[2] != written.origin[2]);
        CHECK(false == AfxVrMath::ShouldRestoreBaseView(true, true, written, nudged));
    }
}

// ---------------------------------------------------------------------------------

static void TestYawThenPitchQuat() {
    check::Case("yaw then local pitch keeps a panel level, whatever direction it faces");

    for (int yawDeg = -180; yawDeg <= 180; yawDeg += 30) {
        for (int pitchDeg = -60; pitchDeg <= 60; pitchDeg += 15) {
            float yaw = (float)(yawDeg * 3.14159265358979323846 / 180.0);
            float pitch = (float)(pitchDeg * 3.14159265358979323846 / 180.0);

            float qx, qy, qz, qw;
            YawThenPitchQuat(yaw, pitch, qx, qy, qz, qw);

            CHECK_NEAR(qx * qx + qy * qy + qz * qz + qw * qw, 1.0, 1e-5);

            // The property the HUD panels need and the one a wrong sign destroys: the
            // local X axis - the panel's own horizontal - stays horizontal. Pitching about
            // the world's X instead rolls it by about sin(yaw)*pitch, which is zero at yaw
            // 0 and gross everywhere else, so a desk check facing forwards sees nothing.
            float rx, ry, rz;
            QuatRotate(qx, qy, qz, qw, 1.0f, 0.0f, 0.0f, rx, ry, rz);
            CHECK_NEAR(ry, 0.0, 2e-6);

            // And the face points back the way it was placed: for a panel put at azimuth
            // `yaw` and elevation `pitch`, its +Z must be the direction from the panel to
            // the anchor.
            float nx, ny, nz;
            QuatRotate(qx, qy, qz, qw, 0.0f, 0.0f, 1.0f, nx, ny, nz);
            CHECK_NEAR(nx, sin(yaw) * cos(pitch), 2e-6);
            CHECK_NEAR(ny, -sin(pitch), 2e-6);
            CHECK_NEAR(nz, cos(yaw) * cos(pitch), 2e-6);
        }
    }

    // The sign that was wrong, stated as a number so it cannot come back quietly. At yaw
    // 90 and pitch 30 the two orders differ; the wrong one has a local X with a vertical
    // component of half.
    {
        float yaw = (float)(90.0 * 3.14159265358979323846 / 180.0);
        float pitch = (float)(30.0 * 3.14159265358979323846 / 180.0);
        float qx, qy, qz, qw;
        YawThenPitchQuat(yaw, pitch, qx, qy, qz, qw);
        CHECK(qz < 0.0f);   // -sin(yaw/2)sin(pitch/2), not plus

        float rx, ry, rz;
        // The bug was exactly one sign, on z, and nothing else.
        QuatRotate(qx, qy, -qz, qw, 1.0f, 0.0f, 0.0f, rx, ry, rz);
        (void)rx; (void)rz;
        CHECK_NEAR(ry, 0.5, 1e-3);   // half the panel width of tilt, at this yaw and pitch
    }
}


// ---------------------------------------------------------------------------------

static void Normalise3(float v[3]) {
    float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 0.0f) { v[0] /= length; v[1] /= length; v[2] /= length; }
}

// The same quaternion PlaceRegion builds, so these point at the panels the headset
// actually shows rather than at an idealised quad.
static void RegionQuat(float azimuthDegrees, float elevationDegrees, float spread,
                       float anchorYawRadians, float q[4]) {
    const double d2r = 3.14159265358979323846 / 180.0;
    YawThenPitchQuat((float)(anchorYawRadians + azimuthDegrees * spread * d2r),
                     (float)(elevationDegrees * spread * d2r),
                     q[0], q[1], q[2], q[3]);
}

static void TestRegionPlacementRoundTrip() {
    check::Case("a panel dragged somewhere comes back as the numbers that put it there");

    const float head[3] = { 0.4f, 1.3f, -0.9f };

    // Every group in the shipped table, at both ends of the spread dial and at anchor
    // yaws all the way round. A drag has to survive all of them: the numbers written back
    // are what the NEXT frame rebuilds the pose from, so an inverse that is right only
    // near yaw zero would move the panel the instant it was let go - which is exactly how
    // the quaternion sign error hid, being zero at yaw zero too.
    const float azimuths[]   = {  0.0f,  78.0f, -40.0f, 140.0f, -179.0f };
    const float elevations[] = { 44.0f, -36.0f, -66.0f,  26.0f,    0.0f };
    const float distances[]  = {  1.2f,   1.0f,  0.55f,   2.0f,    3.5f };
    const float spreads[]    = {  1.0f,   0.8f,   1.3f };
    const float anchors[]    = {  0.0f,   1.1f,  -2.6f,   3.0f };

    for (int s = 0; s < 3; s++) {
        for (int a = 0; a < 4; a++) {
            for (int i = 0; i < 5; i++) {
                float point[3];
                RegionPlacementToPoint(head, anchors[a], spreads[s],
                                       azimuths[i], elevations[i], distances[i], point);

                float az = 0.0f, el = 0.0f, d = 0.0f;
                PointToRegionPlacement(head, anchors[a], spreads[s], point, az, el, d);

                CHECK_NEAR(d, distances[i], 1e-4);
                CHECK_NEAR(el * spreads[s], elevations[i] * spreads[s], 1e-3);
                // Azimuth is an angle: -179 and +181 are the same place.
                CHECK_NEAR(NormalizeDegrees((az - azimuths[i]) * spreads[s]), 0.0f, 1e-3);
            }
        }
    }

    const float origin[3] = { 0.0f, 0.0f, 0.0f };

    // Straight ahead at zero anchor yaw is -Z, which is OpenXR's forward.
    {
        float point[3];
        RegionPlacementToPoint(origin, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f, point);
        CHECK_NEAR(point[0],  0.0f, 1e-5);
        CHECK_NEAR(point[1],  0.0f, 1e-5);
        CHECK_NEAR(point[2], -2.0f, 1e-5);
    }

    // Positive azimuth is to the left, which in OpenXR's axes is -X.
    {
        float point[3];
        RegionPlacementToPoint(origin, 0.0f, 1.0f, 90.0f, 0.0f, 2.0f, point);
        CHECK_NEAR(point[0], -2.0f, 1e-4);
        CHECK_NEAR(point[2],  0.0f, 1e-4);
    }

    // Positive elevation is up.
    {
        float point[3];
        RegionPlacementToPoint(origin, 0.0f, 1.0f, 0.0f, 90.0f, 2.0f, point);
        CHECK_NEAR(point[1], 2.0f, 1e-4);
    }

    // Dragged into the viewer's own head: straight ahead, not whatever atan2(0,0) gives,
    // so a panel pushed into the face does not reappear behind them.
    {
        float az = 9.0f, el = 9.0f, d = 9.0f;
        PointToRegionPlacement(head, 0.5f, 1.0f, head, az, el, d);
        CHECK_NEAR(d,  0.0f, 1e-6);
        CHECK_NEAR(az, 0.0f, 1e-6);
        CHECK_NEAR(el, 0.0f, 1e-6);
    }
}

static void TestRayQuadHit() {
    check::Case("a ray finds the panel it is pointed at, and only from the front");

    const float head[3] = { 0.0f, 0.0f, 0.0f };
    float t = 0.0f, u = 0.0f, v = 0.0f;

    // The timeline as it actually hangs: below the line of sight, turned to face the
    // viewer, 55 cm away.
    const float az = 0.0f, el = -66.0f, dist = 0.55f, spread = 1.0f, anchor = 0.0f;
    float centre[3], q[4];
    RegionPlacementToPoint(head, anchor, spread, az, el, dist, centre);
    RegionQuat(az, el, spread, anchor, q);

    const float width = 0.5f, height = 0.09f;

    float right[3], up[3];
    QuatRotate(q[0], q[1], q[2], q[3], 1.0f, 0.0f, 0.0f, right[0], right[1], right[2]);
    QuatRotate(q[0], q[1], q[2], q[3], 0.0f, 1.0f, 0.0f, up[0], up[1], up[2]);

    // Pointed from the head straight at its centre.
    {
        float dir[3] = { centre[0], centre[1], centre[2] };
        Normalise3(dir);
        CHECK(RayQuadHit(head, dir, centre, q, width, height, t, u, v));
        CHECK_NEAR(t, dist, 1e-4);   // the beam is as long as the panel is far
        CHECK_NEAR(u, 0.5f, 1e-4);
        CHECK_NEAR(v, 0.5f, 1e-4);
    }

    // A quarter right and a quarter up from centre, along the quad's own axes. Local +Y
    // has to come out at v below a half, because v counts from the top the way the
    // sheet's rects are written - get this upside down and every click lands mirrored.
    {
        float target[3], dir[3];
        for (int i = 0; i < 3; i++) {
            target[i] = centre[i] + right[i] * (0.25f * width) + up[i] * (0.25f * height);
            dir[i] = target[i] - head[i];
        }
        Normalise3(dir);
        CHECK(RayQuadHit(head, dir, centre, q, width, height, t, u, v));
        CHECK_NEAR(u, 0.75f, 1e-3);
        CHECK_NEAR(v, 0.25f, 1e-3);
    }

    // Just inside each edge hits, just outside misses. The edge is where a pointer feels
    // wrong first, and it is the one thing an eye in a headset cannot judge.
    {
        const float inside[2]  = {  0.49f, -0.49f };
        const float outside[2] = {  0.51f, -0.51f };

        for (int k = 0; k < 2; k++) {
            float target[3], dir[3];

            for (int i = 0; i < 3; i++) target[i] = centre[i] + right[i] * (inside[k] * width);
            for (int i = 0; i < 3; i++) dir[i] = target[i] - head[i];
            Normalise3(dir);
            CHECK(RayQuadHit(head, dir, centre, q, width, height, t, u, v));

            for (int i = 0; i < 3; i++) target[i] = centre[i] + right[i] * (outside[k] * width);
            for (int i = 0; i < 3; i++) dir[i] = target[i] - head[i];
            Normalise3(dir);
            CHECK(!RayQuadHit(head, dir, centre, q, width, height, t, u, v));

            for (int i = 0; i < 3; i++) target[i] = centre[i] + up[i] * (inside[k] * height);
            for (int i = 0; i < 3; i++) dir[i] = target[i] - head[i];
            Normalise3(dir);
            CHECK(RayQuadHit(head, dir, centre, q, width, height, t, u, v));

            for (int i = 0; i < 3; i++) target[i] = centre[i] + up[i] * (outside[k] * height);
            for (int i = 0; i < 3; i++) dir[i] = target[i] - head[i];
            Normalise3(dir);
            CHECK(!RayQuadHit(head, dir, centre, q, width, height, t, u, v));
        }
    }

    // Pointed the other way: a miss, not a hit behind the hand.
    {
        float dir[3] = { -centre[0], -centre[1], -centre[2] };
        Normalise3(dir);
        CHECK(!RayQuadHit(head, dir, centre, q, width, height, t, u, v));
        CHECK_NEAR(t, 0.0f, 1e-6);
    }

    // From behind the panel, pointed at it. Not clickable, deliberately: the pieces are
    // spread around the viewer and a ray reaching the score strip from behind, through
    // the back of the timeline, is not what the hand meant.
    {
        float behind[3], dir[3];
        for (int i = 0; i < 3; i++) behind[i] = centre[i] + (centre[i] - head[i]);
        for (int i = 0; i < 3; i++) dir[i] = centre[i] - behind[i];
        Normalise3(dir);
        CHECK(!RayQuadHit(behind, dir, centre, q, width, height, t, u, v));
    }

    // Parallel to the plane: no hit, and no division by a vanishing number on the way.
    {
        float from[3];
        for (int i = 0; i < 3; i++) from[i] = centre[i] - right[i] - (centre[i] - head[i]) * 0.001f;
        CHECK(!RayQuadHit(from, right, centre, q, width, height, t, u, v));
    }

    // A degenerate size is a miss, not a divide by zero.
    {
        float dir[3] = { centre[0], centre[1], centre[2] };
        Normalise3(dir);
        CHECK(!RayQuadHit(head, dir, centre, q, 0.0f, height, t, u, v));
        CHECK(!RayQuadHit(head, dir, centre, q, width, 0.0f, t, u, v));
    }

    // Nearest hit wins. With two panels on one line of sight the distances have to order
    // them, and that ordering is the whole of "which panel did the hand point at".
    {
        float nearPos[3], farPos[3], qn[4], qf[4];
        RegionPlacementToPoint(head, 0.0f, 1.0f, 0.0f, 0.0f, 0.8f, nearPos);
        RegionPlacementToPoint(head, 0.0f, 1.0f, 0.0f, 0.0f, 2.4f, farPos);
        RegionQuat(0.0f, 0.0f, 1.0f, 0.0f, qn);
        RegionQuat(0.0f, 0.0f, 1.0f, 0.0f, qf);

        float dir[3] = { 0.0f, 0.0f, -1.0f };
        float tNear = 0.0f, tFar = 0.0f;
        CHECK(RayQuadHit(head, dir, nearPos, qn, 0.4f, 0.3f, tNear, u, v));
        CHECK(RayQuadHit(head, dir, farPos,  qf, 0.4f, 0.3f, tFar,  u, v));
        CHECK(tNear < tFar);
        CHECK_NEAR(tNear, 0.8f, 1e-4);
        CHECK_NEAR(tFar,  2.4f, 1e-4);
    }

    // A grab has to end where it began: hit a panel, convert the hit point back to
    // placement numbers, and the panel must be describable as sitting exactly there.
    {
        float dir[3] = { centre[0], centre[1], centre[2] };
        Normalise3(dir);
        CHECK(RayQuadHit(head, dir, centre, q, width, height, t, u, v));

        float hit[3];
        for (int i = 0; i < 3; i++) hit[i] = head[i] + dir[i] * t;

        float az2 = 0.0f, el2 = 0.0f, d2 = 0.0f;
        PointToRegionPlacement(head, anchor, spread, hit, az2, el2, d2);
        CHECK_NEAR(NormalizeDegrees(az2 - az), 0.0f, 1e-3);
        CHECK_NEAR(el2, el, 1e-3);
        CHECK_NEAR(d2, dist, 1e-4);
    }
}

// ---------------------------------------------------------------------------------

static ModeInputs Inputs(bool session, bool map, bool demo, bool cursor, bool manual) {
    ModeInputs in;
    in.sessionRunning = session;
    in.mapLoaded = map;
    in.demoPlaying = demo;
    in.cursorShowing = cursor;
    in.manualSheet = manual;
    return in;
}

static void TestDecideMode() {
    check::Case("what the headset shows is a function of five facts, and only those");

    // No session: nothing at all, whatever else is true. The hook runs with no headset
    // in the building far more often than with one.
    for (int i = 0; i < 16; i++) {
        ModeResult r = DecideMode(Inputs(false, 0 != (i & 1), 0 != (i & 2), 0 != (i & 4), 0 != (i & 8)));
        CHECK(kVrModeIdle == r.mode);
        CHECK(!r.worldInEyes);
        CHECK(!r.sheet);
        CHECK(!r.pointer);
    }

    // No map: the game's own window IS the picture, so it is opaque, and the pointer is
    // the only way to do anything at all.
    {
        ModeResult r = DecideMode(Inputs(true, false, false, false, false));
        CHECK(kVrModeMenu == r.mode);
        CHECK(!r.worldInEyes);
        CHECK(r.sheet);
        CHECK(r.sheetOpaque);
        CHECK(r.pointer);
    }

    // A demo. Fly and scrub; no sheet unless something asks for one.
    {
        ModeResult r = DecideMode(Inputs(true, true, true, false, false));
        CHECK(kVrModeWatch == r.mode);
        CHECK(r.worldInEyes);
        CHECK(!r.sheet);
        CHECK(!r.pointer);
        CHECK(!ModeTakesGameInput(r));   // a demo never takes walk and fire
    }

    // A map being played.
    {
        ModeResult r = DecideMode(Inputs(true, true, false, false, false));
        CHECK(kVrModePlay == r.mode);
        CHECK(r.worldInEyes);
        CHECK(!r.sheet);
        CHECK(ModeTakesGameInput(r));
    }

    // The cursor appearing is the signal that something wants pointing at - team select,
    // the buy menu, pause, the scoreboard. Over a live world the sheet is TRANSPARENT, so
    // the player keeps the world under the menu and stays oriented.
    {
        ModeResult r = DecideMode(Inputs(true, true, false, true, false));
        CHECK(kVrModePlay == r.mode);
        CHECK(r.worldInEyes);
        CHECK(r.sheet);
        CHECK(!r.sheetOpaque);
        CHECK(r.pointer);
        // And while it is up, the sticks must not also be walking and firing: an absolute
        // pointer and a relative aim servo on one mouse fight, and the servo wins.
        CHECK(!ModeTakesGameInput(r));
    }

    // The menu button does the same thing by hand, for anything the cursor does not cover.
    {
        ModeResult r = DecideMode(Inputs(true, true, false, false, true));
        CHECK(r.sheet);
        CHECK(!r.sheetOpaque);
        CHECK(r.pointer);
        CHECK(!ModeTakesGameInput(r));
    }

    // Same over a demo: point at the pause menu without leaving the recording.
    {
        ModeResult r = DecideMode(Inputs(true, true, true, true, false));
        CHECK(kVrModeWatch == r.mode);
        CHECK(r.worldInEyes);
        CHECK(r.sheet);
        CHECK(!r.sheetOpaque);
        CHECK(r.pointer);
    }

    // With no map the sheet is opaque whatever the cursor or the button say - there is
    // nothing behind it to keep.
    for (int i = 0; i < 4; i++) {
        ModeResult r = DecideMode(Inputs(true, false, false, 0 != (i & 1), 0 != (i & 2)));
        CHECK(kVrModeMenu == r.mode);
        CHECK(r.sheetOpaque);
        CHECK(!r.worldInEyes);
    }

    // The demo flag never changes what is shown, only who the controls belong to. Worth
    // pinning: "is there a map" and "is it a recording" are two questions, and answering
    // the first with the second is the bug that kept VR out of a game against bots.
    for (int i = 0; i < 4; i++) {
        bool cursor = 0 != (i & 1), manual = 0 != (i & 2);
        ModeResult watch = DecideMode(Inputs(true, true, true, cursor, manual));
        ModeResult play  = DecideMode(Inputs(true, true, false, cursor, manual));
        CHECK(watch.worldInEyes == play.worldInEyes);
        CHECK(watch.sheet == play.sheet);
        CHECK(watch.sheetOpaque == play.sheetOpaque);
        CHECK(watch.pointer == play.pointer);
        CHECK(watch.mode != play.mode);
    }
}

// ---------------------------------------------------------------------------------

static void TestBodyTurnStep() {
    check::Case("the world holds still while the aim crosses a thirty degree cone");

    const float cone = 30.0f, snap = 30.0f;

    // Inside the cone nothing moves. This is the point of the whole thing: the crosshair
    // walks across a still picture.
    for (float rel = -29.9f; rel <= 29.9f; rel += 1.0f) {
        CHECK_NEAR(BodyTurnStep(rel, cone, snap), 0.0f, 1e-4);
    }
    CHECK_NEAR(BodyTurnStep(0.0f, cone, snap), 0.0f, 1e-6);
    CHECK_NEAR(BodyTurnStep(30.0f, cone, snap), 0.0f, 1e-4);
    CHECK_NEAR(BodyTurnStep(-30.0f, cone, snap), 0.0f, 1e-4);

    // Past it, one snap, in the direction the aim went.
    CHECK_NEAR(BodyTurnStep(31.0f, cone, snap), 30.0f, 1e-4);
    CHECK_NEAR(BodyTurnStep(-31.0f, cone, snap), -30.0f, 1e-4);

    // And the aim ends up back inside the cone, which is the property that matters: one
    // step must not leave it still outside, or the next frame steps again and the world
    // spins.
    for (float rel = 30.5f; rel < 179.0f; rel += 0.5f) {
        float step = BodyTurnStep(rel, cone, snap);
        CHECK(fabsf(NormalizeDegrees(rel - step)) <= cone + 1e-3f);
        step = BodyTurnStep(-rel, cone, snap);
        CHECK(fabsf(NormalizeDegrees(-rel - step)) <= cone + 1e-3f);
    }

    // Smooth following: exactly the overshoot, so the aim lands on the edge.
    CHECK_NEAR(BodyTurnStep(45.0f, cone, 0.0f), 15.0f, 1e-4);
    CHECK_NEAR(BodyTurnStep(-45.0f, cone, 0.0f), -15.0f, 1e-4);
    CHECK_NEAR(NormalizeDegrees(45.0f - BodyTurnStep(45.0f, cone, 0.0f)), 30.0f, 1e-3);

    // Wrapping: 190 degrees to the left is 170 to the right, and the body must turn the
    // short way. Turning the long way round is a full spin for a small correction.
    CHECK(BodyTurnStep(190.0f, cone, snap) < 0.0f);
    CHECK(BodyTurnStep(-190.0f, cone, snap) > 0.0f);

    // No cone at all is still legal: every movement turns the body.
    CHECK_NEAR(BodyTurnStep(5.0f, 0.0f, 0.0f), 5.0f, 1e-4);
    CHECK_NEAR(BodyTurnStep(-5.0f, 0.0f, 0.0f), -5.0f, 1e-4);
    // And a negative cone is read as none rather than as an error.
    CHECK_NEAR(BodyTurnStep(5.0f, -10.0f, 0.0f), 5.0f, 1e-4);
}

static void TestTakeWholeUnits() {
    check::Case("a fraction of a mouse count is kept rather than thrown away");

    // The bug this exists for: truncating every frame loses up to a whole count each
    // time, which is a steady drift, and anything below one count produces nothing at all.
    {
        float carry = 0.0f;
        int total = 0;
        for (int i = 0; i < 100; i++) total += TakeWholeUnits(0.4f, carry);
        CHECK(total >= 39 && total <= 40);   // 40 asked for; truncation would give 0
    }

    // Small amounts eventually move, rather than never moving.
    {
        float carry = 0.0f;
        int moved = 0;
        for (int i = 0; i < 20; i++) if (TakeWholeUnits(0.1f, carry)) moved++;
        CHECK(moved >= 1);
    }

    // The carry never grows past one unit, in either direction.
    {
        float carry = 0.0f;
        for (int i = 0; i < 500; i++) {
            TakeWholeUnits((i % 7) * 0.3f - 0.9f, carry);
            CHECK(fabsf(carry) < 1.0f);
        }
    }

    // Symmetric: the same journey out and back nets to nothing, so holding a stick one way
    // and then the other leaves the aim where it started.
    {
        float carry = 0.0f;
        int total = 0;
        for (int i = 0; i < 50; i++) total += TakeWholeUnits(0.37f, carry);
        for (int i = 0; i < 50; i++) total += TakeWholeUnits(-0.37f, carry);
        CHECK(total >= -1 && total <= 1);
    }

    // Whole numbers pass straight through with nothing left over.
    {
        float carry = 0.0f;
        CHECK(3 == TakeWholeUnits(3.0f, carry));
        CHECK_NEAR(carry, 0.0f, 1e-5);
        CHECK(-4 == TakeWholeUnits(-4.0f, carry));
        CHECK_NEAR(carry, 0.0f, 1e-5);
    }
}

// ---------------------------------------------------------------------------------

static void TestStickToGameFrame() {
    check::Case("pushing the stick forward walks where you are looking, not where you aim");

    float f = 0.0f, l = 0.0f;

    // Head and aim agree: straight through.
    StickToGameFrame(0.0f, 1.0f, 0.0f, f, l);
    CHECK_NEAR(f, 1.0f, 1e-4);
    CHECK_NEAR(l, 0.0f, 1e-4);

    StickToGameFrame(1.0f, 0.0f, 0.0f, f, l);
    CHECK_NEAR(f, 0.0f, 1e-4);
    CHECK_NEAR(l, -1.0f, 1e-4);   // right on the stick is negative left

    // The case that was broken: looking behind the body, forward on the stick has to walk
    // where the eyes are, which in the game's frame is backwards.
    StickToGameFrame(0.0f, 1.0f, 180.0f, f, l);
    CHECK_NEAR(f, -1.0f, 1e-3);
    CHECK_NEAR(l, 0.0f, 1e-3);

    // Looking ninety degrees left: forward on the stick is the game's left.
    StickToGameFrame(0.0f, 1.0f, 90.0f, f, l);
    CHECK_NEAR(f, 0.0f, 1e-4);
    CHECK_NEAR(l, 1.0f, 1e-4);

    // Looking ninety degrees right: forward on the stick is the game's right.
    StickToGameFrame(0.0f, 1.0f, -90.0f, f, l);
    CHECK_NEAR(f, 0.0f, 1e-4);
    CHECK_NEAR(l, -1.0f, 1e-4);

    // Magnitude is preserved at every angle, so a half push stays a half push and the
    // press thresholds keep meaning what they say.
    for (int deg = -180; deg <= 180; deg += 15) {
        for (int i = 0; i < 8; i++) {
            float x = 0.3f * (float)((i % 3) - 1);
            float y = 0.7f * (float)((i / 3) - 1);
            StickToGameFrame(x, y, (float)deg, f, l);
            double before = sqrt((double)x * x + (double)y * y);
            double after = sqrt((double)f * f + (double)l * l);
            CHECK_NEAR(after, before, 1e-4);
        }
    }

    // Two rotations compose: turning the head by a then by b is turning it by a+b.
    {
        float f1, l1, f2, l2;
        StickToGameFrame(0.4f, 0.9f, 25.0f, f1, l1);
        // Feed the result back in as a stick reading, remembering right is -left.
        StickToGameFrame(-l1, f1, 35.0f, f2, l2);
        StickToGameFrame(0.4f, 0.9f, 60.0f, f, l);
        CHECK_NEAR(f2, f, 1e-3);
        CHECK_NEAR(l2, l, 1e-3);
    }

    // A centred stick stays centred whatever the head is doing - no drift into a wall.
    for (int deg = -180; deg <= 180; deg += 30) {
        StickToGameFrame(0.0f, 0.0f, (float)deg, f, l);
        CHECK_NEAR(f, 0.0f, 1e-6);
        CHECK_NEAR(l, 0.0f, 1e-6);
    }
}
// ---------------------------------------------------------------------------------

static void TestRoomOffsetToWorld() {
    check::Case("a step in the room is a step in the map, in the direction you face");

    float out[3];

    // Facing along the map's +X. OpenXR is x right, y up, z back, so a metre forward is
    // dz = -1.
    RoomOffsetToWorld(0.0f, 0.0f, 0.0f, -1.0f, out);
    CHECK_NEAR(out[0], 39.3700787, 1e-3);
    CHECK_NEAR(out[1], 0.0, 1e-3);
    CHECK_NEAR(out[2], 0.0, 1e-3);

    // A metre to the right. Source's Y is LEFT, so right is negative Y.
    RoomOffsetToWorld(0.0f, 1.0f, 0.0f, 0.0f, out);
    CHECK_NEAR(out[0], 0.0, 1e-3);
    CHECK_NEAR(out[1], -39.3700787, 1e-3);
    CHECK_NEAR(out[2], 0.0, 1e-3);

    // Standing up out of a crouch.
    RoomOffsetToWorld(0.0f, 0.0f, 1.0f, 0.0f, out);
    CHECK_NEAR(out[0], 0.0, 1e-3);
    CHECK_NEAR(out[1], 0.0, 1e-3);
    CHECK_NEAR(out[2], 39.3700787, 1e-3);

    // Turned ninety degrees left: forward in the room is now the map's +Y.
    RoomOffsetToWorld(90.0f, 0.0f, 0.0f, -1.0f, out);
    CHECK_NEAR(out[0], 0.0, 1e-3);
    CHECK_NEAR(out[1], 39.3700787, 1e-3);
    CHECK_NEAR(out[2], 0.0, 1e-3);

    // ... and right in the room is then the map's +X.
    RoomOffsetToWorld(90.0f, 1.0f, 0.0f, 0.0f, out);
    CHECK_NEAR(out[0], 39.3700787, 1e-3);
    CHECK_NEAR(out[1], 0.0, 1e-3);
    CHECK_NEAR(out[2], 0.0, 1e-3);

    // Height never turns with the yaw, and the horizontal length never changes with it:
    // the yaw rotates the step, it does not stretch it.
    for (int yawDeg = -180; yawDeg <= 180; yawDeg += 45) {
        RoomOffsetToWorld((float)yawDeg, 0.37f, 0.11f, -0.62f, out);
        double horizontal = sqrt((double)out[0] * out[0] + (double)out[1] * out[1]);
        double expected = sqrt(0.37 * 0.37 + 0.62 * 0.62) * 39.3700787;
        CHECK_NEAR(horizontal, expected, 1e-2);
        CHECK_NEAR(out[2], 0.11 * 39.3700787, 1e-3);
    }

    // The property snap turning depends on: turning must pivot about the viewer. The
    // compensation is the difference between the offset before and after the turn, so
    // whatever it is, applying it has to put the head back where it was.
    {
        const float o[3] = { 0.8f, 0.2f, -0.35f };   // a metre or so of lean, in the room
        float before[3], after[3];
        RoomOffsetToWorld(20.0f, o[0], o[1], o[2], before);
        RoomOffsetToWorld(50.0f, o[0], o[1], o[2], after);

        // Without compensation the viewer is thrown this far sideways by a 30 degree snap.
        double thrown = sqrt(
            (double)(after[0] - before[0]) * (after[0] - before[0]) +
            (double)(after[1] - before[1]) * (after[1] - before[1]));
        CHECK(thrown > 10.0);   // over 25 cm of map, per press

        // With it, nothing moves.
        for (int i = 0; i < 3; i++) {
            float compensated = after[i] + (before[i] - after[i]);
            CHECK_NEAR(compensated, before[i], 1e-4);
        }
    }
}

// ---------------------------------------------------------------------------------

// A game, as far as a mouse is concerned: counts go in, and some frames later the angle
// has turned by a gain the sender was never told. Read() is the angle at the top of a
// frame; Send() is what the frame then sends. With lag 1 a send is visible at the next
// Read, with lag 2 the one after.
struct MousePlant {
    float angle = 0.0f;
    float gain = -0.0275f;      // m_yaw 0.022 x sensitivity 1.25, and mouse right turns yaw down
    int lag = 2;
    bool wraps = true;
    int pipe[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

    float Read() const { return angle; }

    void Send(int counts) {
        for (int i = 7; i > 0; i--) pipe[i] = pipe[i - 1];
        pipe[0] = counts;
        // Everything at least (lag - 1) frames old lands now. "At least", so that a lag
        // that changes from frame to frame neither loses a send nor applies one twice.
        for (int i = lag - 1; i < 8; i++) {
            if (i < 0) continue;
            angle += gain * (float)pipe[i];
            pipe[i] = 0;
        }
        if (wraps) angle = AfxVrMath::NormalizeDegrees(angle);
    }

    void Turn(float degrees) {   // somebody else's doing: a hand on the mouse, a respawn
        angle += degrees;
        if (wraps) angle = AfxVrMath::NormalizeDegrees(angle);
    }
};

// Aim the way a thumb does: a burst one way, a rest, a burst the other way.
static void AimInBursts(MousePlant & plant, AfxVrMath::CountGainEstimator & estimator,
                        int bursts, int framesPerBurst, int countsPerFrame, int restFrames) {
    for (int b = 0; b < bursts; b++) {
        int direction = (b % 2) ? -1 : 1;
        for (int f = 0; f < framesPerBurst; f++) {
            float read = plant.Read();
            int counts = direction * countsPerFrame;
            estimator.Feed(counts, read);
            plant.Send(counts);
        }
        for (int f = 0; f < restFrames; f++) {
            estimator.Feed(0, plant.Read());
            plant.Send(0);
        }
    }
}

static void TestCountGainEstimator() {
    using namespace AfxVrMath;
    check::Case("what a mouse count is worth is measured from ordinary aiming");

    // Nothing measured, nothing claimed - except a seed, if one was given.
    {
        CountGainEstimator e;
        CHECK(!e.Converged());
        CHECK_NEAR(e.DegreesPerCount(), 0.0, 1e-9);
        e.Seed(-0.03f);
        CHECK(!e.Converged());
        CHECK_NEAR(e.DegreesPerCount(), -0.03, 1e-6);
    }

    // The plain case, for each delay the game might have. The sign comes out too: mouse
    // right turns a Source yaw down.
    for (int lag = 1; lag <= 3; lag++) {
        MousePlant plant; plant.lag = lag;
        CountGainEstimator e;
        AimInBursts(plant, e, 6, 15, 40, 6);
        CHECK(e.Converged());
        CHECK_NEAR(e.DegreesPerCount(), plant.gain, 0.02 * 0.0275);
    }

    // One short flick is not a measurement.
    {
        MousePlant plant;
        CountGainEstimator e;
        AimInBursts(plant, e, 1, 3, 5, 6);
        CHECK(!e.Converged());
    }

    // The delay is not constant - the render thread is sometimes a frame behind and
    // sometimes not. Frame-against-frame comparison falls apart here; blocks do not care.
    {
        MousePlant plant;
        CountGainEstimator e;
        for (int b = 0; b < 8; b++) {
            int direction = (b % 2) ? -1 : 1;
            for (int f = 0; f < 14; f++) {
                plant.lag = 1 + ((f * 7 + b) % 2);
                float read = plant.Read();
                e.Feed(direction * 35, read);
                plant.Send(direction * 35);
            }
            plant.lag = 2;
            for (int f = 0; f < 6; f++) { e.Feed(0, plant.Read()); plant.Send(0); }
        }
        CHECK(e.Converged());
        CHECK_NEAR(e.DegreesPerCount(), plant.gain, 0.03 * 0.0275);
    }

    // A long sweep with no rest in it, across the +-180 seam several times. Blocks are cut
    // while the stick is still moving; what is in flight at each cut goes to the next one.
    {
        MousePlant plant; plant.angle = 170.0f;
        CountGainEstimator e;
        for (int f = 0; f < 400; f++) {
            float read = plant.Read();
            e.Feed(-60, read);        // mouse left: yaw climbs through +180 and wraps
            plant.Send(-60);
        }
        CHECK(e.Converged());
        CHECK_NEAR(e.DegreesPerCount(), plant.gain, 0.03 * 0.0275);
    }

    // Pitch: no wrapping, and the other sign - mouse down is pitch up in Source's numbers.
    {
        MousePlant plant; plant.wraps = false; plant.gain = 0.0275f;
        CountGainEstimator e; e.wraps = false;
        AimInBursts(plant, e, 6, 10, 20, 6);
        CHECK(e.Converged());
        CHECK_NEAR(e.DegreesPerCount(), 0.0275, 0.02 * 0.0275);
    }

    // A respawn in the middle of a burst turns the player a hundred degrees for reasons
    // that have nothing to do with the mouse. That block is thrown away, not averaged in.
    {
        MousePlant plant;
        CountGainEstimator e;
        AimInBursts(plant, e, 6, 15, 40, 6);
        float before = e.DegreesPerCount();

        for (int f = 0; f < 15; f++) {
            if (7 == f) plant.Turn(100.0f);
            float read = plant.Read();
            e.Feed(40, read);
            plant.Send(40);
        }
        for (int f = 0; f < 6; f++) { e.Feed(0, plant.Read()); plant.Send(0); }

        CHECK_NEAR(e.DegreesPerCount(), before, 0.02 * 0.0275);
    }

    // A hand on the real mouse during one burst: a few degrees nobody here sent. One
    // disagreeing block is ignored.
    {
        MousePlant plant;
        CountGainEstimator e;
        AimInBursts(plant, e, 6, 15, 40, 6);
        float before = e.DegreesPerCount();

        for (int f = 0; f < 15; f++) {
            plant.Turn(1.0f);         // fifteen degrees over the burst, against 16.5 of ours
            float read = plant.Read();
            e.Feed(40, read);
            plant.Send(40);
        }
        for (int f = 0; f < 6; f++) { e.Feed(0, plant.Read()); plant.Send(0); }

        CHECK_NEAR(e.DegreesPerCount(), before, 1e-7);
    }

    // The scope goes up and a count is suddenly worth under half what it was. Blocks keep
    // disagreeing, so the old number is dropped and the new one learned.
    {
        MousePlant plant;
        CountGainEstimator e;
        AimInBursts(plant, e, 6, 15, 40, 6);
        CHECK_NEAR(e.DegreesPerCount(), -0.0275, 0.02 * 0.0275);

        plant.gain = -0.0275f * 0.4444f;
        AimInBursts(plant, e, 10, 15, 40, 6);
        CHECK(e.Converged());
        CHECK_NEAR(e.DegreesPerCount(), plant.gain, 0.03 * 0.0275 * 0.4444);
    }
}

// Run an AimServo against a plant until it stops. Reports the worst overshoot, as a
// fraction of the error it started with, and how many counts it sent in all.
struct ServoRun {
    float finalError;
    float worstOvershoot;
    int frames;
    long totalCounts;
    AfxVrMath::AimServo::Outcome outcome;
};

static ServoRun RunServo(MousePlant & plant, float target, float believedGain,
                         AfxVrMath::AimServo & servo) {
    using namespace AfxVrMath;
    ServoRun run = { 0.0f, 0.0f, 0, 0, AimServo::kIdle };

    float startError = NormalizeDegrees(target - plant.Read());
    float startSign = startError < 0.0f ? -1.0f : 1.0f;

    servo.Start();
    // Keep the frames going a little after it stops, so whatever was still on its way
    // when it declared itself finished is counted against it.
    int after = 0;
    for (int f = 0; f < 60 && after < 6; f++) {
        float error = NormalizeDegrees(target - plant.Read());
        int counts = servo.Step(error, believedGain);
        plant.Send(counts);
        run.totalCounts += counts < 0 ? -counts : counts;
        if (servo.Running()) run.frames++; else after++;

        float past = -error * startSign;    // positive once it has gone beyond the target
        float fraction = past / (startError * startSign + 1e-6f);
        if (fraction > run.worstOvershoot) run.worstOvershoot = fraction;
    }
    run.finalError = NormalizeDegrees(target - plant.Read());
    run.outcome = servo.outcome;
    return run;
}

static void TestAimServo() {
    using namespace AfxVrMath;
    check::Case("the aim comes to where you are looking, and stops there");

    // Never started: it sends nothing, whatever it is shown.
    {
        AimServo servo;
        CHECK(0 == servo.Step(40.0f, -0.0275f));
        CHECK(!servo.Running());
    }

    // No idea what a count is worth: refuse, rather than send a number made of nothing.
    {
        AimServo servo;
        servo.Start();
        CHECK(0 == servo.Step(40.0f, 0.0f));
        CHECK(AimServo::kRefused == servo.outcome);
    }

    // Already there.
    {
        AimServo servo;
        servo.Start();
        CHECK(0 == servo.Step(0.1f, -0.0275f));
        CHECK(AimServo::kReached == servo.outcome);
    }

    // The gain known exactly, for every delay, both directions, and across the seam.
    for (int lag = 1; lag <= 3; lag++) {
        const float starts[3]  = { 0.0f, 0.0f, 170.0f };
        const float targets[3] = { 40.0f, -75.0f, -160.0f };
        for (int i = 0; i < 3; i++) {
            MousePlant plant; plant.lag = lag; plant.angle = starts[i];
            AimServo servo;
            ServoRun run = RunServo(plant, targets[i], plant.gain, servo);
            CHECK(AimServo::kReached == run.outcome);
            CHECK(fabs(run.finalError) <= 0.5);
            CHECK(run.worstOvershoot <= 0.02f);
            CHECK(run.frames <= 16);
        }
    }

    // The gain believed a quarter too high and a quarter too low - which is about as wrong
    // as a half-converged estimate gets. It must still arrive, and must not swing wildly.
    for (int lag = 1; lag <= 3; lag++) {
        const float wrong[2] = { 1.25f, 0.75f };
        for (int i = 0; i < 2; i++) {
            MousePlant plant; plant.lag = lag;
            AimServo servo;
            ServoRun run = RunServo(plant, 40.0f, plant.gain * wrong[i], servo);
            CHECK(AimServo::kReached == run.outcome);
            CHECK(fabs(run.finalError) <= 0.5);
            CHECK(run.worstOvershoot <= 0.25f);
        }
    }

    // The delay changes under it from frame to frame, as it does when the render thread
    // is sometimes a frame behind. It measured one delay at the start and the game then
    // uses another; it still has to arrive without swinging.
    for (int phase = 0; phase < 2; phase++) {
        MousePlant plant;
        AimServo servo;
        servo.Start();
        float worstPast = 0.0f;
        for (int f = 0; f < 40; f++) {
            plant.lag = 1 + ((f + phase) % 2);
            float error = NormalizeDegrees(40.0f - plant.Read());
            plant.Send(servo.Step(error, plant.gain));
            if (-error > worstPast) worstPast = -error;
        }
        CHECK(AimServo::kReached == servo.outcome);
        CHECK(fabs(NormalizeDegrees(40.0f - plant.Read())) <= 0.5);
        CHECK(worstPast <= 0.25f * 40.0f);
    }

    // Pitch, where a count is worth a positive number.
    {
        MousePlant plant; plant.wraps = false; plant.gain = 0.0275f;
        AimServo servo;
        ServoRun run = RunServo(plant, -30.0f, plant.gain, servo);
        CHECK(AimServo::kReached == run.outcome);
        CHECK(fabs(run.finalError) <= 0.5);
    }

    // A game that is not listening - paused, or the window lost the keyboard and mouse.
    // It notices within a few frames, and it has sent ONE correction's worth of counts
    // while finding out, not a fortune of them: whatever was sent lands all at once the
    // moment the game wakes up.
    {
        MousePlant plant; plant.gain = 0.0f;
        AimServo servo;
        ServoRun run = RunServo(plant, 40.0f, -0.0275f, servo);
        CHECK(AimServo::kGaveUp == run.outcome);
        long needed = (long)(40.0f / 0.0275f);
        CHECK(run.totalCounts <= needed);
        CHECK(run.frames <= 8);
    }

    // A gain so coarse that one count is more than the tolerance: it settles for the
    // nearest count rather than hunting for ever.
    {
        MousePlant plant; plant.gain = -0.5f;
        AimServo servo;
        ServoRun run = RunServo(plant, 10.2f, plant.gain, servo);
        CHECK(AimServo::kReached == run.outcome);
        CHECK(fabs(run.finalError) <= 0.5);
    }

    // Cancelled halfway: nothing more is sent.
    {
        MousePlant plant;
        AimServo servo;
        servo.Start();
        plant.Send(servo.Step(40.0f, plant.gain));
        servo.Cancel();
        CHECK(!servo.Running());
        CHECK(0 == servo.Step(30.0f, plant.gain));
    }
}

// ---------------------------------------------------------------------------------

// A tiny deterministic wobble, standing in for a hand that is never quite still.
static float Tremor(int frame, float amplitude) {
    static const float pattern[8] = { 0.9f, -0.4f, 0.2f, -1.0f, 0.6f, -0.1f, 0.8f, -0.7f };
    return amplitude * pattern[frame & 7];
}

static void TestAimTracker() {
    using namespace AfxVrMath;
    check::Case("the aim follows the hand, trails it by a bounded amount, and does not ring");

    // Nothing is known about what a count is worth: send nothing.
    {
        AimTracker t;
        CHECK(0 == t.Step(30.0f, 0.0f, 0.0f));
    }

    // A steady sweep, then a dead stop - the two things a hand does. While sweeping the
    // aim trails by no more than the delay accounts for; after the stop it arrives, and
    // it never goes more than a little past where the hand stopped.
    for (int lag = 1; lag <= 3; lag++) {
        MousePlant plant; plant.lag = lag;
        AimTracker t; t.lagFrames = lag;

        const float perFrame = 1.5f;     // fifty-four degrees a second at 36 frames
        float hand = 0.0f;
        float worstTrail = 0.0f;
        for (int f = 0; f < 60; f++) {
            hand += perFrame;
            plant.Send(t.Step(hand, plant.Read(), plant.gain));
            if (f >= 20) {
                float trail = NormalizeDegrees(hand - plant.Read());
                if (trail > worstTrail) worstTrail = trail;
                CHECK(trail > 0.0f);     // behind the hand, never ahead of a steady sweep
            }
        }
        float bound = perFrame * ((float)(lag - 1) + 1.0f / t.kp + 1.0f) + 0.5f;
        CHECK(worstTrail <= bound);

        float worstPast = 0.0f;
        for (int f = 0; f < 30; f++) {
            plant.Send(t.Step(hand, plant.Read(), plant.gain));
            float past = NormalizeDegrees(plant.Read() - hand);
            if (past > worstPast) worstPast = past;
        }
        CHECK(fabs(NormalizeDegrees(hand - plant.Read())) <= 0.2);
        CHECK(worstPast <= 0.3f);
    }

    // The delay believed one frame too short or too long, and the gain a quarter out
    // either way. After the hand stops it must settle, not keep swinging.
    {
        const int trueLag[4]     = { 3, 2, 2, 1 };
        const int believedLag[4] = { 2, 1, 3, 2 };
        const float wrongGain[2] = { 1.25f, 0.75f };
        for (int i = 0; i < 4; i++) for (int g = 0; g < 2; g++) {
            MousePlant plant; plant.lag = trueLag[i];
            AimTracker t; t.lagFrames = believedLag[i];
            float believed = plant.gain * wrongGain[g];

            float hand = 0.0f;
            for (int f = 0; f < 40; f++) { hand += 1.5f; plant.Send(t.Step(hand, plant.Read(), believed)); }

            float worstPast = 0.0f;
            for (int f = 0; f < 40; f++) {
                plant.Send(t.Step(hand, plant.Read(), believed));
                float past = NormalizeDegrees(plant.Read() - hand);
                if (past > worstPast) worstPast = past;
            }
            CHECK(fabs(NormalizeDegrees(hand - plant.Read())) <= 0.3);
            CHECK(worstPast <= 2.0f);    // a swing past, but a small one that dies
        }
    }

    // A snap turn: the target jumps thirty degrees in one frame. The smoothing must not
    // slow that down, and the aim gets there in a handful of frames.
    {
        MousePlant plant;
        AimTracker t;
        for (int f = 0; f < 10; f++) plant.Send(t.Step(0.0f, plant.Read(), plant.gain));
        int arrived = -1;
        for (int f = 0; f < 30; f++) {
            plant.Send(t.Step(30.0f, plant.Read(), plant.gain));
            if (arrived < 0 && fabs(NormalizeDegrees(30.0f - plant.Read())) <= 0.5) arrived = f;
        }
        CHECK(arrived >= 0 && arrived <= 12);
    }

    // A hand held "still". The crosshair must move less than the hand shakes, and the
    // mouse must not be sent a stream of corrections for nothing.
    {
        MousePlant plant;
        AimTracker t;
        const float amplitude = 0.25f;
        for (int f = 0; f < 20; f++) plant.Send(t.Step(10.0f, plant.Read(), plant.gain));

        float low = 1e9f, high = -1e9f;
        for (int f = 0; f < 80; f++) {
            float hand = 10.0f + Tremor(f, amplitude);
            plant.Send(t.Step(hand, plant.Read(), plant.gain));
            float a = plant.Read();
            if (a < low) low = a;
            if (a > high) high = a;
        }
        CHECK((high - low) <= amplitude);             // under half of the 2 x amplitude the hand swings through
        CHECK(fabs(0.5f * (high + low) - 10.0f) <= 0.15); // and centred on where it is held
    }

    // Across the +-180 seam, both ways, without ever going the long way round.
    {
        MousePlant plant; plant.angle = 175.0f;
        AimTracker t;
        float hand = 175.0f;
        float worst = 0.0f;
        for (int f = 0; f < 40; f++) {
            hand = NormalizeDegrees(hand + 1.0f);      // 175 -> -145, through the seam
            plant.Send(t.Step(hand, plant.Read(), plant.gain));
            float trail = (float)fabs(NormalizeDegrees(hand - plant.Read()));
            if (trail > worst) worst = trail;
        }
        CHECK(worst <= 6.0f);
    }

    // Pitch: no wrap, positive gain.
    {
        MousePlant plant; plant.wraps = false; plant.gain = 0.0275f;
        AimTracker t; t.wraps = false;
        for (int f = 0; f < 40; f++) plant.Send(t.Step(-25.0f, plant.Read(), plant.gain));
        CHECK(fabs(-25.0f - plant.Read()) <= 0.2);
    }

    // Reset forgets the old target, so re-entering the mode does not start by chasing
    // where the hand was last time.
    {
        AimTracker t;
        t.Step(50.0f, 0.0f, -0.0275f);
        t.Reset();
        CHECK(!t.haveTarget);
        t.Step(-20.0f, -20.0f, -0.0275f);
        CHECK_NEAR(t.SmoothedTarget(), -20.0, 1e-5);
    }
}

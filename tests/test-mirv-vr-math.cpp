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

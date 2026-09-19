#pragma once

// cs2-vr-spectator: everything that is a function of its arguments and nothing else.
//
// The rest of this module can only run inside CS2 with a headset attached, which makes it
// expensive to be wrong about. What is here instead has no engine, no Windows, no OpenXR
// and no global state, so tests/ compiles it on its own and checks it in a second. Keep
// that property: anything that needs the engine belongs in MirvVr.cpp or MirvVrXr.cpp.

#include <math.h>
#include <stddef.h>
#include <string.h>

namespace AfxVrMath {

// ---------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------

// Source's AngleVectors, for angles in degrees as (pitch, yaw, roll). Any of the three
// outputs may be null.
//
// This is the engine's convention, not a general one: yaw turns left, pitch is inverted
// (positive pitch looks down), and `right` points to the camera's right which is -Y at
// zero yaw. Getting this wrong separates the eyes along a world axis instead of across
// the gaze, which looks almost right and is completely wrong.
inline void AngleVectors(const float angles[3], float forward[3], float right[3], float up[3]) {
    const double d = 3.14159265358979323846 / 180.0;
    double sp = sin(angles[0] * d), cp = cos(angles[0] * d);
    double sy = sin(angles[1] * d), cy = cos(angles[1] * d);
    double sr = sin(angles[2] * d), cr = cos(angles[2] * d);

    if (forward) {
        forward[0] = (float)(cp * cy);
        forward[1] = (float)(cp * sy);
        forward[2] = (float)(-sp);
    }
    if (right) {
        right[0] = (float)(-sr * sp * cy + cr * sy);
        right[1] = (float)(-sr * sp * sy - cr * cy);
        right[2] = (float)(-sr * cp);
    }
    if (up) {
        up[0] = (float)(cr * sp * cy + sr * sy);
        up[1] = (float)(cr * sp * sy - sr * cy);
        up[2] = (float)(cr * cp);
    }
}

// Horizontal movement in the plane the viewer faces. Deliberately flat: tilting the head
// down and pushing the stick forward should not drive the viewer into the floor.
inline void MoveInViewPlane(float viewYawDegrees, float right, float forward, float up,
                            float outDelta[3]) {
    const double d = 3.14159265358979323846 / 180.0;
    double sy = sin(viewYawDegrees * d), cy = cos(viewYawDegrees * d);

    outDelta[0] = (float)(forward * cy + right * sy);
    outDelta[1] = (float)(forward * sy - right * cy);
    outDelta[2] = up;
}

// An OpenXR orientation quaternion as Source's (pitch, yaw, roll) in degrees.
//
// Source and OpenXR do not agree on axes: Source's world is X forward, Y left, Z up, with
// positive pitch looking down; OpenXR's is X right, Y up, Z back, with positive pitch
// looking up. Yaw agrees, the other two are inverted.
//
// This was written from memory and never checked, which mattered the moment the two eyes
// stopped sharing an orientation: a conversion that is wrong by a consistent amount
// cancels between identical eyes and does not cancel between different ones. It shows up
// as one horizontal line in the world appearing at two different angles.
//
// tests/ checks it the only way that does not just re-state the same assumption: convert
// to angles, run Source's own AngleVectors, and compare against rotating the basis vectors
// by the quaternion directly.
inline void QuatToSourceAngles(float qx, float qy, float qz, float qw,
                               float & pitchDegrees, float & yawDegrees, float & rollDegrees) {
    double sinPitch = 2.0 * ((double)qw * qx - (double)qy * qz);
    if (sinPitch > 1.0) sinPitch = 1.0;
    if (sinPitch < -1.0) sinPitch = -1.0;

    double pitch = asin(sinPitch);
    double yaw   = atan2(2.0 * ((double)qw * qy + (double)qz * qx),
                         1.0 - 2.0 * ((double)qx * qx + (double)qy * qy));
    double roll  = atan2(2.0 * ((double)qw * qz + (double)qx * qy),
                         1.0 - 2.0 * ((double)qx * qx + (double)qz * qz));

    const double r2d = 180.0 / 3.14159265358979323846;
    pitchDegrees = (float)(-pitch * r2d);
    yawDegrees   = (float)( yaw   * r2d);
    rollDegrees  = (float)(-roll  * r2d);
}

// Rotate a vector by a quaternion.
inline void QuatRotate(float qx, float qy, float qz, float qw,
                       float x, float y, float z,
                       float & ox, float & oy, float & oz) {
    float tx = 2.0f * (qy * z - qz * y);
    float ty = 2.0f * (qz * x - qx * z);
    float tz = 2.0f * (qx * y - qy * x);
    ox = x + qw * tx + (qy * tz - qz * ty);
    oy = y + qw * ty + (qz * tx - qx * tz);
    oz = z + qw * tz + (qx * ty - qy * tx);
}

// An OpenXR-frame direction in Source's world axes. Source: X forward, Y left, Z up.
// OpenXR: X right, Y up, Z back. So forward is -Z, left is -X, up is +Y.
inline void XrDirectionToSource(float x, float y, float z,
                                float & sx, float & sy, float & sz) {
    sx = -z;
    sy = -x;
    sz =  y;
}

// A rotation as the basis it takes the world to, written column by column: forward, left,
// up. Source's AngleVectors hands back `right`, which is -Y at zero yaw, so the matrix
// built straight out of it has determinant -1 and is a reflection rather than a rotation.
// Flipping that one column is the whole difference, and getting it wrong composes two
// rotations into a mirror image that still looks almost plausible.
//
// Stored row-major: m[row * 3 + col].
inline void SourceAnglesToRotation(const float angles[3], float m[9]) {
    float f[3], r[3], u[3];
    AngleVectors(angles, f, r, u);
    for (int i = 0; i < 3; i++) {
        m[i * 3 + 0] =  f[i];
        m[i * 3 + 1] = -r[i];
        m[i * 3 + 2] =  u[i];
    }
}

// The inverse. Straight out of the same definitions: forward is (cp cy, cp sy, -sp), and
// left and up differ in their third component only by sin and cos of the roll.
inline void RotationToSourceAngles(const float m[9], float angles[3]) {
    const double r2d = 180.0 / 3.14159265358979323846;

    double fx = m[0], fy = m[3], fz = m[6];   // column 0: forward
    double lz = m[7];                          // column 1, row 2: left
    double uz = m[8];                          // column 2, row 2: up

    double sp = -fz;
    if (sp >  1.0) sp =  1.0;
    if (sp < -1.0) sp = -1.0;
    angles[0] = (float)(asin(sp) * r2d);

    double cp = sqrt(fx * fx + fy * fy);
    if (cp > 1e-6) {
        angles[1] = (float)(atan2(fy, fx) * r2d);
        angles[2] = (float)(atan2(lz, uz) * r2d);
    } else {
        // Straight up or straight down: yaw and roll turn about the same axis and only
        // their difference is defined. Give the whole of it to yaw, which is the one the
        // rest of this module reads back.
        angles[1] = (float)(atan2(-(double)m[1], (double)m[4]) * r2d);
        angles[2] = 0.0f;
    }
}

// Two rotations, applied in order: first `base`, then `head` in the frame base leaves
// behind. The result is the angles that describe the combination.
//
// This exists because adding Euler angles is not composition, and the difference is
// invisible until it is not. `base + head` equals `base then head` only while the base
// camera's pitch and roll are zero. Spectating a player who is looking 20 degrees down,
// a head yaw of theta is then rendered as a rotation about the WORLD's up axis with the
// view pitched - horizontal flow of theta*cos(20) plus a twist of theta*sin(20) - while
// the pose handed to the compositor says it was a clean yaw about the head's own up. The
// runtime reprojects by the motion it was told about, and the picture shears against the
// head for the whole length of the turn.
inline void ComposeSourceAngles(const float base[3], const float head[3], float out[3]) {
    float b[9], h[9], m[9];
    SourceAnglesToRotation(base, b);
    SourceAnglesToRotation(head, h);
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            m[row * 3 + col] = b[row * 3 + 0] * h[0 * 3 + col]
                             + b[row * 3 + 1] * h[1 * 3 + col]
                             + b[row * 3 + 2] * h[2 * 3 + col];
        }
    }
    RotationToSourceAngles(m, out);
}

// A yaw about the world's up axis, then a pitch about the result's OWN right axis.
//
// The order is the whole content of this function. Yaw-then-local-pitch keeps the thing
// level: its local X axis stays horizontal whatever the two angles are. Pitching about the
// WORLD's X instead - which is what the other order gives, and what a hand-expanded
// quaternion product gives if one sign is wrong - rolls it by about sin(yaw)*pitch. That
// is zero at yaw 0, so it survives every desk check, and it tilted the HUD panels by
// thirty-five degrees the moment the viewer faced a different direction.
//
// Right-handed, y up, as OpenXR uses.
inline void YawThenPitchQuat(float yawRadians, float pitchRadians,
                             float & x, float & y, float & z, float & w) {
    double sy = sin(yawRadians * 0.5), cy = cos(yawRadians * 0.5);
    double sp = sin(pitchRadians * 0.5), cp = cos(pitchRadians * 0.5);
    // (0, sy, 0, cy) * (sp, 0, 0, cp)
    w = (float)( cy * cp);
    x = (float)( cy * sp);
    y = (float)( sy * cp);
    z = (float)(-sy * sp);
}

// One inch per Source unit, which is what makes a metre of room 39.37 units of map.
const float kUnitsPerMetre = 39.3700787f;

// Where a step, a lean or a crouch in the room lands in the map.
//
// Only the head's ORIENTATION used to reach the game; its position was thrown away, so the
// world was glued to the face and leaning did nothing. Which is also a lie told to the
// compositor, since the projection layer reports the real eye poses: it was being told
// about a translation that had not been rendered.
//
// The offset arrives in OpenXR's axes and metres - x right, y up, z back - and Source's
// world is x forward, y LEFT, z up. So the viewer's own frame is (-dz, -dx, +dy) in units,
// and that is then turned by the yaw that maps "forward in the room" onto the map. That
// yaw is the one the view angles use MINUS the head's own yaw, and it has to be taken from
// the same place, or leaning and turning end up disagreeing about where forward is.
inline void RoomOffsetToWorld(float yawDegrees, float dx, float dy, float dz, float out[3]) {
    const double d2r = 3.14159265358979323846 / 180.0;
    double forward = -(double)dz * kUnitsPerMetre;
    double left    = -(double)dx * kUnitsPerMetre;
    double up      =  (double)dy * kUnitsPerMetre;

    double sy = sin(yawDegrees * d2r), cy = cos(yawDegrees * d2r);

    out[0] = (float)(forward * cy - left * sy);
    out[1] = (float)(forward * sy + left * cy);
    out[2] = (float)up;
}

// Fold an angle into (-180, 180].
inline float NormalizeDegrees(float degrees) {
    while (degrees > 180.0f) degrees -= 360.0f;
    while (degrees <= -180.0f) degrees += 360.0f;
    return degrees;
}

// ---------------------------------------------------------------------------------
// Whose camera is in the view struct?
// ---------------------------------------------------------------------------------

// The eye poses are written into a persistent object between render passes, and the
// engine reads that same object again at the top of the next frame - so whatever the last
// pass left there would be read back as the game's own camera and accumulated. The cure
// was to put the base camera back before that read.
//
// Which is right only while the engine is not computing a camera of its own. On a PAUSED
// demo it does not, the struct still holds our last write, and restoring is exactly
// correct. On a PLAYING demo it writes a fresh camera every frame - and the restore threw
// it away, so the base froze at whatever it was on the first frame an eye was enabled and
// never moved again. The viewer stayed at the point where the session started while the
// world went on without them, and every "next player" landed back in the same place.
//
// That went unnoticed for a long time because experiments 03 to 07 were all run paused,
// where the two behaviours are indistinguishable, and because free look takes the
// orientation from the head, which hides everything except the position.
//
// The test is simply whether anyone else has touched it: if the struct still holds,
// exactly, the last thing this module wrote, the engine has not been here.
struct ViewTriple {
    float origin[3];
    float angles[3];
    float fov;
};

inline bool SameView(const ViewTriple & a, const ViewTriple & b) {
    for (int i = 0; i < 3; i++) {
        if (a.origin[i] != b.origin[i]) return false;
        if (a.angles[i] != b.angles[i]) return false;
    }
    return a.fov == b.fov;
}

// Exact equality, deliberately. These are floats copied verbatim, never arithmetic
// results, so anything other than bit equality means a different value was written.
inline bool ShouldRestoreBaseView(bool dirty, bool haveLastWritten,
                                  const ViewTriple & lastWritten, const ViewTriple & structNow) {
    if (!dirty) return false;
    if (!haveLastWritten) return false;
    return SameView(lastWritten, structNow);
}

// ---------------------------------------------------------------------------------
// Stick shaping
// ---------------------------------------------------------------------------------

// Deadzone, rescaled so the usable part of the throw still reaches 1.0. Without the
// rescale the stick has a dead patch and then jumps.
inline float ApplyDeadzone(float value, float deadzone) {
    if (deadzone < 0.0f) deadzone = 0.0f;
    if (deadzone > 0.99f) deadzone = 0.99f;
    if (value > deadzone)  return (value - deadzone) / (1.0f - deadzone);
    if (value < -deadzone) return (value + deadzone) / (1.0f - deadzone);
    return 0.0f;
}

// An exponent on the shaped stick value, keeping the sign. 1.0 is linear; above that,
// small deflections move less, which is what makes fine positioning possible without
// giving up the top speed.
inline float ApplyResponseCurve(float value, float exponent) {
    if (exponent <= 0.0f) return value;
    float magnitude = value < 0.0f ? -value : value;
    if (magnitude > 1.0f) magnitude = 1.0f;
    float shaped = (float)pow((double)magnitude, (double)exponent);
    return value < 0.0f ? -shaped : shaped;
}

// Snap turning: the view jumps by a fixed step instead of sweeping. Smooth rotation that
// the body did not ask for is the main cause of sickness in VR, and a jump gives the
// inner ear nothing to disagree with.
//
// `state` is the caller's latch: the stick has to return inside the release threshold
// before it will fire again, so holding it over does not spin the viewer.
struct SnapTurnState {
    bool armed = true;
};

inline float SnapTurn(SnapTurnState & state, float stickX, float stepDegrees,
                      float fireThreshold = 0.7f, float releaseThreshold = 0.4f) {
    float magnitude = stickX < 0.0f ? -stickX : stickX;

    if (!state.armed) {
        if (magnitude < releaseThreshold) state.armed = true;
        return 0.0f;
    }
    if (magnitude < fireThreshold) return 0.0f;

    state.armed = false;
    // Push right, turn right. Source yaw increases to the left, hence the sign.
    return stickX > 0.0f ? -stepDegrees : stepDegrees;
}

// ---------------------------------------------------------------------------------
// Does this still look like a camera?
// ---------------------------------------------------------------------------------

// The whole mechanism rests on writing floats at fixed byte offsets into an object whose
// layout belongs to whatever CS2 build is installed. When those offsets move, nothing
// fails to load and nothing prints a warning -- the hook just writes into whatever now
// lives there. This is the check that turns that into a refusal.
//
// It is a plausibility test, not a proof. Its job is to catch the offsets pointing
// somewhere else entirely, which is overwhelmingly the failure mode: arbitrary memory
// read as a float is almost never a field of view between 1 and 179 degrees.
struct ViewCheck {
    bool ok = true;
    const char * why = nullptr; // set when ok is false; a static string, safe to print
};

inline bool IsFinitef(float v) {
    // Deliberately not std::isfinite: this header is compiled by whatever the advancedfx
    // tree uses, and <cmath> in that tree has been known to disagree with itself.
    return !(v != v) && v < 3.0e38f && v > -3.0e38f;
}

// Source maps live inside +-16384 units on each axis; double that is generous and still
// nowhere near what arbitrary memory produces.
const float kMaxWorldCoordinate = 32768.0f;

inline ViewCheck CheckView(const float origin[3], const float angles[3], float fov) {
    ViewCheck r;

    for (int i = 0; i < 3; i++) {
        if (!IsFinitef(origin[i])) { r.ok = false; r.why = "view origin is not a finite number"; return r; }
        if (origin[i] > kMaxWorldCoordinate || origin[i] < -kMaxWorldCoordinate) {
            r.ok = false; r.why = "view origin is outside any possible map"; return r;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (!IsFinitef(angles[i])) { r.ok = false; r.why = "view angles are not finite numbers"; return r; }
        if (angles[i] > 720.0f || angles[i] < -720.0f) {
            r.ok = false; r.why = "view angles are not angles"; return r;
        }
    }
    if (!IsFinitef(fov)) { r.ok = false; r.why = "field of view is not a finite number"; return r; }
    if (fov < 1.0f || fov > 179.0f) { r.ok = false; r.why = "field of view is outside 1..179 degrees"; return r; }

    return r;
}

// ---------------------------------------------------------------------------------
// steam.inf
// ---------------------------------------------------------------------------------

// steam.inf is KEY=VALUE lines. Returns false when the key is absent, the buffer is too
// small, or the text is null. Matching is exact and case sensitive, which is what the
// file uses.
inline bool SteamInfValue(const char * text, const char * key, char * out, size_t outSize) {
    if (!text || !key || !out || 0 == outSize) return false;
    out[0] = '\0';

    size_t keyLength = strlen(key);
    if (0 == keyLength) return false;

    const char * p = text;
    while (*p) {
        const char * lineStart = p;
        const char * lineEnd = p;
        while (*lineEnd && '\n' != *lineEnd) lineEnd++;

        // Advance now; everything below works on [lineStart, lineEnd).
        p = *lineEnd ? lineEnd + 1 : lineEnd;

        if ((size_t)(lineEnd - lineStart) > keyLength
            && 0 == strncmp(lineStart, key, keyLength)
            && '=' == lineStart[keyLength]) {

            const char * valueStart = lineStart + keyLength + 1;
            const char * valueEnd = lineEnd;
            // Trailing CR, because the file is written on Windows and may be read as text
            // or as bytes depending on who opened it.
            while (valueEnd > valueStart && ('\r' == valueEnd[-1] || ' ' == valueEnd[-1])) valueEnd--;

            size_t length = (size_t)(valueEnd - valueStart);
            if (length + 1 > outSize) return false;
            memcpy(out, valueStart, length);
            out[length] = '\0';
            return true;
        }
    }
    return false;
}

// Where something is, relative to a file whose path is known: strips the file name, then
// `extraLevels` more directories, then appends `suffix`. Accepts either separator and
// leaves the part it keeps exactly as it was given.
//
// A DLL has no business knowing an absolute path on the machine it was built on. Until
// this existed the OpenXR loader was looked for at a literal
// "D:\Dev\cs2-vr-tools\openxr\..." - which is one developer's disk, and the first thing
// that would have to be explained to anyone who unpacked a release. What the hook actually
// wants to ask is "what is next to me", and the answer is here so it can be tested without
// a game, a headset or that disk.
//
// A template because the caller for steam.inf reads a narrow path out of the engine and
// the caller for the loader must pass a wide one to LoadLibraryW: a path with a non-ASCII
// character in it - somebody's name in a folder name - is not hypothetical, and narrowing
// it is how that turns into "could not load openxr_loader.dll" with no reason given.
template <class C>
inline bool PathRelativeToFile(const C * path, int extraLevels, const C * suffix,
                               C * out, size_t outSize) {
    if (!path || !suffix || !out || 0 == outSize) return false;
    out[0] = (C)0;
    if (extraLevels < 0) return false;

    size_t length = 0;
    while (path[length]) length++;

    int toStrip = extraLevels + 1;   // the file name first, then whole directories
    while (toStrip > 0 && length > 0) {
        while (length > 0 && (C)'\\' != path[length - 1] && (C)'/' != path[length - 1]) length--;
        if (0 == length) return false;
        length--;   // the separator itself
        toStrip--;
    }
    if (toStrip > 0 || 0 == length) return false;

    size_t suffixLength = 0;
    while (suffix[suffixLength]) suffixLength++;
    if (length + suffixLength + 1 > outSize) return false;

    for (size_t i = 0; i < length; i++) out[i] = path[i];
    for (size_t i = 0; i <= suffixLength; i++) out[length + i] = suffix[i];
    return true;
}

// cs2.exe lives at <install>/game/bin/win64/cs2.exe and steam.inf at
// <install>/game/csgo/steam.inf, so the answer is three directories up and back down.
inline bool SteamInfPathFromExe(const char * exePath, char * out, size_t outSize) {
    return PathRelativeToFile(exePath, 2, "\\csgo\\steam.inf", out, outSize);
}

} // namespace AfxVrMath

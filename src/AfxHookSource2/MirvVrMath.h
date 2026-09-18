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

// Fold an angle into (-180, 180].
inline float NormalizeDegrees(float degrees) {
    while (degrees > 180.0f) degrees -= 360.0f;
    while (degrees <= -180.0f) degrees += 360.0f;
    return degrees;
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

// cs2.exe lives at <install>/game/bin/win64/cs2.exe and steam.inf at
// <install>/game/csgo/steam.inf, so the answer is three directories up and back down.
// Accepts either separator and always writes backslashes.
inline bool SteamInfPathFromExe(const char * exePath, char * out, size_t outSize) {
    if (!exePath || !out || 0 == outSize) return false;
    out[0] = '\0';

    size_t length = strlen(exePath);
    // Strip the file name and then two more directories: win64, bin.
    int toStrip = 3;
    while (toStrip > 0 && length > 0) {
        while (length > 0 && '\\' != exePath[length - 1] && '/' != exePath[length - 1]) length--;
        if (0 == length) return false;
        length--; // the separator itself
        toStrip--;
    }
    if (0 == length) return false;

    const char * suffix = "\\csgo\\steam.inf";
    size_t suffixLength = strlen(suffix);
    if (length + suffixLength + 1 > outSize) return false;

    memcpy(out, exePath, length);
    memcpy(out + length, suffix, suffixLength + 1);
    return true;
}

} // namespace AfxVrMath

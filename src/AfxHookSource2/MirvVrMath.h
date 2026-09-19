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


// ---------------------------------------------------------------------------------
// Pointing at a panel
// ---------------------------------------------------------------------------------

// Where a piece of the HUD hangs, as a position.
//
// The panels are not stored as poses. They are stored as azimuth, elevation, distance and
// width in a room-fixed yaw frame measured from the HEAD, which is what lets them keep
// their arrangement when the viewer stands up, sits down or walks across the room. This is
// the half of PlaceRegion that turns those numbers into a point.
//
// `spread` is the one dial that moves every group out of the middle of the view at once;
// it multiplies both angles, so it has to appear in the inverse as well or a dragged panel
// lands somewhere else the moment it is let go.
inline void RegionPlacementToPoint(const float head[3], float anchorYawRadians, float spread,
                                   float azimuthDegrees, float elevationDegrees,
                                   float distanceMetres, float outPoint[3]) {
    const double d2r = 3.14159265358979323846 / 180.0;

    double yaw = (double)anchorYawRadians + (double)azimuthDegrees * spread * d2r;
    double el  = (double)elevationDegrees * spread * d2r;
    double cosEl = cos(el), sinEl = sin(el);

    // OpenXR looks down -Z, so a yaw of zero is straight ahead at (0, 0, -distance).
    outPoint[0] = head[0] + (float)(-sin(yaw) * cosEl * distanceMetres);
    outPoint[1] = head[1] + (float)( sinEl            * distanceMetres);
    outPoint[2] = head[2] + (float)(-cos(yaw) * cosEl * distanceMetres);
}

// And back again: the numbers that would put a piece at `point`.
//
// This is what makes a grabbed panel movable. A drag that ended in a pose would be thrown
// away on the next frame, because the next frame rebuilds every pose from these four
// numbers.
inline void PointToRegionPlacement(const float head[3], float anchorYawRadians, float spread,
                                   const float point[3],
                                   float & outAzimuthDegrees, float & outElevationDegrees,
                                   float & outDistanceMetres) {
    const double r2d = 180.0 / 3.14159265358979323846;

    double dx = (double)point[0] - head[0];
    double dy = (double)point[1] - head[1];
    double dz = (double)point[2] - head[2];

    double distance = sqrt(dx * dx + dy * dy + dz * dz);
    outDistanceMetres = (float)distance;

    // On top of the viewer's head there is no direction to report. Say straight ahead
    // rather than whatever atan2(0, 0) gives, so a panel dragged into the face does not
    // reappear behind the viewer.
    if (distance < 1e-6 || !(spread > 0.0f)) {
        outAzimuthDegrees = 0.0f;
        outElevationDegrees = 0.0f;
        return;
    }

    double sinEl = dy / distance;
    if (sinEl >  1.0) sinEl =  1.0;
    if (sinEl < -1.0) sinEl = -1.0;
    outElevationDegrees = (float)(asin(sinEl) * r2d / spread);

    double yaw = atan2(-dx, -dz);
    outAzimuthDegrees = (float)(NormalizeDegrees((float)((yaw - (double)anchorYawRadians) * r2d))
                                / spread);
}

// Where a ray meets a quad layer, in the quad's own coordinates.
//
// An OpenXR quad occupies the XY plane of its pose, centred on the pose's position, and
// shows only the face whose normal is its local +Z. That is the side the viewer is on,
// because every piece is turned to face where the viewer stands - so a hit that counts is
// a FRONT hit, and the back of a panel is not clickable. Deliberately: the pieces are
// spread around the viewer and a ray reaching the score strip from behind, through the
// back of the timeline, is not what the hand was pointing at.
//
// Misses, the back face, and a ray parallel to the plane all return false.
//
// On a hit, `outDistance` is how far along the ray the hit is - in metres only if
// `rayDirection` is a unit vector, which is also what makes "nearest hit wins" mean what
// it says when several panels overlap. (outU, outV) are texture coordinates: u from the
// left edge, v from the TOP, matching the way the sheet's rects are written.
inline bool RayQuadHit(const float rayOrigin[3], const float rayDirection[3],
                       const float quadPosition[3], const float quadOrientation[4],
                       float width, float height,
                       float & outDistance, float & outU, float & outV) {
    outDistance = 0.0f;
    outU = 0.0f;
    outV = 0.0f;
    if (!(width > 0.0f) || !(height > 0.0f)) return false;

    // Into the quad's frame: translate, then rotate by the conjugate, which is the inverse
    // for the unit quaternions a pose carries.
    float qx = -quadOrientation[0], qy = -quadOrientation[1], qz = -quadOrientation[2];
    float qw =  quadOrientation[3];

    float ox, oy, oz;
    QuatRotate(qx, qy, qz, qw,
               rayOrigin[0] - quadPosition[0],
               rayOrigin[1] - quadPosition[1],
               rayOrigin[2] - quadPosition[2],
               ox, oy, oz);

    float dx, dy, dz;
    QuatRotate(qx, qy, qz, qw,
               rayDirection[0], rayDirection[1], rayDirection[2],
               dx, dy, dz);

    // Parallel to the plane, or travelling away from it.
    if (!(dz < -1e-6f)) return false;
    // Starting behind it: that is the back face.
    if (!(oz > 0.0f)) return false;

    float t = -oz / dz;
    if (!(t > 0.0f)) return false;

    float hx = ox + t * dx;
    float hy = oy + t * dy;
    if (hx < -0.5f * width  || hx > 0.5f * width)  return false;
    if (hy < -0.5f * height || hy > 0.5f * height) return false;

    outDistance = t;
    outU = hx / width + 0.5f;
    outV = 0.5f - hy / height;
    return true;
}


// ---------------------------------------------------------------------------------
// What the headset is showing, and therefore what the controllers do
// ---------------------------------------------------------------------------------

enum VrMode {
    kVrModeIdle  = 0,   // no session; nothing is being shown
    kVrModeMenu  = 1,   // no map: the game's own window, on a screen
    kVrModeWatch = 2,   // a demo: fly the camera, scrub, change who you watch
    kVrModePlay  = 3    // a map being played: walk, aim, fire
};

struct ModeInputs {
    bool sessionRunning;
    bool mapLoaded;
    bool demoPlaying;
    // Panorama shows the system cursor exactly when it wants something pointed at: the
    // main menu, team select, the buy menu, pause, settings, the scoreboard, the console.
    // It hides it when the game owns the mouse. One cheap call, no offsets to go stale,
    // and it is self-evidently the thing we mean by "there is something to click".
    bool cursorShowing;
    bool manualSheet;   // the menu button, held over whatever is loaded
};

struct ModeResult {
    int mode;
    bool worldInEyes;   // render the world into both eyes
    bool sheet;         // put the whole window on a quad
    bool sheetOpaque;   // and nothing behind it
    bool pointer;       // the controller ray, and the mouse it drives
};

// Deliberately a function of its arguments and nothing else, with tests, because this
// project has now had three faults in one day that were all the same fault: a mode decided
// from the wrong signal, silently. The demo tick that is available at the main menu, the
// level name that reads "<empty>" as a map, and a frame begun in one mode and ended in
// another.
inline ModeResult DecideMode(const ModeInputs & in) {
    ModeResult out;
    out.mode = kVrModeIdle;
    out.worldInEyes = false;
    out.sheet = false;
    out.sheetOpaque = false;
    out.pointer = false;

    if (!in.sessionRunning) return out;

    if (!in.mapLoaded) {
        // Nothing behind the screen to see, so it is the picture rather than an overlay.
        out.mode = kVrModeMenu;
        out.sheet = true;
        out.sheetOpaque = true;
        out.pointer = true;
        return out;
    }

    out.mode = in.demoPlaying ? kVrModeWatch : kVrModePlay;
    out.worldInEyes = true;

    // Over a live world the sheet is transparent: the main pass is already cleared before
    // the UI, so the team picker or the buy menu floats over the world instead of
    // replacing it. Keeping the world visible under a menu is both nicer and the only way
    // the player stays oriented.
    if (in.cursorShowing || in.manualSheet) {
        out.sheet = true;
        out.sheetOpaque = false;
        out.pointer = true;
    }
    return out;
}

// While the pointer is driving the mouse, the game must not also be driven by sticks and
// triggers: an absolute pointer and a relative aim servo on one device fight, and the
// servo wins. This is the one place that decides it.
inline bool ModeTakesGameInput(const ModeResult & mode) {
    return (kVrModePlay == mode.mode) && !mode.pointer;
}


// ---------------------------------------------------------------------------------
// Aiming with a stick while the head owns the picture
// ---------------------------------------------------------------------------------

// How far to turn the body, given how far the aim has wandered from where the body faces.
//
// This is the whole of what stops a stick-aimed headset making people ill, and it is not
// an idea this project invented: it is what Quakespasm-OpenVR calls decoupled aiming, and
// what Cyberpunk's VR port calls decoupled pitch. Inside a cone - thirty degrees by
// default - the aim moves and the world does NOT. The crosshair walks across a still
// picture, which costs nothing to watch. Only when the aim reaches the edge of the cone
// does the body follow, and then it follows in a snap by default, because a world that
// rotates smoothly under a head that is not turning is exactly the motion nobody's
// stomach forgives.
//
// `relativeDegrees` is the aim's yaw measured from where the BODY faces - not from where
// the head is looking. Measured from the gaze, glancing over your shoulder would push the
// aim out of the cone and spin the world for no reason at all.
//
// Returns the degrees to add to the body's yaw: zero inside the cone, and outside it
// either one snap step or exactly enough to bring the aim back to the edge.
inline float BodyTurnStep(float relativeDegrees, float deadzoneDegrees,
                          float snapDegrees) {
    float rel = NormalizeDegrees(relativeDegrees);
    if (deadzoneDegrees < 0.0f) deadzoneDegrees = 0.0f;

    float over = (rel > 0.0f) ? (rel - deadzoneDegrees) : (rel + deadzoneDegrees);
    if ((rel > 0.0f && over <= 0.0f) || (rel < 0.0f && over >= 0.0f) || 0.0f == rel) {
        return 0.0f;
    }

    if (snapDegrees <= 0.0f) return over;   // smooth: follow by exactly the overshoot

    // A snap, in whole steps, so the aim ends up back inside the cone however far it has
    // been pushed. One press of the stick against the edge is one step; holding it there
    // steps again when it reaches the edge again.
    float sign = (over > 0.0f) ? 1.0f : -1.0f;
    float magnitude = (over > 0.0f) ? over : -over;
    int steps = 1 + (int)(magnitude / snapDegrees);
    return sign * snapDegrees * (float)steps;
}

// Turn a wanted amount into whole units, keeping what is left over for next time.
//
// A mouse takes integers. Truncating a per-frame amount throws away up to one count every
// frame on every axis, which at this frame rate is a steady drift in one direction, and it
// means small stick deflections produce exactly nothing - a dead band on top of the
// stick's own, in the range where aiming actually happens.
inline int TakeWholeUnits(float wanted, float & carry) {
    float total = wanted + carry;
    // Toward zero, so the carry never changes sign and the remainder stays below one unit.
    int whole = (int)total;
    carry = total - (float)whole;
    return whole;
}


// Where a push on the stick should take you, expressed the way the game understands it.
//
// The game walks relative to ITS own yaw - the direction the player is aiming - while the
// picture is the game's yaw plus wherever the head is turned. So pressing forward walks
// along the aim, not along the gaze, and with the head turned round the player walks
// backwards. Reported from inside the headset in exactly those words: "I look forward and
// the legs go back, the controls are broken".
//
// So rotate the stick out of the frame the eyes are in and into the frame the keys are in.
// `deltaDegrees` is how far the view is turned from the game's own yaw, positive to the
// left, which is Source's sense for yaw.
//
// Outputs are along the game's forward and the game's left, which is what W/S and A/D
// press. Magnitude is preserved, so a half-pushed stick stays a half-pushed stick and the
// press thresholds keep meaning what they say.
inline void StickToGameFrame(float stickRight, float stickForward, float deltaDegrees,
                             float & outForward, float & outLeft) {
    const double d2r = 3.14159265358979323846 / 180.0;

    // The stick in (forward, left). Right on the stick is negative left.
    double forward = stickForward;
    double left = -stickRight;

    double d = deltaDegrees * d2r;
    double c = cos(d), s = sin(d);

    // Rotate by delta about the up axis: the same world direction, named in the game's
    // frame instead of the view's.
    outForward = (float)(forward * c - left * s);
    outLeft    = (float)(forward * s + left * c);
}

// ---------------------------------------------------------------------------------
// What a mouse count is worth, and spending that knowledge
// ---------------------------------------------------------------------------------

// Learns how many degrees the game turns per mouse count, by watching.
//
// The aim is moved with synthetic mouse counts, and a count is not a degree: it is
// m_yaw x sensitivity, scaled again while a scope is up. Both can be read or guessed, and
// both are somebody else's setting that changes without telling us. But every frame we
// know how many counts we sent and we can read the angle the game ended up with, so the
// number can simply be measured, continuously, from ordinary aiming.
//
// Two things make the obvious version - this frame's turn over this frame's counts -
// wrong. The counts sent in one frame show up in the angle a frame or two later, and not
// always the same number of frames later. So nothing here is compared frame against
// frame. Counts and degrees are summed over a BLOCK of aiming that ends only once the
// stick has been still long enough for everything sent to have arrived; within a block it
// does not matter when each count landed. A block that has to be cut short while the
// stick is still moving leaves its last few frames of counts for the next block, which is
// where their degrees will be.
//
// Blocks are pooled as a regression through the origin, recent ones weighted more. A block
// that disagrees badly with the estimate is thrown away - a hand on the real mouse, a
// respawn - unless several in a row disagree, which is what a scope going up looks like,
// and then the estimate starts again from them.
//
// One of these per axis. The result is signed: in Source, mouse right turns the yaw DOWN,
// so the yaw's degrees per count is negative. Callers divide by it and never care.
//
// Feed() wants, every frame, the counts sent THIS frame and the angle as read this frame
// BEFORE those counts were sent.
struct CountGainEstimator {
    // How it behaves. Defaults are for ~36 frames a second.
    int   idleFramesToClose = 4;      // stick still this long: everything has landed
    int   maxBlockFrames = 24;        // cut a long sweep into pieces of this many frames
    int   assumedLagFrames = 2;       // how many trailing frames a cut block leaves behind
    float minBlockCounts = 40.0f;     // less than this is noise, not a measurement
    float jumpDegrees = 45.0f;        // a turn this big in one frame is a teleport
    float forget = 0.85f;             // weight of everything older, per accepted block
    bool  wraps = true;               // yaw wraps at +-180, pitch does not

    // What it knows.
    double sumCountsDegrees = 0.0;
    double sumCountsSquared = 0.0;
    int accepted = 0;
    int rejectedInARow = 0;
    float seed = 0.0f;                // used until something has been measured

    // The block being gathered.
    bool blockOpen = false;
    double blockCounts = 0.0;
    double blockDegrees = 0.0;
    int blockFrames = 0;
    int idleFrames = 0;
    int recent[4] = { 0, 0, 0, 0 };   // counts sent in the last four frames, newest first

    float previous = 0.0f;
    bool havePrevious = false;

    void Seed(float degreesPerCount) { seed = degreesPerCount; }

    // Signed degrees per count. The seed until a measurement exists, zero if neither does.
    float DegreesPerCount() const {
        if (accepted > 0 && sumCountsSquared > 0.0) return (float)(sumCountsDegrees / sumCountsSquared);
        return seed;
    }

    // Enough agreeing blocks that acting on the number is reasonable.
    bool Converged() const { return accepted >= 3 && sumCountsSquared > 0.0; }

    void Forget() {
        sumCountsDegrees = sumCountsSquared = 0.0;
        accepted = 0;
        rejectedInARow = 0;
    }

    void DropBlock() {
        blockOpen = false;
        blockCounts = blockDegrees = 0.0;
        blockFrames = idleFrames = 0;
    }

    void CloseBlock(double counts, double degrees) {
        double magnitude = counts < 0.0 ? -counts : counts;
        if (magnitude < (double)minBlockCounts) return;

        if (accepted >= 2) {
            double predicted = (sumCountsDegrees / sumCountsSquared) * counts;
            double miss = degrees - predicted;
            if (miss < 0.0) miss = -miss;
            double allowed = (predicted < 0.0 ? -predicted : predicted) * 0.30;
            if (allowed < 1.0) allowed = 1.0;
            if (miss > allowed) {
                // Once is somebody's hand on the real mouse. Three times running is the
                // number itself having changed, and the old one is then worth nothing.
                if (++rejectedInARow >= 3) {
                    Forget();
                } else {
                    return;
                }
            }
        }

        sumCountsDegrees = (double)forget * sumCountsDegrees + counts * degrees;
        sumCountsSquared = (double)forget * sumCountsSquared + counts * counts;
        accepted++;
        rejectedInARow = 0;
    }

    void Feed(int countsSentThisFrame, float angleReadThisFrame) {
        float delta = 0.0f;
        if (havePrevious) {
            delta = angleReadThisFrame - previous;
            if (wraps) delta = NormalizeDegrees(delta);
        }
        previous = angleReadThisFrame;
        havePrevious = true;

        float size = delta < 0.0f ? -delta : delta;
        if (size > jumpDegrees) {
            // A respawn, a round restart, a teleport. Whatever was being measured is
            // spoiled, and so is anything still on its way.
            DropBlock();
            recent[0] = recent[1] = recent[2] = recent[3] = 0;
            // Fall through: counts sent this frame still start a fresh block below.
            delta = 0.0f;
        }

        if (0 != countsSentThisFrame) {
            if (!blockOpen) { DropBlock(); blockOpen = true; }
            idleFrames = 0;
        } else if (blockOpen) {
            idleFrames++;
        }

        recent[3] = recent[2]; recent[2] = recent[1]; recent[1] = recent[0];
        recent[0] = countsSentThisFrame;

        if (!blockOpen) return;

        blockCounts += countsSentThisFrame;
        blockDegrees += delta;
        blockFrames++;

        if (idleFrames >= idleFramesToClose) {
            // Still for long enough: all of it has arrived.
            CloseBlock(blockCounts, blockDegrees);
            DropBlock();
        } else if (blockFrames >= maxBlockFrames) {
            // Still moving. The last few frames of counts have not shown up yet, so they
            // are not this block's; they open the next one, where their degrees will be.
            int lag = assumedLagFrames;
            if (lag < 0) lag = 0;
            if (lag > 4) lag = 4;
            double pending = 0.0;
            for (int i = 0; i < lag; i++) pending += recent[i];

            CloseBlock(blockCounts - pending, blockDegrees);
            DropBlock();
            blockOpen = true;
            blockCounts = pending;
        }
    }
};


// Brings the aim onto a direction, in a few frames, with mouse counts.
//
// "Shoot at what I am looking at" for someone aiming with a stick: look, click, and the
// crosshair comes to the gaze. One of these per axis, run together.
//
// It is a closed loop over a delay. A count sent now changes the angle a frame or two
// from now, so a loop that sends "the error, in counts" every frame sends it two or three
// times over and swings past. What has been sent and not yet seen has to be subtracted,
// and for that the delay has to be known - so the first thing this does is MEASURE it:
// send one correction, then send nothing and count frames until the angle moves. Until it
// has moved, everything sent is still on its way, which needs no arithmetic at all. From
// then on, what is on its way is whatever was sent in the last (delay - 1) frames.
//
// Measuring per run rather than being told has two other effects worth having. A game
// that is not listening - paused, or the window has lost the mouse - is recognised after
// a handful of frames and one correction's worth of counts, not after a fortune of them
// has been queued to land all at once when it wakes. And a wrong gain only changes how
// fast the error shrinks; it cannot make the loop count a send twice.
//
// An earlier version inferred what was in flight from "degrees sent, less degrees of
// progress seen". Exact when the gain is exact. With the gain a quarter low it concluded
// nothing was in flight while plenty was, and swung eight degrees either side of the
// target for as long as it was allowed to. The test for that is still here.
//
// Start() once, then Step() every frame with the CURRENT error until Running() is false.
// Hold the target still while it runs - the direction the gaze had at the click, not the
// gaze as it wanders - or the head's motion is mistaken for the mouse's.
struct AimServo {
    enum Outcome { kIdle = 0, kRunning, kReached, kGaveUp, kRefused };

    float kp = 0.6f;                  // of what remains, per frame
    float toleranceDegrees = 0.3f;
    int   maxCalls = 16;              // under half a second at 36 frames a second
    int   maxWaitFrames = 5;          // no answer to the first correction in this long: deaf
    int   maxCountsPerFrame = 3000;   // a bound, so a bad gain cannot fling the view

    Outcome outcome = kIdle;
    int calls = 0;
    int lagFrames = 0;                // 0 until measured
    int firstSendCall = -1;
    float firstError = 0.0f;
    double firstWorth = 0.0;          // degrees the first correction should have been worth
    double recentDegrees[3] = { 0.0, 0.0, 0.0 }; // what the last three frames sent, newest first
    float carry = 0.0f;

    void Start() {
        outcome = kRunning;
        calls = 0;
        lagFrames = 0;
        firstSendCall = -1;
        firstError = 0.0f;
        firstWorth = 0.0;
        recentDegrees[0] = recentDegrees[1] = recentDegrees[2] = 0.0;
        carry = 0.0f;
    }

    void Cancel() { if (kRunning == outcome) outcome = kIdle; }

    bool Running() const { return kRunning == outcome; }

    // `errorDegrees` is target minus current, already wrapped by the caller if it is a yaw.
    // `degreesPerCount` is signed, as CountGainEstimator reports it. Returns the counts to
    // send this frame.
    int Step(float errorDegrees, float degreesPerCount) {
        if (kRunning != outcome) return 0;

        float gainSize = degreesPerCount < 0.0f ? -degreesPerCount : degreesPerCount;
        if (gainSize < 1e-6f) { outcome = kRefused; return 0; }

        // One count is the smallest step there is; asking for closer than that never ends.
        float tolerance = toleranceDegrees;
        if (tolerance < 0.75f * gainSize) tolerance = 0.75f * gainSize;

        if (0 == calls) firstError = errorDegrees;

        // Has the first correction arrived yet?
        if (0 == lagFrames && firstSendCall >= 0) {
            double progress = (double)firstError - (double)errorDegrees;
            double needed = 0.25 * (firstWorth < 0.0 ? -firstWorth : firstWorth);
            if (needed < 0.15) needed = 0.15;
            bool sameWay = (progress > 0.0) == (firstWorth > 0.0);
            double size = progress < 0.0 ? -progress : progress;
            if (sameWay && size >= needed) {
                lagFrames = calls - firstSendCall;
                if (lagFrames < 1) lagFrames = 1;
                if (lagFrames > 4) lagFrames = 4;
            }
        }

        // Sent and not yet seen.
        bool waiting = (0 == lagFrames && firstSendCall >= 0);
        double inFlight = 0.0;
        if (!waiting) {
            for (int i = 0; i < lagFrames - 1 && i < 3; i++) inFlight += recentDegrees[i];
        }

        float errorSize = errorDegrees < 0.0f ? -errorDegrees : errorDegrees;
        double flightSize = inFlight < 0.0 ? -inFlight : inFlight;
        if (!waiting && errorSize <= tolerance && flightSize <= (double)tolerance) {
            outcome = kReached;
            return 0;
        }
        if (calls >= maxCalls) { outcome = kGaveUp; return 0; }
        if (waiting && calls - firstSendCall >= maxWaitFrames) { outcome = kGaveUp; return 0; }

        int counts = 0;
        if (!waiting) {
            double remaining = (double)errorDegrees - inFlight;
            float wanted = (float)((double)kp * remaining / (double)degreesPerCount);
            if (wanted >  (float)maxCountsPerFrame) wanted =  (float)maxCountsPerFrame;
            if (wanted < -(float)maxCountsPerFrame) wanted = -(float)maxCountsPerFrame;
            counts = TakeWholeUnits(wanted, carry);
        }

        double worth = (double)counts * (double)degreesPerCount;
        if (firstSendCall < 0 && 0 != counts) { firstSendCall = calls; firstWorth = worth; }

        recentDegrees[2] = recentDegrees[1];
        recentDegrees[1] = recentDegrees[0];
        recentDegrees[0] = worth;
        calls++;
        return counts;
    }
};

// Keeps the aim on a direction that moves: the hand holding the gun.
//
// AimServo brings the aim to one fixed direction and stops. Pointing a controller is the
// same problem without the stopping - the target moves every frame, for ever, and what
// has to be bounded is not how long it takes but how far behind the aim trails and
// whether it rings when the hand stops.
//
// Same loop, same subtraction of what has been sent and not yet seen. The delay is not
// measured here: run an AimServo first to bring the aim onto the hand - it measures the
// delay as a by-product - and copy its lagFrames across. A delay believed one frame too
// SHORT is the dangerous direction (sends get counted as landed while still in flight),
// which is why kp is lower here than in the servo: at 0.5 that mistake rings and dies in
// a few frames, where at 0.8 it would ring for a second.
//
// A hand is never still. Tremor of a few tenths of a degree, passed straight through,
// becomes a crosshair that shivers and a stream of one-count corrections. So the target
// is smoothed before it is chased, by an amount that shrinks as the hand moves faster:
// heavy when it is nearly still, none at all in a sweep, so deliberate motion is not
// delayed by the cure for the accidental kind.
//
// With the hand sweeping steadily at v degrees a frame the aim trails it by about
// v x (lag - 1 + 1/kp): at 36 frames a second, lag 2 and sixty degrees a second, five
// degrees. It closes as soon as the hand slows, which is when anyone fires.
struct AimTracker {
    float kp = 0.5f;
    int   lagFrames = 2;              // from AimServo::lagFrames, once one has run
    float deadbandDegrees = 0.04f;    // do not chase less than this
    float stillSmoothing = 0.35f;     // how much of a new target is believed when still
    float smoothingPerDegree = 0.5f;  // and how much more per degree a frame of motion
    int   maxCountsPerFrame = 1500;
    bool  wraps = true;

    bool haveTarget = false;
    float smoothed = 0.0f;
    double recentDegrees[3] = { 0.0, 0.0, 0.0 };
    float carry = 0.0f;

    void Reset() {
        haveTarget = false;
        smoothed = 0.0f;
        recentDegrees[0] = recentDegrees[1] = recentDegrees[2] = 0.0;
        carry = 0.0f;
    }

    // The target after smoothing - where the aim is being asked to go. For drawing.
    float SmoothedTarget() const { return smoothed; }

    // `targetDegrees` is where the hand points, `currentDegrees` where the game aims, both
    // as read at the top of this frame. Returns the counts to send this frame.
    int Step(float targetDegrees, float currentDegrees, float degreesPerCount) {
        float gainSize = degreesPerCount < 0.0f ? -degreesPerCount : degreesPerCount;
        if (gainSize < 1e-6f) return 0;

        if (!haveTarget) {
            haveTarget = true;
            smoothed = targetDegrees;
        } else {
            float step = targetDegrees - smoothed;
            if (wraps) step = NormalizeDegrees(step);
            float size = step < 0.0f ? -step : step;
            float believe = stillSmoothing + smoothingPerDegree * size;
            if (believe > 1.0f) believe = 1.0f;
            smoothed += believe * step;
            if (wraps) smoothed = NormalizeDegrees(smoothed);
        }

        float error = smoothed - currentDegrees;
        if (wraps) error = NormalizeDegrees(error);

        int lag = lagFrames;
        if (lag < 1) lag = 1;
        if (lag > 4) lag = 4;
        double inFlight = 0.0;
        for (int i = 0; i < lag - 1; i++) inFlight += recentDegrees[i];

        double remaining = (double)error - inFlight;
        double remainingSize = remaining < 0.0 ? -remaining : remaining;

        int counts = 0;
        if (remainingSize > (double)deadbandDegrees) {
            float wanted = (float)((double)kp * remaining / (double)degreesPerCount);
            if (wanted >  (float)maxCountsPerFrame) wanted =  (float)maxCountsPerFrame;
            if (wanted < -(float)maxCountsPerFrame) wanted = -(float)maxCountsPerFrame;
            counts = TakeWholeUnits(wanted, carry);
        } else {
            carry = 0.0f;
        }

        recentDegrees[2] = recentDegrees[1];
        recentDegrees[1] = recentDegrees[0];
        recentDegrees[0] = (double)counts * (double)degreesPerCount;
        return counts;
    }
};


// ---------------------------------------------------------------------------------
// Field of view
// ---------------------------------------------------------------------------------
//
// Source is handed ONE angle and renders it horizontally; the vertical follows from the
// shape of the image. Nothing here can change that, so all of these exist to answer one
// question - what number do we hand it - and every one of them was got wrong at least
// once with a headset on. That is why they are down here with tests rather than up in
// MirvVrXr.cpp with the globals they used to read.
//
// Angles in: radians, OpenXR's units. Angles out: degrees, Source's.

// The widest symmetric frustum that covers an asymmetric one. CS2 can only express a
// single fov, so the eye is rendered wider than needed and the angles actually rendered
// are reported back to the compositor - correct, at the cost of edge pixels.
inline float SymmetricFovDegrees(float angleLeft, float angleRight) {
    float a = fabsf(angleLeft), b = fabsf(angleRight);
    float half = a > b ? a : b;
    return (float)(2.0 * (double)half * 180.0 / 3.14159265358979323846);
}

// The engine's chain, forwards: what it actually renders horizontally when handed
// `askedDegrees` into an image of this shape.
//
// The 0.75 is the 4:3 convention. The number in the fov field is the horizontal angle a
// 4:3 image would have had: the engine derives a vertical from it, then the real
// horizontal from the real aspect. An aspect of zero or less means the swapchain does not
// exist yet, and the only honest answer then is the question.
inline float RenderedFovForAsked(float askedDegrees, float aspect) {
    const double d2r = 3.14159265358979323846 / 180.0, r2d = 180.0 / 3.14159265358979323846;
    if (aspect <= 0.0f) return askedDegrees;
    double halfY = atan(tan(0.5 * (double)askedDegrees * d2r) * 0.75);
    double halfX = atan(tan(halfY) * (double)aspect);
    return (float)(2.0 * halfX * r2d);
}

// The same chain backwards: the angle to hand the engine so that it renders
// `wantedDegrees` horizontally. The exact inverse of RenderedFovForAsked, which is what
// the round-trip test checks - the two were written weeks apart and only one of them
// could be right.
inline float SourceFovForWanted(float wantedDegrees, float aspect) {
    const double d2r = 3.14159265358979323846 / 180.0, r2d = 180.0 / 3.14159265358979323846;
    if (aspect <= 0.0f) return wantedDegrees;
    double halfX = 0.5 * (double)wantedDegrees * d2r;
    double halfY = atan(tan(halfX) / (double)aspect);
    double half43 = atan(tan(halfY) / 0.75);
    return (float)(2.0 * half43 * r2d);
}

// The smallest symmetric horizontal angle that contains the runtime's frustum AND leaves
// the vertical tall enough once this image's shape has had its way with it.
//
// Horizontally that is just the wider of the two angles. Vertically there is no choice to
// make, so if the image is too wide the vertical comes out short and there is nothing
// drawn where the headset wants to look.
//
// Measured, on the day a window was changed from 2528x2780 to 2560x1600 to make CS2's menu
// fit on the monitor. The Quest 3 wants 110 degrees vertically (up 44, down 55, so 55
// either side of a symmetric frustum). At the tall window a 108 degree horizontal gave 113
// vertical - enough, narrowly, which is why nobody had to think about it. At the wide one
// it gives 81: twenty-nine degrees short, and the operator's words were "everything at the
// edges is badly distorted".
//
// So ask for whichever horizontal angle satisfies BOTH, and let the crop throw away what
// is not needed. The cost is pixels: at 1.6:1 about 40 per cent of each row is rendered
// and discarded. The cure for that is a window whose shape matches the headset's frustum -
// tan(54)/tan(55), about 0.96:1, which is what 2528x2780 nearly was - not a smaller field
// of view.
inline float ContainingFovDegrees(float angleLeft, float angleRight,
                                  float angleUp, float angleDown, float aspect) {
    const double r2d = 180.0 / 3.14159265358979323846;
    double halfH = 0.5 * (double)SymmetricFovDegrees(angleLeft, angleRight) / r2d;

    float up = fabsf(angleUp), down = fabsf(angleDown);
    double halfV = (up > down) ? up : down;

    if (aspect > 0.0f) {
        double neededH = atan(tan((double)halfV) * (double)aspect);
        if (neededH > halfH) halfH = neededH;
    }
    return (float)(2.0 * halfH * r2d);
}

// Every angle handed to the engine passes through this. Zero degrees, or four hundred, is
// not a field of view - it is a bug reaching another program's memory. The bounds differ
// between the two callers, because what we ask the engine for may legitimately exceed
// what we wanted: undoing the 4:3 convention inflates it.
inline float ClampFovDegrees(float degrees, float lo, float hi) {
    if (degrees < lo) return lo;
    if (degrees > hi) return hi;
    return degrees;
}

} // namespace AfxVrMath

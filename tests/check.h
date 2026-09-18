#pragma once

// A test harness small enough to read in one sitting, because the alternative is making
// the build of a 1300-line hook depend on fetching a test framework. Nothing here needs
// to be clever: the tests it runs are about arithmetic and string parsing.

#include <stdio.h>
#include <math.h>

namespace check {

inline int & Failures() { static int n = 0; return n; }
inline int & Checks() { static int n = 0; return n; }
inline const char * & CurrentCase() { static const char * s = "(none)"; return s; }

inline void Fail(const char * file, int line, const char * what) {
    Failures()++;
    printf("  FAIL  %s\n        at %s:%d\n", what, file, line);
}

inline void Case(const char * name) {
    CurrentCase() = name;
    printf("- %s\n", name);
}

inline bool NearlyEqual(double a, double b, double tolerance) {
    double d = a - b;
    if (d < 0) d = -d;
    return d <= tolerance;
}

} // namespace check

#define CHECK(expr) \
    do { \
        check::Checks()++; \
        if (!(expr)) check::Fail(__FILE__, __LINE__, #expr); \
    } while (0)

#define CHECK_NEAR(a, b, tol) \
    do { \
        check::Checks()++; \
        if (!check::NearlyEqual((a), (b), (tol))) { \
            char buf[512]; \
            snprintf(buf, sizeof(buf), "%s == %s  (got %.9g, wanted %.9g, tolerance %g)", \
                     #a, #b, (double)(a), (double)(b), (double)(tol)); \
            check::Fail(__FILE__, __LINE__, buf); \
        } \
    } while (0)

#define CHECK_STR(a, b) \
    do { \
        check::Checks()++; \
        if (0 != strcmp((a), (b))) { \
            char buf[512]; \
            snprintf(buf, sizeof(buf), "%s == \"%s\"  (got \"%s\")", #a, (b), (a)); \
            check::Fail(__FILE__, __LINE__, buf); \
        } \
    } while (0)

#define CHECK_MAIN() \
    int main() { \
        RunTests(); \
        printf("\n%d checks, %d failures\n", check::Checks(), check::Failures()); \
        return check::Failures() ? 1 : 0; \
    }

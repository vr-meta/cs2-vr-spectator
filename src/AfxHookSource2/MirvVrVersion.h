#pragma once

// The one place this project stamps itself, and the one place a release has to set.
//
// It exists because an alpha's entire support channel is what a stranger can copy out of
// console.log. "It does not work" is unanswerable; "cs2-vr-spectator 0.1.0, built for CS2
// 2000908, this game is 2001174" answers itself. Both numbers therefore have to be in the
// log of every session, whether or not anything went wrong, and they have to be one edit
// apart so a release cannot ship a version string describing the previous build.
//
// Every value here can be overridden from the compiler command line, which is how the
// release workflow sets them without a commit that edits a file:
//
//     /D AFXVR_VERSION=\"0.1.0\"
//
// Unset, the version says so: a build made from a working tree is not a release and must
// not claim a release's number. If a bug report quotes "0.0.0-dev" then whoever filed it
// built it themselves, which is a different conversation.

#ifndef AFXVR_VERSION
#define AFXVR_VERSION "0.0.0-dev"
#endif

// The CS2 ClientVersion the view field offsets in MirvVr.cpp were measured against.
//
// This lives here rather than beside the offsets because it is half of what a release
// promises - a zip is for one game build - and the workflow that assembles the zip puts
// it in the file name. Two copies of that number would eventually disagree.
#ifndef AFXVR_TESTED_CLIENT_VERSION
#define AFXVR_TESTED_CLIENT_VERSION "2000908"
#endif

// Where somebody reading a warning in console.log can go. Messages used to name files
// under docs/, which is right for this repository and useless in a zip that does not
// carry them.
#ifndef AFXVR_PROJECT_URL
#define AFXVR_PROJECT_URL "https://github.com/vr-meta/cs2-vr-spectator"
#endif

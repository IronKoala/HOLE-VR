#pragma once
// The source revision this DLL was built from: the git short hash, with
// "-dirty" when tracked files had uncommitted changes. Generated into the
// build folder at every build (cmake/build_id.cmake); logged at start-up and
// written into diagnostic captures, so a log names the exact code that ran.
#if __has_include("generated_build_id.h")
#include "generated_build_id.h"
#endif
#ifndef HOLEVR_BUILD_ID
#define HOLEVR_BUILD_ID "unknown"
#endif

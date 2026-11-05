#pragma once
#include "native_linear_sample_target.h"
// Preserve standalone fixtures while testing the same owned implementation
// intended for native rendering. No game admission is implied by this alias.
namespace recomp::gpu { using MultisampleDrawProbe=NativeLinearSampleTarget; }

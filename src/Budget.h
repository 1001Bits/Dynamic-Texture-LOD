#pragma once

#include "core/BudgetMode.h"

// Budget mode for Fallout 4: the shared dtl::budget runner, configured from
// the ini. Lod plans against it each cycle.
namespace Budget
{
    // Hands the [Budget] settings to the runner. Call after the settings are read and
    // before the video memory monitor starts.
    void Configure();
}

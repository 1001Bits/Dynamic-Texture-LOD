#include "Budget.h"

namespace Budget
{
    // Lod's planning thread runs on its own clock and reads the latest
    // assessment, so nothing needs calling on each reading.
    void Configure() { dtl::budget::Configure(dtl::budget::FromIni(), nullptr); }
}

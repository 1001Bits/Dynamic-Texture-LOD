#pragma once

// The plugin builds with /EHa, so a worker's catch (...) also stops an access
// violation, but says nothing about it. On a thread that called this, such a
// fault arrives as a std::exception whose what() names the code, the module
// and offset it happened at and, for an access violation, the address.
namespace FaultReport
{
    void TranslateOnThisThread();
}

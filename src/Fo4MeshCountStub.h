#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace Fo4Mesh {
    // Exact OG type5 count call14287F177: RCX=geometry,RDX=LOD mode,R14=pass.
    // Tail-jump adds the borrowed pass as argument3 without changing the
    // native return address, nonvolatile registers, stack or unwind frame.
    inline std::array<std::uint8_t,17> MakeCountStub(std::uintptr_t target) {
        std::array<std::uint8_t,17> code{0x4d,0x8b,0xc6, 0xff,0x25,0,0,0,0, 0,0,0,0,0,0,0,0};
        std::memcpy(code.data()+9,&target,8);return code;
    }
}

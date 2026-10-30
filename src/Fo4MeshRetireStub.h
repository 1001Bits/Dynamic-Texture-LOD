#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace Fo4Mesh {
    // OG1.10.163 Renderer::DecRef(TriShape*) zero-ref branch at 1D0C5EB.
    // The callback runs before either native Buffer wrapper loses its owner.
    // RDI=shape and RSI=renderer survive the callback. RCX must be restored:
    // the resumed C5F5 reads the renderer from RCX if the index refs reach0.
    struct alignas(4) RetireStub {
        std::array<std::uint8_t,44> code{};
        // Native frame: RBX saved [RSP+30], RSI [RSP+38], PUSH RDI,
        // SUB RSP,20. Describe that frame for stack walks in the callback.
        std::array<std::uint8_t,16> unwind{1,0,6,0, 0,0x34,6,0, 0,0x64,7,0, 0,0x32,0,0x70};
    };
    static_assert(sizeof(RetireStub)==60);
    inline RetireStub MakeRetireStub(std::uintptr_t callback, std::uintptr_t resume) {
        RetireStub result;
        result.code={
            0x48,0x8b,0xcf,                                     // RCX=RDI
            0x48,0xb8,0,0,0,0,0,0,0,0, 0xff,0xd0,               // call callback
            0x48,0x8b,0xce, 0x48,0x8b,0xd7,                     // restore RCX=RSI,RDX=RDI
            0x48,0x8b,0x52,0x10, 0xf0,0xff,0x4a,0x38,          // native MOV/LOCK DEC
            0xff,0x25,0,0,0,0, 0,0,0,0,0,0,0,0, 0x90};         // resume with DEC flags
        std::memcpy(result.code.data()+5,&callback,8);
        std::memcpy(result.code.data()+35,&resume,8);
        return result;
    }
}

#include <Windows.h>
#include "Fo4MeshRetireStub.h"
#include "Fo4MeshCountStub.h"
#include <cstdlib>
#include <iostream>

namespace {
    void Check(bool value,const char* message) { if(!value) {std::cerr<<message<<'\n';std::exit(1);} }
    struct Buffer {std::uint8_t pad[0x38]{}; LONG refs=1;};
    struct Shape {std::uint64_t desc=0;Buffer* vertices=nullptr;Buffer* indices=nullptr;};
    Shape* seen=nullptr;
    void BeforeRelease(Shape* shape) {seen=shape;Check(shape->indices->refs==1,"callback before owner release");}
    std::uint32_t Count(void* geometry,std::uint32_t mode,void* pass) {
        return geometry==reinterpret_cast<void*>(0x1234) && mode==3 && pass==reinterpret_cast<void*>(0x5678) ? 321 : 0;
    }
}
int main() {
    auto* memory=static_cast<std::byte*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    Check(memory!=nullptr,"code allocation");
    // Reproduce the proven native stack state, including saved nonvolatiles.
    const std::uint8_t prefix[]{0x48,0x89,0x5c,0x24,0x08, 0x48,0x89,0x74,0x24,0x10,
        0x57,0x48,0x83,0xec,0x20, 0x48,0x8b,0xfa,0x48,0x8b,0xf1, 0xff,0x25,0,0,0,0};
    std::memcpy(memory,prefix,sizeof(prefix));
    const auto address=reinterpret_cast<std::uintptr_t>(memory+64);
    std::memcpy(memory+27,&address,8);
    const auto stub=Fo4Mesh::MakeRetireStub(reinterpret_cast<std::uintptr_t>(&BeforeRelease),reinterpret_cast<std::uintptr_t>(memory+160));
    std::memcpy(memory+64,&stub,sizeof(stub));
    // Preserve DEC flags, verify restored RCX by comparing to RSI, and
    // return1 only if both DEC zero and renderer identity are correct.
    const std::uint8_t resume[]{0x0f,0x94,0xc0,0x0f,0xb6,0xc0,0x48,0x39,0xf1,0x0f,0x94,0xc2,
        0x0f,0xb6,0xd2,0x21,0xd0, 0x48,0x8b,0x5c,0x24,0x30,0x48,0x8b,0x74,0x24,0x38,
        0x48,0x83,0xc4,0x20,0x5f,0xc3};
    std::memcpy(memory+160,resume,sizeof(resume));
    FlushInstructionCache(GetCurrentProcess(),memory,4096);
    RUNTIME_FUNCTION function{0,43,44};
    Check(RtlAddFunctionTable(&function,1,address)!=FALSE,"register unwind");
    Buffer indices;Shape shape;shape.indices=&indices;
    using Execute=int(*)(void*,Shape*);
    Check(reinterpret_cast<Execute>(memory)(reinterpret_cast<void*>(0x1234),&shape)==1 && seen==&shape && indices.refs==0,
        "callback identity, renderer restoration and LOCK DEC flags");
    DWORD64 base=0;
    auto* entry=RtlLookupFunctionEntry(address+15,&base,nullptr);
    Check(entry==&function && base==address,"lookup unwind");
    alignas(16) std::uint64_t stack[10]{};
    stack[4]=0x7777;stack[5]=0x4444;stack[6]=0x3333;stack[7]=0x6666;
    CONTEXT context{};context.Rsp=reinterpret_cast<DWORD64>(stack);context.Rip=address+15;
    PVOID handler=nullptr;DWORD64 establisher=0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,context.Rip,entry,&context,&handler,&establisher,nullptr);
    Check(context.Rbx==stack[6] && context.Rsi==stack[7] && context.Rdi==stack[4] && context.Rip==stack[5] &&
        context.Rsp==reinterpret_cast<DWORD64>(stack+6),"unwind restores caller and all native nonvolatiles");
    Check(RtlDeleteFunctionTable(&function)!=FALSE,"unregister unwind");
    const auto countStub=Fo4Mesh::MakeCountStub(reinterpret_cast<std::uintptr_t>(&Count));
    std::memcpy(memory+256,countStub.data(),countStub.size());
    const std::uint8_t countPrefix[]{0x41,0x56,0x48,0x83,0xec,0x20,0x4d,0x8b,0xf0,0x45,0x33,0xc0,
        0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xd0,0x48,0x83,0xc4,0x20,0x41,0x5e,0xc3};
    std::memcpy(memory+320,countPrefix,sizeof(countPrefix));
    const auto countAddress=reinterpret_cast<std::uintptr_t>(memory+256);
    std::memcpy(memory+334,&countAddress,8);
    FlushInstructionCache(GetCurrentProcess(),memory,4096);
    using CountExecute=std::uint32_t(*)(void*,std::uint32_t,void*);
    Check(reinterpret_cast<CountExecute>(memory+320)(reinterpret_cast<void*>(0x1234),3,reinterpret_cast<void*>(0x5678))==321,
        "count shim forwards native R14 pass without changing existing arguments");
    VirtualFree(memory,0,MEM_RELEASE);
    std::cout<<"FO4 zero-ref stub callback, register/flag replay and OS unwind passed\n";
}

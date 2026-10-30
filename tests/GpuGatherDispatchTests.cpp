#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "../src/GpuGatherDispatch.h"
#include <cassert>
#include <iostream>

namespace {
    template<class T, std::size_t N> void Put(std::array<std::byte,N>& value,std::size_t offset,T field)
    { std::memcpy(value.data()+offset,&field,sizeof(field)); }
    std::uintptr_t Probe(std::uintptr_t a,std::uintptr_t b,std::uintptr_t head,std::uintptr_t d)
    { assert(a==0x1234 && b==0x5678 && head==0xABCDEF && d==0x9988); return 0xBEEF; }
    void* Executable(const std::vector<std::uint8_t>& code)
    {
        auto* p=VirtualAlloc(nullptr,code.size(),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); assert(p);
        std::memcpy(p,code.data(),code.size()); DWORD old{};
        assert(VirtualProtect(p,code.size(),PAGE_EXECUTE_READ,&old));
        assert(FlushInstructionCache(GetCurrentProcess(),p,code.size())); return p;
    }
    void Append64(std::vector<std::uint8_t>& code,std::uintptr_t value)
    { const auto* bytes=reinterpret_cast<const std::uint8_t*>(&value);code.insert(code.end(),bytes,bytes+8); }
}
int main()
{
    using namespace GpuGather::Dispatch;
    std::array<std::byte,0xB8> command{},unsubmitted{},zero{},dynamic{};
    Put(command,4,std::uint8_t{1});Put(command,0x3C,std::uint32_t{12});unsubmitted=command;dynamic=command;
    Put(dynamic,0x68,std::uintptr_t{1});Put(zero,4,std::uint8_t{1});
    std::array<std::byte,0x50> a{},b{},c{},d{},e{};
    Put(a,0,command.data());Put(a,0x40,b.data());Put(b,0,unsubmitted.data());Put(b,0x40,c.data());
    Put(c,0,zero.data());Put(c,0x40,d.data());Put(d,0,dynamic.data());Put(d,0x40,e.data());
    std::array<void*,5> queue{command.data(),zero.data(),dynamic.data(),nullptr,nullptr};
    std::vector<std::uintptr_t> submitted;std::vector<void*> passes;std::size_t accepted{};
    assert(SubmittedPasses(queue.data(),a.data(),submitted,passes,[&](void* p){assert(p==a.data());++accepted;}));
    assert(accepted==1); // absent commands, zero counts, dynamic and null command excluded
    Put(e,0x40,b.data());accepted=0;
    assert(!SubmittedPasses(queue.data(),a.data(),submitted,passes,[&](void*){++accepted;}));assert(!accepted);
    Put(e,0x40,static_cast<void*>(nullptr));
    std::vector<void*> full(kCommands+1,command.data());full.back()=nullptr;
    assert(SubmittedPasses(full.data(),a.data(),submitted,passes,[&](void*){++accepted;}));assert(accepted==1);
    full.back()=command.data();accepted=0;
    assert(!SubmittedPasses(full.data(),a.data(),submitted,passes,[&](void*){++accepted;}));assert(!accepted);
    assert(!SubmittedPasses(nullptr,a.data(),submitted,passes,[](void*){}));

    // Execute both actual leaf gateways with their verified caller registers.
    // The caller allocates Win64 shadow space and saves its nonvolatile registers.
    std::vector<std::uint8_t> caller{0x53,0x56,0x41,0x56,0x48,0x83,0xEC,0x20,
        0x48,0x8B,0xC1,0x48,0x8B,0xDA,0x48,0x8B,0xF2,0x45,0x33,0xF6,0x48,0xB9};
    Append64(caller,0x1234);caller.insert(caller.end(),{0x48,0xBA});Append64(caller,0x5678);
    caller.insert(caller.end(),{0x49,0xB9});Append64(caller,0x9988);
    caller.insert(caller.end(),{0xFF,0xD0,0x48,0x83,0xC4,0x20,0x41,0x5E,0x5E,0x5B,0xC3});
    auto* invoke=Executable(caller);std::uintptr_t head=0xABCDEF;
    for (bool inlineBatch:{false,true}) {
        const auto bytes=Gateway(inlineBatch,reinterpret_cast<std::uintptr_t>(Probe));auto* gateway=Executable(bytes);
        assert(reinterpret_cast<std::uintptr_t(*)(void*,void*)>(invoke)(gateway,&head)==0xBEEF);
        assert(VirtualFree(gateway,0,MEM_RELEASE));
    }
    assert(VirtualFree(invoke,0,MEM_RELEASE));
    std::cout << "GpuGather dispatch admission and both native register gateways passed\n";
}

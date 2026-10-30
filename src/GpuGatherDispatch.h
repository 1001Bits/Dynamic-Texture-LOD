#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

// Pure helpers shared by the native adapter and its synthetic/ABI tests.
namespace GpuGather::Dispatch
{
    inline constexpr std::size_t kCommands = 8192, kPasses = 16384;

    template <class T> T Read(const void* pointer, std::size_t offset = 0) noexcept
    {
        T value{};
        std::memcpy(&value, static_cast<const std::byte*>(pointer) + offset, sizeof(value));
        return value;
    }

    // ProcessCommandBuffer uses command[4] draw ranges and uint32 counts at
    // +0x3C. A rigid cached command has one range; other layouts fail closed.
    inline bool RigidCommandHasPrimitives(const void* command) noexcept
    {
        return command && Read<std::uint8_t>(command, 4) == 1 &&
            Read<std::uint32_t>(command, 0x3C) != 0 &&
            Read<std::uintptr_t>(command, 0x68) == 0; // dynamic geometry path
    }

    template <class Callback>
    bool SubmittedPasses(const void* commands, void* head,
        std::vector<std::uintptr_t>& submitted, std::vector<void*>& passes, Callback&& callback)
    {
        submitted.clear();
        passes.clear();
        if (!commands) return false;
        std::size_t index{};
        for (; index <= kCommands; ++index) {
            const auto command = Read<std::uintptr_t>(commands, index * sizeof(void*));
            if (!command) break;
            if (index == kCommands) return false;
            if (RigidCommandHasPrimitives(reinterpret_cast<void*>(command))) submitted.push_back(command);
        }
        // The bounded list is validated completely before publishing any
        // observations, so an overlong/cyclic list produces no partial demand.
        for (; head && passes.size() != kPasses; head = Read<void*>(head, 0x40)) passes.push_back(head);
        if (head) return false;
        std::sort(submitted.begin(), submitted.end());
        for (void* pass : passes) {
            const auto command = Read<std::uintptr_t>(pass);
            if (command && std::binary_search(submitted.begin(), submitted.end(), command)) callback(pass);
        }
        return true;
    }

    // A leaf gateway changes only the previously unspecified third argument.
    // No stack/nonvolatile changes, flags unchanged, absolute tail jump to the
    // compiled wrapper. The original CALL supplies its ordinary return address.
    inline std::vector<std::uint8_t> Gateway(bool inlineBatch, std::uintptr_t wrapper)
    {
        std::vector<std::uint8_t> code = inlineBatch ?
            std::vector<std::uint8_t>{ 0x4E, 0x8B, 0x04, 0xF6 } : // R8=[RSI+R14*8]
            std::vector<std::uint8_t>{ 0x4C, 0x8B, 0x03 };        // R8=[RBX]
        constexpr std::uint8_t jump[]{ 0xFF, 0x25, 0, 0, 0, 0 };
        code.insert(code.end(), std::begin(jump), std::end(jump));
        const auto* target = reinterpret_cast<const std::uint8_t*>(&wrapper);
        code.insert(code.end(), target, target + sizeof(wrapper));
        return code;
    }
}

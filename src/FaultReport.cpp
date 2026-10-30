#include "FaultReport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <eh.h>

#include <cstring>
#include <exception>
#include <format>
#include <string>

namespace
{
    class Fault : public std::exception
    {
    public:
        explicit Fault(std::string a_text) : text(std::move(a_text)) {}
        const char* what() const noexcept override { return text.c_str(); }

    private:
        std::string text;
    };

    // "Fallout4.exe+0x1D37C71" when the address lies in a loaded module.
    std::string Where(const void* a_address)
    {
        HMODULE module = nullptr;
        char path[MAX_PATH]{};
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCSTR>(a_address), &module) && ::GetModuleFileNameA(module, path, MAX_PATH)) {
            const char* name = std::strrchr(path, '\\');
            return std::format("{}+0x{:X}", name ? name + 1 : path,
                static_cast<const char*>(a_address) - reinterpret_cast<const char*>(module));
        }
        return std::format("0x{:X}", reinterpret_cast<std::uintptr_t>(a_address));
    }

    void Translate(unsigned int a_code, EXCEPTION_POINTERS* a_info)
    {
        const auto* record = a_info ? a_info->ExceptionRecord : nullptr;
        auto text = std::format("structured exception 0x{:08X} at {}", a_code,
            record ? Where(record->ExceptionAddress) : std::string("an unknown address"));
        if (record && a_code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
            const auto kind = record->ExceptionInformation[0];
            text += std::format(", {} 0x{:X}", kind == 1 ? "writing" : kind == 8 ? "executing" : "reading",
                record->ExceptionInformation[1]);
        }
        throw Fault(std::move(text));
    }
}

namespace FaultReport
{
    void TranslateOnThisThread() { _set_se_translator(&Translate); }
}

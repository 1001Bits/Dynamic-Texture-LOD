#include "Addresses.h"
#include "AddressLibrary.h"

namespace Addr
{
    std::uintptr_t ImageAddress(std::uintptr_t offset)
    {
        if (!offset) return 0;
        const auto& module = REL::Module::get();
        const auto address = module.base() + offset;
        if (address < module.base()) return 0;
        for (unsigned i = 0; i < REL::Segment::total; ++i) {
            const auto segment = module.segment(static_cast<REL::Segment::Name>(i));
            if (address >= segment.address() && address - segment.address() < segment.size()) return address;
        }
        logger::error("RVA {:X} lies outside the runtime's known image sections", offset);
        return 0;
    }

    std::uintptr_t Resolve(const Location& location)
    {
        if (REL::Module::IsVR()) return ImageAddress(location.vr);
        const auto id = REL::Module::IsNG() ? location.ng : location.og;
        if (!id) return 0;
        const auto version = REL::Module::get().version();
        const auto path = std::format("Data/F4SE/Plugins/version-{}.bin", version.string("-"sv));
        std::ifstream input(path, std::ios::binary);
        const auto offset = ExactOffset(input, id);
        if (!offset) {
            logger::error("Exact Address Library ID {} absent or invalid in {}", id, path);
            return 0;
        }
        return ImageAddress(*offset);
    }

    std::uintptr_t RendererDataSlot()
    {
        // Validated runtime globals, also independently used by the private
        // Present probe. The OG-only CommonLib singleton ID is absent on AE.
        return Resolve(RendererData);
    }
}

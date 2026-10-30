#include "Fo4MeshPilot.h"
#include <fstream>
#include <iostream>

static_assert(_WIN32_WINNT == 0x0601 && WINVER == 0x0601, "Match the product's Windows7 API baseline");

namespace {
    unsigned checks{};
    void Check(bool value, const char* message) {
        ++checks;
        if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
    }
    void Write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream stream(path, std::ios::binary); stream << text;
        Check(bool(stream), "test input written");
    }
}
int main() {
    using namespace Fo4Mesh;
    using namespace Fo4Mesh::Pilot;
    const std::string abc = "abc";
    auto abcHash = Hash(std::as_bytes(std::span(abc)));
    Check(abcHash && Hex(*abcHash) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 known vector");
    const auto emptyHash=Hash({});
    Check(emptyHash && Hex(*emptyHash)=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty hash with explicit provider lifecycle");
    const std::string millionAs(1000000,'a');
    const auto longHash=Hash(std::as_bytes(std::span(millionAs)));
    Check(longHash && Hex(*longHash)=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "multi-block SHA256 known vector");
    Check(!ParseDigest(L"bad"), "short digest rejected");
    Check(!ParseDigest(std::wstring(64, L'z')), "nonhex digest rejected");
    auto digestText = Hex(*abcHash);
    Check(ParseDigest(std::wstring(digestText.begin(), digestText.end())) == abcHash, "digest round trip");

    NativeBuffer fullVB{}, fullIB{}, reducedVB{}, reducedIB{};
    fullVB.capacity = 1024; fullIB.capacity = 512; reducedVB.capacity = 256; reducedIB.capacity = 128;
    NativeShape live{0x100000000003, &reducedVB, &reducedIB, 1}, full{live.descriptor, &fullVB, &fullIB, 1};
    FullPair retained;
    Check(retained.Retain(live, full), "retain complete native pair");
    Check(retained.FullCapacity() == 1536 && retained.HasFullSource(), "full capacity remains charged");
    Check(!retained.TakeRestored(), "cannot retire the restoration guarantee while reduced");
    Check(!retained.Retain(live, full), "cannot overwrite owned full pair");
    Check(retained.Restore(), "restore requires no allocation or engine call");
    Check(live.vertices == &fullVB && live.indices == &fullIB && full.vertices == &reducedVB && full.indices == &reducedIB,
        "restoration transfers original pointers and complete native wrappers");
    Check(live.references == 1 && full.references == 1, "restoration does not fabricate native owners");
    Check(!retained.HasFullSource() && !retained.FullCapacity(), "full pair is now owned by live shape");
    Check(retained.InactiveCapacity() == 384, "reduced pair remains honestly charged after restoration");
    Check(!retained.Restore(), "repeated restoration cannot swap back to reduced");
    ++reducedVB.capacity;
    Check(!retained.UseReduced(), "retained reduced capacity change prevents reuse");
    --reducedVB.capacity;
    reducedIB.offset=4;
    Check(!retained.UseReduced(), "retained reduced offset change prevents reuse");
    reducedIB.offset=0;
    reducedVB.buffer=reinterpret_cast<ID3D11Buffer*>(1);
    Check(!retained.UseReduced(), "retained reduced resource change prevents reuse");
    reducedVB.buffer=nullptr;
    ++reducedIB.size;
    Check(!retained.UseReduced(), "retained reduced payload-size change prevents reuse");
    --reducedIB.size;
    reducedVB.suballocated=true;
    Check(!retained.UseReduced(), "retained reduced allocation kind change prevents reuse");
    reducedVB.suballocated=false;
    Check(retained.UseReduced() && retained.InactiveCapacity() == 1536, "ready pair reused without another allocation");
    Check(live.vertices == &reducedVB && full.vertices == &fullVB, "repeated downgrade preserves exact full guarantee");
    const auto originalDescriptor=live.descriptor;
    ++live.descriptor;++full.descriptor;
    Check(!retained.Restore(), "matching changed descriptors cannot replace original binding identity");
    live.descriptor=full.descriptor=originalDescriptor;
    Check(retained.Restore(), "second allocation-free restoration");
    Check(retained.TakeRestored() == &full && !retained.Owned(), "reduced retirement receipt transferred once");
    Check(!retained.TakeRestored(), "no duplicate retirement ownership");

    NativeBuffer secondFullVB{}, secondFullIB{}, secondSmallVB{}, secondSmallIB{};
    secondFullVB.capacity = 2048; secondFullIB.capacity = 1024;
    NativeShape secondLive{live.descriptor, &secondSmallVB, &secondSmallIB, 1};
    NativeShape secondFull{live.descriptor, &secondFullVB, &secondFullIB, 1};
    // Each admitted record must retain its own full pair. This intentionally
    // exercises the multiple-unsupported-consumer case a shared reserve fails.
    live.vertices = &reducedVB; live.indices = &reducedIB; full.vertices = &fullVB; full.indices = &fullIB;
    FullPair firstGuarantee, secondGuarantee;
    Check(firstGuarantee.Retain(live, full) && secondGuarantee.Retain(secondLive, secondFull), "independent guarantees");
    Check(firstGuarantee.Restore() && secondGuarantee.Restore(), "two unexpected consumers restore independently");
    Check(live.vertices == &fullVB && secondLive.vertices == &secondFullVB, "no shared emergency-buffer alias");
    auto moved = std::move(secondGuarantee);
    Check(!secondGuarantee.Owned() && moved.Owned() == &secondFull, "move transfers sole native ownership receipt");
    Check(moved.TakeForOwnerDestruction() == &secondFull, "owner teardown returns retained pair for native retirement");

    const auto temporary = std::filesystem::temp_directory_path() / (L"dtl-mesh-pilot-" + std::to_wstring(GetCurrentProcessId()));
    const auto meshes = temporary / L"game" / L"Data" / L"meshes";
    std::filesystem::create_directories(meshes);
    const auto fixture = meshes / L"pilot.nif", manifest = temporary / L"pilot.ini";
    Write(fixture, abc);
    const auto manifestText = "[Pilot]\nMode=SingleFixtureRetainedFull\nMaximumRecords=1\nNetMemorySavingClaim=0\n"
        "FixturePath=" + fixture.string() + "\nFixtureSHA256=" + digestText + "\nGeometryName=DTLMeshPilot_test\n"
        "DescriptorHex=100000000003\nVertexCount=3\nTriangleCount=2\nSegment0=1\nSegment1=1\nSegment2=0\n"
        "VerticesSHA256=" + digestText + "\nIndicesSHA256=" + digestText + "\n";
    Write(manifest, manifestText);
    {
        const auto loaded = Load(manifest, temporary / L"game");
        Check(loaded.has_value(), "valid explicit private identity manifest");
        Check(loaded->Metadata("DTLMeshPilot_test", 0x100000000003, 3, 2, {1, 1, 0}), "exact runtime metadata match");
        Check(!loaded->Metadata("another", 0x100000000003, 3, 2, {1, 1, 0}), "unrelated geometry excluded");
        Check(!loaded->Metadata("DTLMeshPilot_test", 0x100000000003, 3, 2, {2, 0, 0}), "same shape with changed segments excluded");
        const auto handle = CreateFileW(fixture.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "fixture immutable during admitted lifetime");
        Check(!DeleteFileW(manifest.c_str()) && GetLastError() == ERROR_SHARING_VIOLATION, "manifest immutable during admitted lifetime");
        Check(!loaded->Source({{}, {}, 12}), "unverified source rejected");
    }
    auto invalid = manifestText;
    invalid.replace(invalid.find("MaximumRecords=1"), 16, "MaximumRecords=2");
    Write(manifest, invalid);
    Check(!Load(manifest, temporary / L"game"), "pilot rejects more than one admitted record");
    invalid = manifestText; invalid.replace(invalid.find("NetMemorySavingClaim=0"), 22, "NetMemorySavingClaim=1");
    Write(manifest, invalid);
    Check(!Load(manifest, temporary / L"game"), "cannot label retained-full pilot as net saving");
    Write(manifest, manifestText);
    Write(fixture, "changed");
    Check(!Load(manifest, temporary / L"game"), "changed fixture hash rejected");
    Check(!Load(L"relative.ini", temporary / L"game"), "relative manifest rejected");
    // Delete only exact private test files, then their known empty directories.
    std::filesystem::remove(manifest); std::filesystem::remove(fixture);
    std::filesystem::remove(meshes); std::filesystem::remove(meshes.parent_path());
    std::filesystem::remove(temporary / L"game"); std::filesystem::remove(temporary);
    std::cout << checks << " private pilot identity/ownership checks passed\n";
}

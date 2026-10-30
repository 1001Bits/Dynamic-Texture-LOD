#pragma once

#include "Fo4MeshNative.h"
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace Fo4Mesh::Pilot {
    // Test-only, never enabled by the shipped INI. The benchmark must provide
    // an absolute manifest path in this environment variable before launch.
    inline constexpr wchar_t kEnvironment[] = L"DTL_MESH_PILOT_CONFIG";
    inline constexpr std::uint64_t kMaximumFixtureBytes = 16ull << 20;
    using Digest = std::array<std::byte, 32>;

    inline std::optional<Digest> ParseDigest(std::wstring_view text) {
        if (text.size() != 64) return std::nullopt;
        auto digit = [](wchar_t value) -> int {
            if (value >= L'0' && value <= L'9') return value - L'0';
            if (value >= L'a' && value <= L'f') return value - L'a' + 10;
            if (value >= L'A' && value <= L'F') return value - L'A' + 10;
            return -1;
        };
        Digest result;
        for (std::size_t i = 0; i < result.size(); ++i) {
            const auto high = digit(text[i * 2]), low = digit(text[i * 2 + 1]);
            if (high < 0 || low < 0) return std::nullopt;
            result[i] = static_cast<std::byte>((high << 4) | low);
        }
        return result;
    }
    inline std::string Hex(const Digest& digest) {
        constexpr char alphabet[] = "0123456789abcdef";
        std::string result(64, '0');
        for (std::size_t i = 0; i < digest.size(); ++i) {
            const auto byte = std::to_integer<unsigned>(digest[i]);
            result[i * 2] = alphabet[byte >> 4]; result[i * 2 + 1] = alphabet[byte & 15];
        }
        return result;
    }
    inline std::optional<Digest> Hash(std::span<const std::byte> bytes) {
        if (bytes.size() > ULONG_MAX) return std::nullopt;
        BCRYPT_ALG_HANDLE algorithm{};
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return std::nullopt;
        struct Close { BCRYPT_ALG_HANDLE value; ~Close() { BCryptCloseAlgorithmProvider(value, 0); } } close{algorithm};
        ULONG objectBytes{}, returned{}, digestBytes{};
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &returned, 0) < 0 ||
            returned != sizeof(objectBytes) || !objectBytes || objectBytes > (1u << 20)) return std::nullopt;
        if (BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digestBytes), sizeof(digestBytes), &returned, 0) < 0 ||
            returned != sizeof(digestBytes) || digestBytes != Digest{}.size()) return std::nullopt;
        // BCryptHash's one-shot API needs a newer Windows target. Keep the
        // product's Windows7 baseline and retain provider-owned object storage
        // until BCryptDestroyHash has completed.
        std::vector<UCHAR> object(objectBytes);
        BCRYPT_HASH_HANDLE hash{};
        if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0) return std::nullopt;
        struct Destroy { BCRYPT_HASH_HANDLE value; ~Destroy() { BCryptDestroyHash(value); } } destroy{hash};
        if (!bytes.empty() && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
                static_cast<ULONG>(bytes.size()), 0) < 0) return std::nullopt;
        Digest result;
        if (BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(result.data()), static_cast<ULONG>(result.size()), 0) < 0)
            return std::nullopt;
        return result;
    }
    struct FileHandle {
        HANDLE value = INVALID_HANDLE_VALUE;
        FileHandle() = default;
        explicit FileHandle(HANDLE handle) : value(handle) {}
        ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        FileHandle(FileHandle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
        FileHandle& operator=(FileHandle&& other) noexcept {
            if (this != &other) {
                if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
                value = std::exchange(other.value, INVALID_HANDLE_VALUE);
            }
            return *this;
        }
        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;
    };
    struct Identity {
        std::filesystem::path manifest, fixture;
        std::string geometryName;
        Digest manifestDigest{}, fixtureDigest{}, verticesDigest{}, indicesDigest{};
        std::uint64_t descriptor{};
        std::uint32_t vertices{}, triangles{};
        std::array<std::uint32_t, 3> segments{};
        // Deny writes/deletion to both inputs for the entire private session.
        FileHandle manifestFile, fixtureFile;

        bool Metadata(std::string_view name, std::uint64_t vertexDescriptor, std::uint32_t vertexCount,
                      std::uint32_t triangleCount, std::array<std::uint32_t, 3> counts) const {
            return name == geometryName && vertexDescriptor == descriptor && vertexCount == vertices &&
                triangleCount == triangles && counts == segments;
        }
        bool Source(SourceView source) const {
            if (source.stride != (descriptor & 15) * 4 || source.vertices.size() != std::uint64_t(vertices) * source.stride ||
                source.indices.size() != std::uint64_t(triangles) * 3) return false;
            const auto vertexHash = Hash(source.vertices), indexHash = Hash(std::as_bytes(source.indices));
            return vertexHash && indexHash && *vertexHash == verticesDigest && *indexHash == indicesDigest;
        }
    };
    inline std::optional<Digest> OpenHashed(const std::filesystem::path& path, FileHandle& handle) {
        FileHandle opened(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr));
        if (opened.value == INVALID_HANDLE_VALUE) return std::nullopt;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(opened.value, &size) || size.QuadPart <= 0 || std::uint64_t(size.QuadPart) > kMaximumFixtureBytes)
            return std::nullopt;
        std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
        DWORD read{};
        if (!ReadFile(opened.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())
            return std::nullopt;
        auto digest = Hash(bytes);
        if (digest) handle = std::move(opened);
        return digest;
    }
    inline std::optional<Identity> Load(const std::filesystem::path& manifest, const std::filesystem::path& gameDirectory) {
        if (!manifest.is_absolute() || !gameDirectory.is_absolute()) return std::nullopt;
        Identity result; result.manifest = manifest;
        auto manifestHash = OpenHashed(manifest, result.manifestFile);
        if (!manifestHash) return std::nullopt;
        auto setting = [&](const wchar_t* key) -> std::wstring {
            std::array<wchar_t, 32768> value{};
            const auto length = GetPrivateProfileStringW(L"Pilot", key, L"", value.data(), static_cast<DWORD>(value.size()), manifest.c_str());
            if (length >= value.size() - 1) return {};
            return {value.data(), length};
        };
        if (setting(L"Mode") != L"SingleFixtureRetainedFull" || setting(L"MaximumRecords") != L"1" ||
            setting(L"NetMemorySavingClaim") != L"0") return std::nullopt;
        auto number = [&](const wchar_t* key, unsigned base) -> std::optional<std::uint64_t> {
            const auto text = setting(key);
            if (text.empty() || text.size() > 16) return std::nullopt;
            std::uint64_t value{};
            for (auto c : text) {
                const auto digit = c >= L'0' && c <= L'9' ? c - L'0' :
                    c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
                if (digit < 0 || static_cast<unsigned>(digit) >= base || value > (UINT64_MAX - digit) / base) return std::nullopt;
                value = value * base + digit;
            }
            return value;
        };
        const auto fixtureDigest = ParseDigest(setting(L"FixtureSHA256")), vertexDigest = ParseDigest(setting(L"VerticesSHA256")),
            indexDigest = ParseDigest(setting(L"IndicesSHA256"));
        const auto descriptor = number(L"DescriptorHex", 16), vertices = number(L"VertexCount", 10), triangles = number(L"TriangleCount", 10);
        if (!fixtureDigest || !vertexDigest || !indexDigest || !descriptor || !vertices || !triangles || !*vertices ||
            *vertices > 65535 || !*triangles || *triangles > (2u << 20) / 6 || !(*descriptor & 15) ||
            !(*descriptor & (1ull << 44)) || (*descriptor & (1ull << 50))) return std::nullopt;
        result.descriptor = *descriptor; result.vertices = static_cast<std::uint32_t>(*vertices);
        result.triangles = static_cast<std::uint32_t>(*triangles);
        const std::array<const wchar_t*, 3> keys{L"Segment0", L"Segment1", L"Segment2"};
        for (unsigned i = 0; i < keys.size(); ++i) {
            const auto count = number(keys[i], 10);
            if (!count || *count > result.triangles) return std::nullopt;
            result.segments[i] = static_cast<std::uint32_t>(*count);
        }
        if (!NativeLODRange(result.segments, 3, result.triangles)) return std::nullopt;
        const auto name = setting(L"GeometryName");
        if (!name.starts_with(L"DTLMeshPilot_") || name.size() > 127) return std::nullopt;
        for (auto character : name) {
            if (character < 32 || character > 126) return std::nullopt;
            result.geometryName += static_cast<char>(character);
        }
        result.fixture = setting(L"FixturePath");
        if (!result.fixture.is_absolute()) return std::nullopt;
        std::error_code error;
        const auto root = std::filesystem::canonical(gameDirectory / L"Data" / L"meshes", error);
        if (error) return std::nullopt;
        result.fixture = std::filesystem::canonical(result.fixture, error);
        if (error) return std::nullopt;
        const auto relative = result.fixture.lexically_relative(root);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == L"..") return std::nullopt;
        const auto actualFixtureHash = OpenHashed(result.fixture, result.fixtureFile);
        if (!actualFixtureHash || *actualFixtureHash != *fixtureDigest) return std::nullopt;
        result.manifestDigest = *manifestHash; result.fixtureDigest = *fixtureDigest;
        result.verticesDigest = *vertexDigest; result.indicesDigest = *indexDigest;
        return result;
    }

    // Owns exactly the native shape supplied by the adapter. No engine calls,
    // heap allocation or reference increments occur in the restoration path.
    // A reduced pair is never admitted without this record's full native pair.
    // This deliberately retains full GPU capacity: it cannot prove net savings.
    class FullPair {
    public:
        FullPair() = default;
        FullPair(const FullPair&) = delete;
        FullPair& operator=(const FullPair&) = delete;
        FullPair(FullPair&& other) noexcept :
            live_(std::exchange(other.live_, nullptr)), owned_(std::exchange(other.owned_, nullptr)),
            fullVertices_(std::exchange(other.fullVertices_, nullptr)), fullIndices_(std::exchange(other.fullIndices_, nullptr)),
            reducedVertices_(std::exchange(other.reducedVertices_, nullptr)), reducedIndices_(std::exchange(other.reducedIndices_, nullptr)),
            fullIdentity_(other.fullIdentity_),reducedIdentity_(other.reducedIdentity_),
            bytes_(std::exchange(other.bytes_, 0)), fullRetained_(std::exchange(other.fullRetained_, false)) {}
        FullPair& operator=(FullPair&&) = delete; // Never silently discard a native owned pair.
        bool Retain(NativeShape& liveReduced, NativeShape& ownedFull) noexcept {
            if (owned_ || &liveReduced == &ownedFull || liveReduced.descriptor != ownedFull.descriptor ||
                !liveReduced.vertices || !liveReduced.indices || !ownedFull.vertices || !ownedFull.indices ||
                liveReduced.vertices == ownedFull.vertices || liveReduced.indices == ownedFull.indices ||
                AtomicValue(ownedFull.references) != 1) return false;
            live_ = &liveReduced; owned_ = &ownedFull;
            fullVertices_ = ownedFull.vertices; fullIndices_ = ownedFull.indices; fullRetained_ = true;
            reducedVertices_ = liveReduced.vertices; reducedIndices_ = liveReduced.indices;
            fullIdentity_=PairIdentity::Capture(ownedFull);reducedIdentity_=PairIdentity::Capture(liveReduced);
            bytes_ = std::uint64_t(fullVertices_->capacity) + fullIndices_->capacity;
            return true;
        }
        bool Restore() noexcept {
            if (!owned_ || !fullRetained_ || owned_->vertices != fullVertices_ || owned_->indices != fullIndices_ ||
                live_->descriptor != fullIdentity_.descriptor || !fullIdentity_.Matches(*owned_)) return false;
            std::swap(live_->vertices, owned_->vertices); std::swap(live_->indices, owned_->indices);
            fullRetained_ = false;
            return true;
        }
        bool UseReduced() noexcept {
            if (!owned_ || fullRetained_ || live_->vertices != fullVertices_ || live_->indices != fullIndices_ ||
                owned_->vertices != reducedVertices_ || owned_->indices != reducedIndices_ ||
                !fullIdentity_.Matches(*live_) || !reducedIdentity_.Matches(*owned_)) return false;
            std::swap(live_->vertices, owned_->vertices); std::swap(live_->indices, owned_->indices);
            fullRetained_ = true;
            return true;
        }
        NativeShape* Owned() const noexcept { return owned_; }
        bool HasFullSource() const noexcept { return owned_ && fullRetained_; }
        bool FullIsLive() const noexcept {
            return owned_ && !fullRetained_ && live_->vertices == fullVertices_ && live_->indices == fullIndices_ && fullIdentity_.Matches(*live_);
        }
        std::uint64_t FullCapacity() const noexcept { return HasFullSource() ? bytes_ : 0; }
        std::uint64_t InactiveCapacity() const noexcept {
            return owned_ ? std::uint64_t(owned_->vertices->capacity) + owned_->indices->capacity : 0;
        }
        // Caller must first queue the required GPU completion event, or retain
        // the returned pair until native process teardown. Restore alone never
        // establishes that the old reduced buffers are no longer in GPU use.
        NativeShape* TakeRestored() noexcept {
            if (fullRetained_) return nullptr;
            live_ = nullptr; fullVertices_ = fullIndices_ = reducedVertices_ = reducedIndices_ = nullptr; bytes_ = 0;
            return std::exchange(owned_, nullptr);
        }
        NativeShape* TakeForOwnerDestruction() noexcept {
            live_ = nullptr; fullVertices_ = fullIndices_ = reducedVertices_ = reducedIndices_ = nullptr; bytes_ = 0; fullRetained_ = false;
            return std::exchange(owned_, nullptr);
        }
    private:
        NativeShape* live_{}; NativeShape* owned_{};
        NativeBuffer* fullVertices_{}; NativeBuffer* fullIndices_{};
        NativeBuffer* reducedVertices_{}; NativeBuffer* reducedIndices_{};
        PairIdentity fullIdentity_,reducedIdentity_;
        std::uint64_t bytes_{}; bool fullRetained_{};
    };
}

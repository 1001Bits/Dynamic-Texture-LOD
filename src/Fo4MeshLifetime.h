#pragma once

#include <d3d11.h>
#include <array>
#include <cstddef>
#include <cstdint>

// Diagnostic only: live integration remains off unless explicitly enabled.
// A private-data interface's final Release can mean either device-child
// destruction OR private-data overwrite/removal. Neither physical destruction
// nor freed VidMm bytes is inferred here. The module/state must remain loaded
// for the process lifetime; Enable pins its module before issuing any tokens.
// Tokens intentionally have no resource backreference.
// Scope is D3D11 buffers. Query/view objects, driver bookkeeping, allocation
// padding and CPU source storage are not byte-quantified by this observer.
// Include their separate/unknown costs when reporting a resource balance;
// no sum from this API is a complete physical-memory or net-VRAM figure.
namespace Fo4Mesh::Lifetime {
    inline constexpr std::size_t MaximumResources=512;
    inline constexpr std::size_t MaximumEvents=MaximumResources*2;
    inline constexpr GUID PrivateGuid{0x9be591a7,0xba41,0x4d31,{0xb7,0x5e,0x91,0x3a,0x08,0xec,0x6b,0x72}};

    enum class Role : std::uint32_t {
        NativeHeap=1u<<0,
        DedicatedMesh=1u<<1,
        Candidate=1u<<2,
        DrawReserve=1u<<3,
        SourceStaging=1u<<4,
        OtherAuxiliary=1u<<5
    };
    inline constexpr std::size_t RoleCount=6;
    constexpr Role operator|(Role a,Role b) noexcept {
        return static_cast<Role>(static_cast<std::uint32_t>(a)|static_cast<std::uint32_t>(b));
    }
    enum class Result : std::uint8_t {
        Attached, AlreadyObservedUnverified, Disabled, Teardown, InvalidResource,
        IdentityFailure, PrivateDataOccupied, PrivateDataQueryFailed,
        Capacity, AttachFailed, IdentityConflict, InternalFailure
    };
    struct Attachment {
        Result result{Result::Disabled};
        HRESULT hresult{S_FALSE};
        // An observer attachment generation, not a proven native-resource
        // birth. Address reuse and private-data lifetime are separate facts.
        std::uint64_t generation{};
        std::uint64_t resourceIdentity{};
        std::uint32_t byteWidth{};
    };
    enum class EventKind : std::uint8_t {
        Attachment, ObservationUpdated, AttachmentFailure, PrivateDataLifetimeNotification
    };
    struct Event {
        EventKind kind{};
        std::uint64_t sequence{},generation{},resourceIdentity{},ownerEpoch{},qpc{};
        std::uint32_t byteWidth{},roles{};
        // Nominal D3D buffer size/type. ByteWidth does not expose driver
        // allocation alignment, system/local placement or physical residency.
        D3D11_BUFFER_DESC descriptor{};
        // A repeated canonical address plus occupied pointer-sized private
        // data cannot prove that THIS token is still stored there. Foreign
        // replacement or address reuse while an external token ref survives
        // remains possible. ByteWidth dedup after this point is conditional.
        bool identityMembershipUnverified{};
        HRESULT hresult{S_OK};
        // True only after the caller explicitly marked process teardown.
        // False does not certify that process teardown has not begun.
        bool duringProcessTeardown{};
        // These are deliberately always false. A downstream claim requires
        // independent evidence, not a reinterpretation of this notification.
        bool privateDataOverwriteRuledOut{};
        bool physicalDestructionProven{};
    };
    struct Statistics {
        bool enabled{},processTeardown{},modulePinned{};
        std::uint32_t capacity{};
        std::uint64_t attempts{},attachments{},deduplicated{},attachmentFailures{};
        std::uint64_t identityUnverifiedObservations{};
        std::uint64_t capacityFailures{},identityConflicts{},internalFailures{};
        std::uint64_t addedByteWidth{},lifetimeNotifications{},notifiedByteWidth{};
        std::uint64_t teardownNotifications{},teardownNotifiedByteWidth{};
        std::uint64_t unnotifiedResources{},unnotifiedByteWidth{},notificationProtocolErrors{};
        // Failure counters remain available when no event slot can be kept.
        // Capacity is a coverage loss, not evidence that no resource existed.
        std::uint64_t unrecordedAttachmentFailures{};
        std::array<std::uint64_t,RoleCount> roleAttempts{},roleFailures{};
        // Counts all attached resources/roles, including auxiliary allocations.
        // Roles may overlap; never add per-role byte totals as distinct bytes.
        // unnotifiedByteWidth is NOT live allocation, residency or net savings.
    };
    struct Snapshot {
        Statistics statistics;
        std::uint64_t qpcFrequency{};
        std::uint64_t drainBeginQpc{},drainEndQpc{},sequenceBegin{},sequenceEnd{};
        // Callback counters are individually atomic, not one transactional
        // sample. Use the drained events for accounting/reconciliation; a
        // callback published during this drain can appear in the next one.
        bool concurrentCounterSnapshot{true};
        std::size_t count{};
        std::array<Event,MaximumEvents> events;
    };

    // First enable pins this executable/DLL with GetModuleHandleEx(FROM_ADDRESS
    // | PIN), then creates a fixed process-lifetime token pool. Failure to pin
    // refuses enable. Pinning is intentionally irreversible for this process.
    // Later calls may re-enable the same capacity; changing it is rejected.
    // Default off. State and callback code both survive process teardown order.
    bool Enable(std::uint32_t capacity=MaximumResources) noexcept;
    bool Enabled() noexcept; // cheap atomic check; performs no COM access
    void Disable() noexcept; // stops attachment only; existing tokens keep reporting
    void BeginProcessTeardown() noexcept;

    // Caller owns a valid buffer throughout this call and uses a legal engine
    // lifetime/thread boundary. GetDesc supplies actual ByteWidth. QueryInterface
    // is used briefly for canonical IUnknown identity; no reference is retained.
    // An occupied GUID is rejected; this module never intentionally overwrites
    // or clears private data. An unrelated concurrent writer cannot be ruled
    // out by a check-then-set API. Repeated address/size observations return
    // AlreadyObservedUnverified, merge roles and mark coverage uncertainty;
    // this NEVER certifies current token membership. External retention/removal of this
    // private interface can delay notification past native-resource lifetime.
    // Therefore even an attachment generation is diagnostic, not proof that
    // all native resource incarnations were observed. The caller must retain
    // each returned failure: absent/failed attachment means missing coverage.
    Attachment Attach(ID3D11Buffer*,Role,std::uint64_t ownerEpoch=0) noexcept;

    // Nonblocking snapshot drain. False means busy/unavailable; no events were
    // consumed. Only this consumer may drain; keep every successful snapshot.
    // Role changes between drains are coalesced to the latest role union/time.
    // Callback-side final Release does no allocation, locking, disk/engine work
    // or COM access. Snapshots never query or dereference released resources.
    bool Drain(Snapshot&) noexcept;
}

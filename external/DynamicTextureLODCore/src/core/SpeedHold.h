#pragma once

#include <cmath>
#include <cstdint>

// [Smoothing] UpgradeHoldSpeed: no raises while the camera moves fast, as the
// game holds its own above fTextureUpgradeDisabledMinSpeed. What is raised in
// flight is soon left behind, and its file reads compete with the loading the
// movement itself causes. Pure, for tests; the backend feeds it the camera
// position from its planner thread.
namespace dtl
{
    class SpeedHold
    {
    public:
        static constexpr std::uint64_t kWindowMs = 250;   // speed measured over at least this
        static constexpr std::uint64_t kReleaseMs = 1000; // this long slower before raises return
        static constexpr std::uint64_t kStaleMs = 2000;   // no camera for this long: no hold
        static constexpr float         kJumpUnits = 4096; // more within one window is a teleport

        // a_limit in game units per second; 0 never holds.
        bool Update(float a_x, float a_y, float a_z, std::uint64_t a_nowMs, std::uint32_t a_limit)
        {
            if (a_limit == 0 || !std::isfinite(a_x) || !std::isfinite(a_y) || !std::isfinite(a_z)) {
                Reset();
                return false;
            }
            if (!_anchored || a_nowMs < _anchorMs || a_nowMs - _anchorMs > kStaleMs) {
                Anchor(a_x, a_y, a_z, a_nowMs);
                _holding = false;
                return false;
            }
            const auto elapsed = a_nowMs - _anchorMs;
            if (elapsed < kWindowMs) return _holding;
            const float dx = a_x - _x, dy = a_y - _y, dz = a_z - _z;
            const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
            Anchor(a_x, a_y, a_z, a_nowMs);
            // Fast travel, a loading door or a respawn: no speed, no new hold.
            if (moved > kJumpUnits) {
                _holding = false;
                return false;
            }
            _speed = moved * 1000.0f / static_cast<float>(elapsed);
            if (_speed >= static_cast<float>(a_limit)) {
                _holding = true;
                _fastMs = a_nowMs;
            } else if (_holding && a_nowMs - _fastMs >= kReleaseMs) {
                _holding = false;
            }
            return _holding;
        }

        void Reset()
        {
            _anchored = false;
            _holding = false;
            _speed = 0;
        }

        bool  Holding() const { return _holding; }
        float Speed() const { return _speed; }

    private:
        void Anchor(float a_x, float a_y, float a_z, std::uint64_t a_nowMs)
        {
            _x = a_x;
            _y = a_y;
            _z = a_z;
            _anchorMs = a_nowMs;
            _anchored = true;
        }

        float         _x = 0, _y = 0, _z = 0;
        float         _speed = 0;
        std::uint64_t _anchorMs = 0, _fastMs = 0;
        bool          _anchored = false, _holding = false;
    };
}

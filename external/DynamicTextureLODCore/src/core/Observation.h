#pragma once

#include <algorithm>
#include <cstdint>

namespace dtl
{
    // Largest observation in the active engine round and its predecessor.
    // A round advances only after the engine finishes its entire traversal;
    // elapsed wall time cannot turn an unvisited object into an unseen one.
    struct Observation
    {
        std::uint64_t round = 0;
        std::uint64_t lastVisit = 0;
        std::uint64_t lastSeen = 0;
        float pixels[2]{};
        float priority[2]{};
        // Every visit, in view or not: what its size at its distance asks for.
        // Planning by distance uses these, so turning around changes nothing.
        float reachPixels[2]{};
        float reach[2]{};

        void Advance(std::uint64_t next)
        {
            if (next == round) {
                return;
            }
            const bool adjacent = next == round + 1;
            pixels[1] = adjacent ? pixels[0] : 0.0f;
            priority[1] = adjacent ? priority[0] : 0.0f;
            reachPixels[1] = adjacent ? reachPixels[0] : 0.0f;
            reach[1] = adjacent ? reach[0] : 0.0f;
            pixels[0] = priority[0] = reachPixels[0] = reach[0] = 0.0f;
            round = next;
        }

        void Visit(std::uint64_t now, bool visible, float size, float rank)
        {
            Advance(now);
            lastVisit = now;
            reachPixels[0] = std::max(reachPixels[0], size);
            reach[0] = std::max(reach[0], rank);
            if (visible) {
                lastSeen = now;
                pixels[0] = std::max(pixels[0], size);
                priority[0] = std::max(priority[0], rank);
            }
        }

        bool Seen(std::uint64_t now) const { return lastSeen != 0 && now - lastSeen <= 1; }
        float Pixels() const { return std::max(pixels[0], pixels[1]); }
        float Priority() const { return std::max(priority[0], priority[1]); }
        float ReachPixels() const { return std::max(reachPixels[0], reachPixels[1]); }
        float Reach() const { return std::max(reach[0], reach[1]); }
    };
}

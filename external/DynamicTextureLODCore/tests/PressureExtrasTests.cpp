// [MaxSize] path rules, [MipSkip], the speed hold and the reserve from
// measured jumps. Pure, no game.
#include "core/ReserveMeter.h"
#include "core/SpeedHold.h"
#include "core/TextureCap.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace
{
    int checks = 0;
    void Check(bool a_value, const char* a_message)
    {
        ++checks;
        if (!a_value) throw std::runtime_error(a_message);
    }
    constexpr std::uint64_t MiB(std::uint64_t a_value) { return a_value << 20; }

    void Caps()
    {
        using namespace dtl::cap;
        Check(Normalize("Textures/Architecture/Wall_D.DDS") == "textures\\architecture\\wall_d.dds", "lower case, backslashes");
        Check(KindOf(Normalize("Textures\\SetDressing\\Shelving\\GenericShelving_n.DDS")) == Kind::Normal, "normal map");
        Check(KindOf("textures\\a\\b_msn.dds") == Kind::Normal, "model-space normal map");
        Check(KindOf("textures\\a\\b_s.dds") == Kind::Specular, "specular map");
        Check(KindOf("textures\\a\\b_g.dds") == Kind::Glow, "glow map");
        Check(KindOf("textures\\a\\b_d.dds") == Kind::Diffuse, "diffuse map");
        Check(KindOf("textures\\a\\b_em.dds") == Kind::Other && KindOf("textures\\a\\b_h.dds") == Kind::Other, "masks are other");
        Check(KindOf("textures\\a\\plain.dds") == Kind::Diffuse && KindOf("textures\\a_n\\plain.dds") == Kind::Diffuse,
            "no ending is diffuse; folders do not count");
        Check(KindOf("textures\\a\\b_color.dds") == Kind::Diffuse, "unknown ending is diffuse");

        Check(!ParseRule("", 512) && !ParseRule("Shiny:door", 512) && !ParseRule("Normal:", 512), "empty or unknown is no rule");
        const auto any = ParseRule("\\Interface\\", 0);
        Check(any && any->kind == -1 && any->text == "\\interface\\" && any->edge == 0, "a rule for every kind");
        const auto normal = ParseRule("normal:Door", 1024);
        Check(normal && normal->kind == static_cast<std::int8_t>(Kind::Normal) && normal->text == "door", "a rule for one kind");

        Limits limits;
        Check(!limits.Any(), "nothing set: no caps");
        limits.rules.push_back(*ParseRule("\\actors\\", 0));
        Check(!limits.Any(), "a full-size rule alone caps nothing");
        limits.edge[static_cast<std::size_t>(Kind::Diffuse)] = 1024;
        limits.edge[static_cast<std::size_t>(Kind::Normal)] = 2048;
        limits.rules.push_back(*ParseRule("Normal:\\landscape\\", 512));
        limits.rules.push_back(*ParseRule("\\landscape\\", 256));
        Check(limits.Any(), "a kind's size caps");
        Check(EdgeFor(limits, "textures\\architecture\\wall_d.dds") == 1024, "kind limit");
        Check(EdgeFor(limits, "textures\\architecture\\wall_n.dds") == 2048, "per kind");
        Check(EdgeFor(limits, "textures\\architecture\\wall_s.dds") == 0, "unset kind: full size");
        Check(EdgeFor(limits, "textures\\actors\\character\\face_d.dds") == 0, "a full-size rule overrides the kind");
        Check(EdgeFor(limits, "textures\\landscape\\dirt_n.dds") == 512, "first match wins, by kind");
        Check(EdgeFor(limits, "textures\\landscape\\dirt_d.dds") == 256, "a kind rule skips other kinds");

        Check(LevelsFor(0, 4096, 4096) == 0, "no limit, no levels");
        Check(LevelsFor(1024, 4096, 4096) == 2 && LevelsFor(1024, 1024, 1024) == 0, "power of two");
        Check(LevelsFor(1024, 4096, 512) == 2, "the longer edge decides");
        Check(LevelsFor(1000, 2048, 2048) == 2 && LevelsFor(1, 4, 4) == 2, "rounds to fit");
    }

    void MipSkip()
    {
        using namespace dtl::cap;
        Check(FormatOf(71) == Format::BC1 && FormatOf(72) == Format::BC1, "BC1");
        Check(FormatOf(74) == Format::BC3 && FormatOf(77) == Format::BC3, "BC2 counts as BC3");
        Check(FormatOf(80) == Format::BC4 && FormatOf(83) == Format::BC5 && FormatOf(98) == Format::BC7, "BC4, BC5, BC7");
        Check(FormatOf(95) == Format::Other && FormatOf(87) == Format::Other && FormatOf(28) == Format::Other,
            "BC6H and uncompressed are other");

        Skip skip;
        Check(!skip.Any(), "nothing set: no skip");
        skip.levels[static_cast<std::size_t>(Format::BC1)] = 1;
        skip.levels[static_cast<std::size_t>(Format::BC3)] = 2;
        Check(skip.Any(), "a format's skip");
        Check(SkipLevelsFor(skip, 71, 4096, 4096) == 1, "BC1 4096 loses one");
        Check(SkipLevelsFor(skip, 77, 4096, 2048) == 2, "BC3 4096 loses two, down to 1024");
        Check(SkipLevelsFor(skip, 77, 2048, 2048) == 1, "never below the minimum");
        Check(SkipLevelsFor(skip, 71, 1024, 1024) == 0 && SkipLevelsFor(skip, 71, 512, 512) == 0,
            "at or under the minimum: untouched");
        Check(SkipLevelsFor(skip, 83, 4096, 4096) == 0, "BC5 normal maps keep their detail");
        skip.minDimension = 256;
        Check(SkipLevelsFor(skip, 77, 1024, 512) == 2, "a lower minimum allows more");

        Limits limits;
        limits.skip = skip;
        Check(limits.Any(), "a skip alone is a cap");
        limits.edge[static_cast<std::size_t>(Kind::Diffuse)] = 512;
        Check(CapLevels(limits, "textures\\a\\b_d.dds", 71, 2048, 2048) == 2, "the size limit is stronger");
        Check(CapLevels(limits, "textures\\a\\b_n.dds", 77, 2048, 2048) == 2, "the skip is stronger");
        Check(CapLevels(limits, "textures\\a\\b_n.dds", 83, 2048, 2048) == 0, "neither applies");
    }

    void Speed()
    {
        dtl::SpeedHold hold;
        Check(!hold.Update(0, 0, 0, 1000, 600), "first reading only anchors");
        // Walking, 300 units a second.
        std::uint64_t t = 1000;
        float x = 0;
        for (int i = 0; i < 20; ++i) Check(!hold.Update(x += 30, 0, 0, t += 100, 600), "walking never holds");
        // A vertibird, 1200 units a second.
        bool held = false;
        for (int i = 0; i < 5; ++i) held = hold.Update(x += 120, 0, 0, t += 100, 600);
        Check(held && hold.Holding(), "flying holds raises");
        // Slow again: held for another second, then released.
        Check(hold.Update(x += 10, 0, 0, t += 300, 600), "still held right after");
        for (int i = 0; i < 4; ++i) hold.Update(x += 10, 0, 0, t += 300, 600);
        Check(!hold.Holding(), "released after a second of slow movement");
        // A fast-travel jump is no speed.
        Check(!hold.Update(x += 50000, 0, 0, t += 300, 600), "a teleport never holds");
        // Off, or no reading for a while.
        Check(!hold.Update(x += 1000, 0, 0, t += 300, 0), "0 is off");
        hold.Update(x, 0, 0, t += 100, 600);
        Check(!hold.Update(x += 5000, 0, 0, t += 5000, 600), "a stale anchor starts over");
    }

    void Meter()
    {
        using dtl::ReserveMeter;
        ReserveMeter meter;
        // A steady scene: no jump.
        std::int64_t t = 100000;
        for (int i = 0; i < 40; ++i, t += 100) meter.Add(t, MiB(2000));
        Check(meter.Largest() == 0, "steady usage is no jump");
        // A door: 700 MB in a second and a half.
        for (int i = 0; i < 15; ++i, t += 100) meter.Add(t, MiB(2000 + 700 * (i + 1) / 15));
        Check(meter.Largest() == MiB(700), "rise over the 3 s low");
        // A slow climb over a minute is no jump.
        ReserveMeter slow;
        for (int i = 0; i < 600; ++i) slow.Add(200000 + i * 100, MiB(2000 + i));
        Check(slow.Largest() < MiB(40), "a slow climb measures only its last 3 s");
        // A load in between: nothing measured across it.
        ReserveMeter load;
        load.Add(100000, MiB(500));
        load.Break();
        load.Add(100100, MiB(2500));
        Check(load.Largest() == 0, "no jump across a break");
        // Play time: at most a second between readings, none across a break.
        ReserveMeter play;
        play.Add(0, MiB(1));
        play.Add(500, MiB(1));
        play.Add(60500, MiB(1));
        Check(play.PlayMs() == 1500, "a long gap adds a second");
        play.Break();
        play.Add(70000, MiB(1));
        Check(play.PlayMs() == 1500 && !play.Ready(), "a break adds nothing");
        for (std::int64_t at = 70000; at <= 70000 + 180000; at += 500) play.Add(at, MiB(1));
        Check(play.Ready(), "three minutes of play");
        // Early buckets: an unused bucket is no low reading.
        ReserveMeter early;
        early.Add(10, MiB(1000));
        early.Add(20, MiB(1100));
        Check(early.Largest() == MiB(100), "early readings");

        // The largest jump plus a quarter, in 256 MB steps, 512 MB to 6 GB,
        // at most a quarter of the budget.
        Check(ReserveMeter::KeepFree(0, 0) == MiB(512), "at least 512 MB");
        Check(ReserveMeter::KeepFree(MiB(700), 0) == MiB(1024), "700 MB plus a quarter rounds to 1 GB");
        Check(ReserveMeter::KeepFree(MiB(1500), 0) == MiB(2048), "1.5 GB plus a quarter rounds to 2 GB");
        Check(ReserveMeter::KeepFree(MiB(9000), 0) == MiB(6144), "at most 6 GB");
        Check(ReserveMeter::KeepFree(MiB(1500), MiB(7180)) == MiB(1795), "at most a quarter of the budget");
    }
}

int main()
{
    try {
        Caps();
        MipSkip();
        Speed();
        Meter();
        std::cout << checks << " size cap, mip skip, speed hold and reserve checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

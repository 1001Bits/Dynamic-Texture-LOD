#include "GpuProcessMemory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>
#include <Pdh.h>
#include <PdhMsg.h>

#pragma comment(lib, "pdh.lib")

namespace dtl::gpumem
{
    namespace
    {
        constexpr std::int64_t kHistoryMs = 30000;

        std::atomic<bool> g_started{ false };
        std::mutex        g_lock;
        std::deque<Sample> g_history;  // oldest first, at most kHistoryMs

        std::int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // Sums the instances of one counter that belong to this process:
        // "pid_<pid>_luid_..._phys_N", one per adapter it touches.
        bool SumOwn(PDH_HCOUNTER a_counter, std::wstring_view a_prefix, std::vector<std::byte>& a_buffer,
            std::uint64_t& a_out)
        {
            DWORD bytes = 0, count = 0;
            auto status = PdhGetFormattedCounterArrayW(a_counter, PDH_FMT_LARGE, &bytes, &count, nullptr);
            a_out = 0;
            if (status == ERROR_SUCCESS) return true;  // no instances at all
            if (status != static_cast<PDH_STATUS>(PDH_MORE_DATA)) return false;
            a_buffer.resize(bytes);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(a_buffer.data());
            if (PdhGetFormattedCounterArrayW(a_counter, PDH_FMT_LARGE, &bytes, &count, items) != ERROR_SUCCESS)
                return false;
            for (DWORD i = 0; i < count; ++i) {
                if ((items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
                        items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) && items[i].szName &&
                    std::wstring_view(items[i].szName).starts_with(a_prefix) && items[i].FmtValue.largeValue > 0)
                    a_out += static_cast<std::uint64_t>(items[i].FmtValue.largeValue);
            }
            return true;
        }

        void Run()
        {
            PDH_HQUERY query{};
            PDH_HCOUNTER dedicated{}, shared{};
            if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS ||
                PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &dedicated) != ERROR_SUCCESS ||
                PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Shared Usage", 0, &shared) != ERROR_SUCCESS) {
                if (query) PdhCloseQuery(query);
                spdlog::warn("GPU process memory counters are unavailable; video memory use comes from DXGI only");
                return;
            }
            const auto prefix = std::format(L"pid_{}_", GetCurrentProcessId());
            std::vector<std::byte> buffer;
            bool logged = false;
            // Held for good: this thread lives as long as the process.
            for (;; std::this_thread::sleep_for(std::chrono::seconds(1))) {
                Sample sample;
                sample.valid = PdhCollectQueryData(query) == ERROR_SUCCESS &&
                    SumOwn(dedicated, prefix, buffer, sample.dedicated) && SumOwn(shared, prefix, buffer, sample.shared);
                sample.atMs = NowMs();
                if (sample.valid && !logged && sample.dedicated) {
                    logged = true;
                    spdlog::info("GPU process memory: {} MB dedicated, {} MB shared", sample.dedicated >> 20,
                        sample.shared >> 20);
                }
                std::scoped_lock lock(g_lock);
                g_history.push_back(sample);
                while (!g_history.empty() && sample.atMs - g_history.front().atMs > kHistoryMs) g_history.pop_front();
            }
        }
    }

    void Start()
    {
        if (g_started.exchange(true)) return;
        std::thread(Run).detach();
    }

    Sample Latest()
    {
        std::scoped_lock lock(g_lock);
        for (auto it = g_history.rbegin(); it != g_history.rend(); ++it)
            if (it->valid) return *it;
        return {};
    }

    bool SharedRising(std::uint64_t a_minRise, std::int64_t a_windowMs)
    {
        return a_minRise > 0 && SharedRise(a_windowMs) >= a_minRise;
    }

    std::uint64_t SharedRise(std::int64_t a_windowMs)
    {
        std::scoped_lock lock(g_lock);
        const std::vector<Sample> history(g_history.begin(), g_history.end());
        return Rise(history, a_windowMs);
    }
}

#pragma once

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>

namespace logger = F4SE::log;

using namespace std::literals;

#define DLLEXPORT __declspec(dllexport)

#include "Plugin.h"

#include "Logging.h"

void OpenLog()
{
    auto path = logger::log_directory();
    if (!path) {
        return;
    }
    *path /= fmt::format("{}.log", Plugin::NAME);

    auto log = std::make_shared<spdlog::logger>("global",
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true));
    log->set_pattern("[%H:%M:%S.%e] [%l] %v");
    log->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(log));

    logger::info("{} {}", Plugin::NAME, Plugin::VERSION.string("."sv));
}

#pragma once

#include "RE/Skyrim.h"
#include "REL/Relocation.h"
#include "SKSE/Logger.h"
#include "SKSE/SKSE.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace std::literals;

namespace logger = SKSE::log;

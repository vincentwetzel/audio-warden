#pragma once

#include <filesystem>

#include <nlohmann/json_fwd.hpp>

void scan_library(const std::filesystem::path& library_path, const nlohmann::json& rules, bool is_dry_run, bool interactive);

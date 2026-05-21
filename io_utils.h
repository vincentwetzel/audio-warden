#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

std::string path_to_utf8(const std::filesystem::path& p);
std::filesystem::path utf8_to_path(const std::string& utf8_str);
std::vector<std::string> load_library_paths(const std::filesystem::path& file_path);
nlohmann::json load_json(const std::filesystem::path& file_path);
void replace_all(std::string& str, const std::string& from, const std::string& to);

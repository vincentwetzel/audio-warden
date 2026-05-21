#include "io_utils.h"

#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string path_to_utf8(const fs::path& p) {
    auto u8str = p.u8string();
    return std::string(u8str.begin(), u8str.end());
}

fs::path utf8_to_path(const std::string& utf8_str) {
#if defined(__cpp_lib_char8_t)
    return fs::path(std::u8string(utf8_str.begin(), utf8_str.end()));
#elif __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
    return fs::path(reinterpret_cast<const char8_t*>(utf8_str.c_str()));
#else
    return fs::u8path(utf8_str);
#endif
}

std::vector<std::string> load_library_paths(const fs::path& file_path) {
    if (!fs::exists(file_path)) {
        throw std::runtime_error("Settings file does not exist: " + path_to_utf8(file_path));
    }

    std::ifstream file(file_path);
    std::vector<std::string> paths;
    std::string line;
    while (std::getline(file, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (!line.empty() && line[0] != '#') {
            paths.push_back(line);
        }
    }
    return paths;
}

json load_json(const fs::path& file_path) {
    if (!fs::exists(file_path)) {
        throw std::runtime_error("File does not exist: " + path_to_utf8(file_path));
    }

    std::ifstream file(file_path);
    json data;
    file >> data;
    return data;
}

void replace_all(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

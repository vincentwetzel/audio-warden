#include <iostream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <taglib/tdebuglistener.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "app.h"
#include "io_utils.h"
#include "tag_debug.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    AudioWardenDebugListener debug_listener;
    TagLib::setDebugListener(&debug_listener);

    CLI::App app{"AudioWarden - Audio Library Organizer"};

    std::string rules_path;
    bool interactive = false;
    bool dry_run = false;

    app.add_option("-r,--rules", rules_path, "Path to the JSON/YAML file containing your rules")->required();
    app.add_flag("-i,--interactive", interactive, "Prompt the user for confirmation before renaming/retagging");
    app.add_flag("-d,--dry-run", dry_run, "Print out proposed changes without modifying files");

    CLI11_PARSE(app, argc, argv);

    if (dry_run) std::cout << "[DRY RUN MODE ENABLED] No files will be modified.\n";

    try {
        json rules = load_json(rules_path);
        
        fs::path settings_path = "settings.txt";
        std::vector<std::string> library_paths = load_library_paths(settings_path);
        
        if (library_paths.empty()) {
            throw std::runtime_error("Invalid settings.txt: No directories specified.");
        }
        
        for (const auto& path : library_paths) {
            scan_library(utf8_to_path(path), rules, dry_run, interactive);
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

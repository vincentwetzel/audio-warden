#include "app.h"

#include <atomic>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "actions.h"
#include "io_utils.h"
#include "models.h"
#include "prompter.h"
#include "scanner.h"
#include "thread_safe_queue.h"
#include "validator.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

void scan_library(const fs::path& library_path, const json& rules, bool is_dry_run, bool interactive) {
    if (!fs::exists(library_path) || !fs::is_directory(library_path)) {
        std::cerr << "Error: Library path does not exist or is not a directory.\n";
        return;
    }

    std::cout << "Starting background scan of: " << path_to_utf8(library_path) << "\n\n";

    std::vector<std::string> category_folders = rules.value(
        "category_folders",
        std::vector<std::string>{"soundtracks", "childrens", "various artists"});
    for (auto& folder : category_folders) {
        std::transform(folder.begin(), folder.end(), folder.begin(), ::tolower);
    }

    std::vector<std::string> standalone_folders = rules.value(
        "standalone_folders",
        std::vector<std::string>{"singles", "loose tracks", "standalone"});
    for (auto& folder : standalone_folders) {
        std::transform(folder.begin(), folder.end(), folder.begin(), ::tolower);
    }

    ThreadSafeQueue<fs::path> album_folders_queue;
    ThreadSafeQueue<Album> albums_queue;

    std::thread discovery_thread([&]() {
        try {
            discover_album_folders(library_path, album_folders_queue);
        } catch (const std::exception& e) {
            std::cerr << "FATAL: Unhandled exception in discovery thread: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "FATAL: Unknown unhandled exception in discovery thread." << std::endl;
        }
        album_folders_queue.finish();
    });

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    std::vector<std::thread> workers;
    std::atomic<int> active_workers{static_cast<int>(num_threads)};
    for (unsigned int i = 0; i < num_threads; ++i) {
        workers.emplace_back([&, i]() {
            try {
                fs::path album_path;
                while (album_folders_queue.pop(album_path)) {
                    try {
                        Album album = parse_album_folder(album_path, category_folders, standalone_folders);
                        if (!album.tracks.empty()) albums_queue.push(std::move(album));
                    } catch (const fs::filesystem_error& e) {
                        std::cerr << "Warning: Filesystem error processing album "
                                  << path_to_utf8(album_path) << ": " << e.what() << std::endl;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "FATAL: Unhandled exception in worker thread " << i << ": " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "FATAL: Unknown unhandled exception in worker thread " << i << "." << std::endl;
            }

            if (--active_workers == 0) albums_queue.finish();
        });
    }

    std::cout << "Scan in progress. Presenting violations as they are found...\n\n";

    int processed_album_count = 0;
    Album current_album;
    std::vector<Album> all_albums;
    while (albums_queue.pop(current_album)) {
        if (current_album.tracks.empty()) continue;
        processed_album_count++;

        FolderValidationResult folder_result = validate_album_folder(current_album);
        prompt_folder_actions(current_album, folder_result, is_dry_run, interactive);
        all_albums.push_back(std::move(current_album));
    }

    std::cout << "Folder pass complete. Presenting track violations...\n\n";

    std::string file_naming_rule = rules.value("file_naming", "{TrackNumber} - {Title}.{ext}");
    for (auto& album : all_albums) {
        TrackValidationResult track_result = validate_album_tracks(album, file_naming_rule);
        prompt_track_actions(album.folder_path, track_result, is_dry_run, interactive);
    }

    discovery_thread.join();
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }

    std::cout << "\nTotal albums processed: " << processed_album_count << "\n\n";
}

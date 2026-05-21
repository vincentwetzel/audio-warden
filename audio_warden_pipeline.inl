#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <map>
#include <set>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tdebuglistener.h>
#include <taglib/flacfile.h>
#include <taglib/flacproperties.h>

#include "tag_debug.h"
#include "models.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

thread_local bool t_has_legacy_frames = false;

// Helper to safely convert path to UTF-8 std::string for C++17/C++20 compatibility
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

// Helper to load library paths from a plain text file
std::vector<std::string> load_library_paths(const fs::path& file_path) {
    if (!fs::exists(file_path)) {
        throw std::runtime_error("Settings file does not exist: " + path_to_utf8(file_path));
    }
    std::ifstream file(file_path);
    std::vector<std::string> paths;
    std::string line;
    while (std::getline(file, line)) {
        // Trim whitespace and handle carriage returns
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        if (!line.empty() && line[0] != '#') {
            paths.push_back(line);
        }
    }
    return paths;
}

// Helper to load JSON files
json load_json(const fs::path& file_path) {
    if (!fs::exists(file_path)) {
        throw std::runtime_error("File does not exist: " + path_to_utf8(file_path));
    }
    std::ifstream file(file_path);
    json data;
    file >> data;
    return data;
}

// A thread-safe queue to pass data between background scanners and the main thread
template <typename T>
class ThreadSafeQueue {
private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool finished_ = false;

public:
    void push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(item));
        cond_.notify_one();
    }

    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this]() { return !queue_.empty() || finished_; });
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
        cond_.notify_all();
    }
};

// Function to recursively scan the audio directory
void scan_library(const fs::path& library_path, const json& rules, bool is_dry_run, bool interactive) {
    if (!fs::exists(library_path) || !fs::is_directory(library_path)) {
        std::cerr << "Error: Library path does not exist or is not a directory.\n";
        return;
    }

    std::cout << "Starting background scan of: " << path_to_utf8(library_path) << "\n\n";

    ThreadSafeQueue<fs::path> album_folders_queue;
    ThreadSafeQueue<Album> albums_queue;
    
    // 1. Discovery Thread: Finds album folders and pushes them to a queue.
    // This is more complex than a simple file iterator because we need to identify albums
    // correctly (e.g., multi-disc albums) and process them as single units.
    std::thread discovery_thread([&]() {
        try {
            std::vector<fs::path> all_dirs;
            auto dir_options = fs::directory_options::skip_permission_denied;
            try {
                for (const auto& entry : fs::recursive_directory_iterator(library_path, dir_options)) {
                    if (entry.is_directory()) {
                        all_dirs.push_back(entry.path());
                    }
                }
            } catch (const fs::filesystem_error& e) {
                std::cerr << "Warning: Filesystem error during discovery: " << e.what() << std::endl;
            }
            all_dirs.push_back(library_path);

            // Sort alphabetically. This generally ensures that parent directories are processed
            // before their children, which is crucial for correctly identifying multi-disc albums.
            std::sort(all_dirs.begin(), all_dirs.end());

            std::set<fs::path> processed_children;
            for (const auto& dir_path : all_dirs) {
                if (processed_children.count(dir_path)) continue;

                bool has_audio_in_root = false, has_subdirs = false, has_non_disc_subdirs = false;
                std::vector<fs::path> subdirs;
                try {
                    for (const auto& entry : fs::directory_iterator(dir_path, dir_options)) {
                        if (entry.is_directory()) {
                            has_subdirs = true;
                            subdirs.push_back(entry.path());
                            std::string fn = path_to_utf8(entry.path().filename());
                            std::transform(fn.begin(), fn.end(), fn.begin(), ::tolower);
                            if (fn.rfind("cd", 0) != 0 && fn.rfind("disc", 0) != 0 && fn.rfind("vol", 0) != 0) {
                                has_non_disc_subdirs = true;
                            }
                        } else if (entry.is_regular_file()) {
                            std::string ext = path_to_utf8(entry.path().extension());
                            if (ext == ".mp3" || ext == ".flac" || ext == ".wav") has_audio_in_root = true;
                        }
                    }
                } catch (const fs::filesystem_error& e) { 
                    std::cerr << "Warning: Filesystem error scanning directory " << path_to_utf8(dir_path) << ": " << e.what() << std::endl;
                    continue; 
                }
                
                // A directory is an album if it contains audio files and is not a sub-directory of another album.
                // This check is key to identifying multi-disc albums correctly.
                // Condition:
                // 1. It must contain audio, either in its root or in valid 'disc' subdirectories.
                // 2. If it has subdirectories, they must ALL be valid disc/vol subdirectories.
                bool has_audio_in_subdirs = false;
                if (!has_audio_in_root && has_subdirs && !has_non_disc_subdirs) {
                    for (const auto& subdir_path : subdirs) {
                        try {
                            for (const auto& sub_entry : fs::directory_iterator(subdir_path, dir_options)) {
                                std::string ext = path_to_utf8(sub_entry.path().extension());
                                if (sub_entry.is_regular_file() && (ext == ".mp3" || ext == ".flac" || ext == ".wav")) {
                                    has_audio_in_subdirs = true;
                                    break;
                                }
                            }
                        } catch (const fs::filesystem_error&) {}
                        if (has_audio_in_subdirs) break;
                    }
                }

                if ((has_audio_in_root || has_audio_in_subdirs) && !has_non_disc_subdirs) {
                    album_folders_queue.push(dir_path);
                    if (has_subdirs) {
                        for (const auto& sd : subdirs) processed_children.insert(sd);
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "FATAL: Unhandled exception in discovery thread: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "FATAL: Unknown unhandled exception in discovery thread." << std::endl;
        }
        album_folders_queue.finish();
    });

    // 2. Worker Threads: Pop an album folder, process all its tracks, and push a complete Album object.
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    
    std::vector<std::thread> workers;
    std::atomic<int> active_workers{ static_cast<int>(num_threads) };

    for (unsigned int i = 0; i < num_threads; ++i) {
        workers.emplace_back([&, i]() {
            try {
                fs::path album_path;
                while (album_folders_queue.pop(album_path)) {
                    Album album;

                    // Heuristic to detect category albums based on parent folder name.
                    // A more robust implementation would use a configurable list from rules.json.
                    std::string parent_dir_name = path_to_utf8(album_path.parent_path().filename());
                    std::transform(parent_dir_name.begin(), parent_dir_name.end(), parent_dir_name.begin(), ::tolower);
                    if (parent_dir_name == "soundtracks" || parent_dir_name == "childrens" || parent_dir_name == "various artists") {
                        album.is_category_album = true;
                    }

                    album.folder_path = album_path;
                    std::vector<fs::path> files_to_process;
                    try {
                        for (const auto& entry : fs::directory_iterator(album_path, fs::directory_options::skip_permission_denied)) {
                            std::string ext = path_to_utf8(entry.path().extension());
                            if (entry.is_regular_file() && (ext == ".mp3" || ext == ".flac" || ext == ".wav")) {
                                files_to_process.push_back(entry.path());
                            } else if (entry.is_directory()) {
                                std::string fn = path_to_utf8(entry.path().filename());
                                std::transform(fn.begin(), fn.end(), fn.begin(), ::tolower);
                                if (fn.rfind("cd", 0) == 0 || fn.rfind("disc", 0) == 0 || fn.rfind("vol", 0) == 0) {
                                    album.is_multi_disc = true;
                                    for (const auto& sub_entry : fs::directory_iterator(entry.path(), fs::directory_options::skip_permission_denied)) {
                                        std::string sub_ext = path_to_utf8(sub_entry.path().extension());
                                        if (sub_entry.is_regular_file() && (sub_ext == ".mp3" || sub_ext == ".flac" || sub_ext == ".wav")) {
                                            files_to_process.push_back(sub_entry.path());
                                        }
                                    }
                                }
                            }
                        }
                    } catch (const fs::filesystem_error& e) { 
                        std::cerr << "Warning: Filesystem error processing album " << path_to_utf8(album_path) << ": " << e.what() << std::endl;
                        continue; 
                    }

                    for (const auto& path : files_to_process) {
                        Track t;
                        t.file_path = path;
                        t.filename = path_to_utf8(path.filename());
                        t.extension = path_to_utf8(path.extension());
                        t_has_legacy_frames = false;
#ifdef _WIN32
                        TagLib::FileRef f(path.wstring().c_str());
#else
                        TagLib::FileRef f(path_to_utf8(path).c_str());
#endif
                        if (!f.isNull() && f.tag()) {
                            TagLib::Tag *tag = f.tag();
                            t.title = tag->title().toCString(true);
                            t.artist = tag->artist().toCString(true);
                            t.album = tag->album().toCString(true);
                            t.year = tag->year();
                            t.track_number = tag->track();
                        }
                        if (!f.isNull() && f.audioProperties()) {
                            t.bitrate = f.audioProperties()->bitrate();
                            t.sample_rate = f.audioProperties()->sampleRate();
                            if (auto flac_file = dynamic_cast<TagLib::FLAC::File*>(f.file())) {
                                t.bits_per_sample = flac_file->audioProperties()->bitsPerSample();
                            }
                        }
                        t.has_legacy_date_frames = t_has_legacy_frames;
                        album.tracks.push_back(std::move(t));
                    }
                    if (!album.tracks.empty()) {
                        albums_queue.push(std::move(album));
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "FATAL: Unhandled exception in worker thread " << i << ": " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "FATAL: Unknown unhandled exception in worker thread " << i << "." << std::endl;
            }
            
            if (--active_workers == 0) {
                albums_queue.finish();
            }
        });
    }

    // 3. Main Thread: Process albums as they are discovered and prompt for fixes.
    std::cout << "Scan in progress. Presenting violations as they are found...\n\n";

    std::string file_naming_rule = rules.value("file_naming", "{TrackNumber} - {Title}.{ext}");
    int processed_album_count = 0;
    Album current_album;
    std::vector<Album> all_albums;

    while (albums_queue.pop(current_album)) {
        if (current_album.tracks.empty()) continue;
        processed_album_count++;

        // --- PASS 1: FOLDER AND SUB-FOLDER RENAMES ---
        auto& folder_path = current_album.folder_path;
        auto& tracks = current_album.tracks;

        // --- GATHER FOLDER VIOLATIONS ---
        // PHASE 1: Folder Naming Violation
        std::vector<std::string> folder_name_violations;
        std::string expected_folder_name_str;
        std::string current_folder_name = path_to_utf8(folder_path.filename());

        unsigned int album_year = tracks[0].year;
        std::string album_name = tracks[0].album;

        if (album_year > 0 && !album_name.empty()) {
            std::string sanitized_album_name = album_name;

            // Convert common edition/version information in parentheses to brackets
            // e.g., "Album (Deluxe Version)" -> "Album [Deluxe Version]"
            size_t open_paren = 0;
            while ((open_paren = sanitized_album_name.find('(', open_paren)) != std::string::npos) {
                size_t close_paren = sanitized_album_name.find(')', open_paren);
                if (close_paren != std::string::npos) {
                    std::string inside = sanitized_album_name.substr(open_paren + 1, close_paren - open_paren - 1);
                    std::string lower_inside = inside;
                    std::transform(lower_inside.begin(), lower_inside.end(), lower_inside.begin(), ::tolower);
                    if (lower_inside.find("version") != std::string::npos || 
                        lower_inside.find("edition") != std::string::npos ||
                        lower_inside.find("remaster") != std::string::npos ||
                        lower_inside.find("deluxe") != std::string::npos ||
                        lower_inside.find("bonus") != std::string::npos ||
                        lower_inside.find("explicit") != std::string::npos ||
                        lower_inside.find("clean") != std::string::npos) {
                        
                        sanitized_album_name[open_paren] = '[';
                        sanitized_album_name[close_paren] = ']';
                    }
                }
                open_paren++;
            }

            size_t colon_pos = 0;
            while ((colon_pos = sanitized_album_name.find(':', colon_pos)) != std::string::npos) {
                sanitized_album_name.replace(colon_pos, 1, " - ");
                colon_pos += 3;
            }

            // Clean up potential double spaces from tags.
            size_t space_pos = 0;
            while ((space_pos = sanitized_album_name.find("  ", space_pos)) != std::string::npos) {
                sanitized_album_name.replace(space_pos, 2, " ");
            }

            std::string invalid_os_chars = "<>\"|?*/\\";
            for (char c : invalid_os_chars) {
                std::replace(sanitized_album_name.begin(), sanitized_album_name.end(), c, '_');
            }

            if (current_album.is_category_album) {
                // Rule: `Album Name [YYYY]`
                expected_folder_name_str = sanitized_album_name + " [" + std::to_string(album_year) + "]";
                if (current_folder_name != expected_folder_name_str) {
                    folder_name_violations.push_back("Category album folder name does not match 'Album Name [YYYY]' format.");
                }
            } else {
                // Rule: `YYYY - Album Name [Technical Info]`
                std::string tech_str;
                const auto& first_track = tracks[0];
                std::string codec = first_track.extension;
                if (!codec.empty()) {
                    codec = codec.substr(1);
                    std::transform(codec.begin(), codec.end(), codec.begin(), 
                                   [](unsigned char c) { return std::toupper(c); });
                }

                std::stringstream ss;
                if (codec == "FLAC") {
                    // [FLAC <Bit-Depth>bit <Sampling-Rate>kHz]
                    ss << "[FLAC " << first_track.bits_per_sample << "bit ";
                    if (first_track.sample_rate % 1000 == 0) {
                        ss << (first_track.sample_rate / 1000) << "kHz]";
                    } else {
                        ss << std::fixed << std::setprecision(1) << (static_cast<double>(first_track.sample_rate) / 1000.0) << "kHz]";
                    }
                } else if (codec == "MP3") {
                    bool is_vbr = false;
                    if (tracks.size() > 1) {
                        int first_bitrate = first_track.bitrate;
                        // Check if all tracks have the same bitrate
                        for (size_t i = 1; i < tracks.size(); ++i) {
                            if (tracks[i].bitrate != first_bitrate) {
                                is_vbr = true;
                                break;
                            }
                        }
                    }
                    
                    if (is_vbr) {
                        // Per user request for VBR MP3, but getting LAME profile (e.g., V0) is not
                        // reliably exposed by TagLib. Using average bitrate as a robust alternative,
                        // which aligns with the project's RULES.md for VBR.
                        long long total_bitrate = 0;
                        for(const auto& t : tracks) total_bitrate += t.bitrate;
                        int avg_bitrate_kbps = tracks.size() > 0 ? static_cast<int>(total_bitrate / tracks.size()) : 0;
                        ss << "[MP3 VBR ~" << avg_bitrate_kbps << "kbps]";
                    } else {
                        // [MP3 <Bitrate>kbps]
                        ss << "[MP3 " << first_track.bitrate << "kbps]";
                    }
                } else if (codec == "OPUS") {
                    // [Opus <Nominal-Bitrate>kbps] - using average as a good album-wide metric.
                    long long total_bitrate = 0;
                    for(const auto& t : tracks) total_bitrate += t.bitrate;
                    int avg_bitrate_kbps = tracks.size() > 0 ? static_cast<int>(total_bitrate / tracks.size()) : 0;
                    ss << "[Opus " << avg_bitrate_kbps << "kbps]";
                } else {
                    // Fallback to old format for other codecs (WAV, etc.)
                    long long total_bitrate = 0;
                    for(const auto& t : tracks) total_bitrate += t.bitrate;
                    int avg_bitrate_kbps = tracks.size() > 0 ? static_cast<int>(total_bitrate / tracks.size()) : 0;
                    
                    std::string lower_codec = codec;
                    std::transform(lower_codec.begin(), lower_codec.end(), lower_codec.begin(),
                                   [](unsigned char c) { return std::tolower(c); });

                    ss << "[" << lower_codec << " ";
                    if (first_track.sample_rate % 1000 == 0) {
                        ss << (first_track.sample_rate / 1000) << "kHz ";
                    } else {
                        ss << std::fixed << std::setprecision(1) << (static_cast<double>(first_track.sample_rate) / 1000.0) << "kHz ";
                    }
                    ss << avg_bitrate_kbps << "kbps]";
                }
                tech_str = ss.str();
                
                std::string base_expected_name = std::to_string(album_year) + " - " + sanitized_album_name;
                expected_folder_name_str = base_expected_name + " " + tech_str;

                // Now, let's do a more intelligent comparison to give better feedback.
                // Check 1: Does the current name have the required tech info?
                bool has_tech_info = false;
                if (current_folder_name.back() == ']') {
                    if (current_folder_name.rfind(" [") != std::string::npos) {
                        has_tech_info = true;
                    }
                }
                
                if (!has_tech_info) {
                    folder_name_violations.push_back("Required [Technical Info] block is missing.");
                }

                // Check 2: Does the base name match? (Year - Album Title)
                std::string current_base_name = current_folder_name;
                if (has_tech_info) {
                    current_base_name = current_folder_name.substr(0, current_folder_name.rfind(" ["));
                }

                if (current_base_name != base_expected_name) {
                    folder_name_violations.push_back("Folder name does not match tag-derived name (Year - Album).");
                }
            }

            if (folder_name_violations.empty() && current_folder_name != expected_folder_name_str) {
                // This can happen if the tech info is present but incorrect.
                folder_name_violations.push_back("Existing [Technical Info] block is incorrect or malformed.");
            }
        }

        // PHASE 1.5: Sub-folder Naming Violations
        struct ProposedSubfolderRename {
            fs::path path;
            std::string current_name;
            std::string expected_name;
        };
        std::map<fs::path, ProposedSubfolderRename> proposed_subfolder_renames;

        if (current_album.is_multi_disc) {
            std::set<fs::path> processed_subfolders;
            for (const auto& t : tracks) {
                fs::path subfolder_path = t.file_path.parent_path();
                if (subfolder_path != folder_path && processed_subfolders.find(subfolder_path) == processed_subfolders.end()) {
                    processed_subfolders.insert(subfolder_path);
                    
                    std::string current_name = path_to_utf8(subfolder_path.filename());
                    std::string expected_name = current_name;

                    std::string lower_name = current_name;
                    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

                    size_t prefix_len = 0;
                    if (lower_name.rfind("disc", 0) == 0) prefix_len = 4;
                    else if (lower_name.rfind("cd", 0) == 0) prefix_len = 2;
                    else if (lower_name.rfind("vol", 0) == 0) prefix_len = 3;

                    if (prefix_len > 0) {
                        size_t num_start_pos = std::string::npos;
                        for(size_t i = prefix_len; i < lower_name.length(); ++i) {
                            if(isdigit(lower_name[i])) {
                                num_start_pos = i;
                                break;
                            }
                        }

                        if (num_start_pos != std::string::npos) {
                            size_t num_end_pos = lower_name.find_first_not_of("0123456789", num_start_pos);
                            if (num_end_pos == std::string::npos) num_end_pos = lower_name.length();

                            size_t sep_pos = num_end_pos;
                            while(sep_pos < current_name.length() && isspace(current_name[sep_pos])) {
                                sep_pos++;
                            }

                            if (sep_pos < current_name.length()) {
                                char sep = current_name[sep_pos];
                                bool violation = false;
                                if (sep == '_') {
                                    violation = true;
                                } else if (sep != '-') {
                                    violation = true;
                                }

                                if (violation) {
                                    std::string part1 = current_name.substr(0, sep_pos);
                                    part1.erase(part1.find_last_not_of(" \t") + 1);
                                    std::string part2 = current_name.substr(sep_pos + 1);
                                    part2.erase(0, part2.find_first_not_of(" \t"));
                                    expected_name = part1 + " - " + part2;
                                }
                            }
                        }
                    }

                    if (current_name != expected_name) {
                        proposed_subfolder_renames[subfolder_path] = {subfolder_path, current_name, expected_name};
                    }
                }
            }
        }

        if (!folder_name_violations.empty() || !proposed_subfolder_renames.empty()) {
            std::cout << "--------------------------------------------------------------------------------\n";
            std::cout << "Directory: " << path_to_utf8(folder_path) << "\n";
            std::cout << "--------------------------------------------------------------------------------\n";
            
            if (!folder_name_violations.empty()) {
                std::cout << "  [VIOLATION DETECTED] Album folder name does not match convention.\n";
                std::cout << "    Reasons:\n";
                for (const auto& reason : folder_name_violations) {
                    std::cout << "      - " << reason << "\n";
                }
                std::cout << "    Current:  " << current_folder_name << "\n";
                std::cout << "    Expected: " << expected_folder_name_str << "\n";

                if (interactive) {
                    std::cout << "  [PROMPT] Rename folder? (y/n/skip): ";
                    std::string response;
                    std::getline(std::cin, response);
                    if (response == "y" || response == "Y") {
                        fs::path old_path_copy = folder_path;
                        fs::path new_folder_path = old_path_copy.parent_path() / expected_folder_name_str;
                        if (!is_dry_run) {
                            try {
                                fs::rename(old_path_copy, new_folder_path);
                                std::cout << "      [SUCCESS] Folder renamed to " << expected_folder_name_str << "\n";
                                current_album.folder_path = new_folder_path;
                                folder_path = current_album.folder_path; // Re-seat reference
                                for (auto& t : tracks) {
                                    t.file_path = new_folder_path / fs::relative(t.file_path, old_path_copy);
                                }
                            } catch (const std::exception& e) {
                                std::cerr << "      [ERROR] Failed to rename folder: " << e.what() << "\n";
                            }
                        } else {
                            std::cout << "      [DRY RUN] Folder would be renamed to " << expected_folder_name_str << "\n";
                        }
                    } else {
                        std::cout << "  -> Skipped folder rename.\n";
                    }
                }
                std::cout << "\n";
            }

            if (!proposed_subfolder_renames.empty()) {
                std::cout << "  [VIOLATION DETECTED] " << proposed_subfolder_renames.size() << " sub-folder(s) do not match naming convention (e.g., 'Vol. 01 - Title').\n";
                std::cout << "    Proposed Changes:\n";
                for (const auto& pair : proposed_subfolder_renames) {
                    std::cout << "      - " << pair.second.current_name << " -> " << pair.second.expected_name << "\n";
                }

                if (interactive) {
                    std::cout << "  [PROMPT] Accept all " << proposed_subfolder_renames.size() << " sub-folder rename(s)? (y/n/skip): ";
                    std::string response;
                    std::getline(std::cin, response);
                    if (response == "y" || response == "Y") {
                        for (const auto& pair : proposed_subfolder_renames) {
                            const auto& rename_op = pair.second;
                            fs::path old_subfolder_path = rename_op.path;
                            fs::path new_subfolder_path = old_subfolder_path.parent_path() / rename_op.expected_name;
                            if (!is_dry_run) {
                                try {
                                    fs::rename(old_subfolder_path, new_subfolder_path);
                                    std::cout << "      [SUCCESS] Sub-folder " << rename_op.current_name << " -> " << rename_op.expected_name << "\n";
                                    // Now update all track paths that were in this subfolder
                                    for (auto& t : tracks) {
                                        if (t.file_path.parent_path() == old_subfolder_path) {
                                            t.file_path = new_subfolder_path / t.file_path.filename();
                                        }
                                    }
                                } catch (const std::exception& e) {
                                    std::cerr << "      [ERROR] Failed to rename sub-folder " << rename_op.current_name << ": " << e.what() << "\n";
                                }
                            } else {
                                std::cout << "      [DRY RUN] Sub-folder " << rename_op.current_name << " -> " << rename_op.expected_name << "\n";
                            }
                        }
                    } else {
                        std::cout << "  -> Skipped sub-folder renames.\n";
                    }
                }
                std::cout << "\n";
            }
        }
        
        all_albums.push_back(std::move(current_album));
    } // End of folder processing loop

    std::cout << "Folder pass complete. Presenting track violations...\n\n";

    for (auto& current_album : all_albums) {
        auto& folder_path = current_album.folder_path;
        auto& tracks = current_album.tracks;
        
        // --- PASS 2: TRACK RENAMES AND METADATA FIXES ---
        // PHASE 2: Metadata and Filename Violations
        struct ProposedRename {
            Track* track;
            std::string expected_filename;
            std::vector<std::string> violations;
        };
        std::vector<ProposedRename> proposed_renames;
        std::vector<Track*> legacy_tag_tracks;

        for (auto& t : tracks) {
            std::vector<std::string> violations;
            
            if (t.has_legacy_date_frames) {
                legacy_tag_tracks.push_back(&t);
            }

            // Calculate expected filename
            std::string expected = file_naming_rule;
            
            std::string track_str = "00";
            if (t.track_number > 0) {
                std::ostringstream ss;
                ss << std::setw(2) << std::setfill('0') << t.track_number;
                track_str = ss.str();
            }
            size_t replace_pos;
            while ((replace_pos = expected.find("{TrackNumber}")) != std::string::npos) {
                expected.replace(replace_pos, 13, track_str);
            }
            
            std::string title_str = t.title.empty() ? "Unknown Title" : t.title;
            
            size_t colon_pos = 0;
            while ((colon_pos = title_str.find(':', colon_pos)) != std::string::npos) {
                title_str.replace(colon_pos, 1, " - ");
                colon_pos += 3;
            }

            // Clean up potential double spaces from tags.
            size_t space_pos = 0;
            while ((space_pos = title_str.find("  ", space_pos)) != std::string::npos) {
                title_str.replace(space_pos, 2, " ");
            }

            // Replace OS-invalid characters (and path separators) with underscores 
            // to prevent filesystem errors and match standard ripping conventions.
            std::string invalid_os_chars = "<>\"|?*/\\";
            for (char c : invalid_os_chars) {
                std::replace(title_str.begin(), title_str.end(), c, '_');
            }

            while ((replace_pos = expected.find("{Title}")) != std::string::npos) {
                expected.replace(replace_pos, 7, title_str);
            }
            
            std::string ext_str = t.extension.empty() ? "" : t.extension.substr(1);
            while ((replace_pos = expected.find("{ext}")) != std::string::npos) {
                expected.replace(replace_pos, 5, ext_str);
            }
            
            if (t.filename != expected) {
                std::string f_norm = t.filename;
                std::string e_norm = expected;
                
                auto remove_space_underscore = [](char c) { return c == '_' || c == ' '; };
                f_norm.erase(std::remove_if(f_norm.begin(), f_norm.end(), remove_space_underscore), f_norm.end());
                e_norm.erase(std::remove_if(e_norm.begin(), e_norm.end(), remove_space_underscore), e_norm.end());
                
                if (f_norm == e_norm) {
                    violations.push_back("Filename does not match rule: " + file_naming_rule + " (Mismatch: Spaces vs Underscores)");
                } else {
                    violations.push_back("Filename does not match rule: " + file_naming_rule);
                }
            }

            if (!violations.empty()) {
                proposed_renames.push_back({&t, expected, violations});
            }
        }

        // --- PRESENT FILE-LEVEL VIOLATIONS AND PROMPT FOR FIXES ---
        if (!legacy_tag_tracks.empty() || !proposed_renames.empty()) {
            std::cout << "--------------------------------------------------------------------------------\n";
            std::cout << "Directory: " << path_to_utf8(folder_path) << "\n";
            std::cout << "--------------------------------------------------------------------------------\n";
            if (!legacy_tag_tracks.empty()) {
                std::cout << "  [WARNING] " << legacy_tag_tracks.size() << " track(s) have legacy ID3v2 date frames (TDAT/TYER/TIME).\n";
                if (interactive) {
                    std::cout << "  [PROMPT] Update tags to standard ID3v2.4 for all " << legacy_tag_tracks.size() << " tracks? (y/n/skip): ";
                    std::string response;
                    std::getline(std::cin, response);
                    if (response == "y" || response == "Y") {
                        bool has_errors = false;
                        for (auto* t : legacy_tag_tracks) {
                            if (!is_dry_run) {
#ifdef _WIN32
                                TagLib::FileRef f(t->file_path.wstring().c_str());
#else
                                TagLib::FileRef f(path_to_utf8(t->file_path).c_str());
#endif
                                if (!f.isNull() && f.file()) {
                                    f.file()->save();
                                    std::cout << "      [SUCCESS] " << t->filename << " -> tags updated to ID3v2.4.\n";
                                } else {
                                    std::cerr << "      [ERROR] " << t->filename << " -> Failed to save tags.\n";
                                    has_errors = true;
                                }
                            } else {
                                std::cout << "      [DRY RUN] " << t->filename << " -> tags updated to ID3v2.4.\n";
                            }
                        }
                        if (is_dry_run) {
                            std::cout << "  -> Tag update dry-run complete.\n";
                        } else {
                            if (has_errors) {
                                std::cout << "  -> Batch tag update completed with errors.\n";
                            } else {
                                std::cout << "  -> Batch tag update applied successfully.\n";
                            }
                        }
                    } else {
                        std::cout << "  -> Skipped tag update.\n";
                    }
                }
                std::cout << "\n";
            }

            if (!proposed_renames.empty()) {
                std::cout << "  [VIOLATIONS DETECTED] " << proposed_renames.size() << " track(s) have filename issues.\n";
                std::vector<std::string> unique_violations;
                for (const auto& pr : proposed_renames) {
                    for (const auto& v : pr.violations) {
                        if (std::find(unique_violations.begin(), unique_violations.end(), v) == unique_violations.end()) {
                            unique_violations.push_back(v);
                        }
                    }
                }
                
                std::cout << "    Reasons:\n";
                for (const auto& v : unique_violations) {
                    std::cout << "      - " << v << "\n";
                }

                // Sort proposed changes sequentially by track number, fallback to filename
                std::sort(proposed_renames.begin(), proposed_renames.end(), [](const auto& a, const auto& b) {
                    if (a.track->track_number != b.track->track_number) {
                        return a.track->track_number < b.track->track_number;
                    }
                    return a.track->filename < b.track->filename;
                });

                std::cout << "    Current Filenames:\n";
                for (const auto& pr : proposed_renames) {
                    std::cout << "      - " << pr.track->filename << "\n";
                }

                std::cout << "    Proposed Filenames:\n";
                for (const auto& pr : proposed_renames) {
                    std::cout << "      - " << pr.expected_filename << "\n";
                }

                if (interactive) {
                    std::cout << "  [PROMPT] Accept all " << proposed_renames.size() << " proposed rename(s)? (y/n/skip): ";
                    std::string response;
                    std::getline(std::cin, response);
                    
                    if (response == "y" || response == "Y") {
                        bool has_errors = false;
                        for (const auto& pr : proposed_renames) {
                            fs::path new_path = pr.track->file_path.parent_path() / pr.expected_filename;
                            std::string old_name = pr.track->filename;
                            if (!is_dry_run) {
                                try {
                                    fs::rename(pr.track->file_path, new_path);
                                    pr.track->file_path = new_path;
                                    pr.track->filename = pr.expected_filename;
                                    std::cout << "      [SUCCESS] " << old_name << " -> " << pr.expected_filename << "\n";
                                } catch (const std::exception& e) {
                                    std::cerr << "      [ERROR] Rename failed for " << old_name << ": " << e.what() << "\n";
                                    has_errors = true;
                                }
                            } else {
                                std::cout << "      [DRY RUN] " << old_name << " -> " << pr.expected_filename << "\n";
                            }
                        }
                        if (is_dry_run) {
                            std::cout << "  -> Batch rename dry-run complete.\n";
                        } else {
                            if (has_errors) {
                                std::cout << "  -> Batch rename completed with errors.\n";
                            } else {
                                std::cout << "  -> Batch rename applied successfully.\n";
                            }
                        }
                    } else {
                        std::cout << "  -> Skipped batch rename.\n";
                    }
                }
                std::cout << "\n";
            }
        }
    } // End of track processing loop

    // Wait for all background threads to complete their work.
    discovery_thread.join();
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    std::cout << "\nTotal albums processed: " << processed_album_count << "\n\n";

}


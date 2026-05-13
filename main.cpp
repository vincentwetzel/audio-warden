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
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tdebuglistener.h>

#include "models.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

thread_local bool t_has_legacy_frames = false;

class AudioWardenDebugListener : public TagLib::DebugListener {
public:
    void printMessage(const TagLib::String &msg) override {
        std::string m = msg.to8Bit(true);
        if (m.find("TDAT") != std::string::npos || 
            m.find("TYER") != std::string::npos ||
            m.find("TIME") != std::string::npos) {
            t_has_legacy_frames = true;
        }
        // By swallowing the output here, we prevent library warnings from interleaving with our CLI prompts.
    }
};

// Helper to safely convert path to UTF-8 std::string for C++17/C++20 compatibility
std::string path_to_utf8(const fs::path& p) {
    auto u8str = p.u8string();
    return std::string(u8str.begin(), u8str.end());
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

    ThreadSafeQueue<fs::path> paths_queue;
    ThreadSafeQueue<Track> tracks_queue;
    
    // 1. Discovery Thread: Finds files and pushes them to the paths queue
    std::thread discovery_thread([&]() {
        auto dir_options = fs::directory_options::skip_permission_denied;
        for (const auto& entry : fs::recursive_directory_iterator(library_path, dir_options)) {
            if (entry.is_regular_file()) {
                std::string extension = path_to_utf8(entry.path().extension());
                if (extension == ".mp3" || extension == ".flac" || extension == ".wav") {
                    paths_queue.push(entry.path());
                }
            }
        }
        paths_queue.finish();
    });

    // 2. Worker Threads: Parse tags in the background
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    
    std::vector<std::thread> workers;
    std::atomic<int> active_workers{ static_cast<int>(num_threads) };

    for (unsigned int i = 0; i < num_threads; ++i) {
        workers.emplace_back([&]() {
            fs::path path;
            while (paths_queue.pop(path)) {
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
                    t.track_number = tag->track();
                }
                
                if (!f.isNull() && f.audioProperties()) {
                    t.bitrate = f.audioProperties()->bitrate();
                    t.sample_rate = f.audioProperties()->sampleRate();
                }
                
                t.has_legacy_date_frames = t_has_legacy_frames;
                
                tracks_queue.push(std::move(t));
            }
            
            if (--active_workers == 0) {
                tracks_queue.finish();
            }
        });
    }

    // 3. Main Thread (Interactive Prompter): Consume parsed tracks as they arrive
    Track current_track;
    int processed_count = 0;
    
    std::map<fs::path, std::vector<Track>> album_folders;
    
    while (tracks_queue.pop(current_track)) {
        processed_count++;
        album_folders[current_track.file_path.parent_path()].push_back(std::move(current_track));
    }

    std::cout << "Background scan complete. Total files processed: " << processed_count << "\n\n";

    std::string file_naming_rule = rules.value("file_naming", "{TrackNumber} - {Title}.{ext}");
    std::vector<std::string> forbidden_chars;
    if (rules.contains("forbidden_characters")) {
        for (const auto& fc_json : rules["forbidden_characters"]) {
            forbidden_chars.push_back(fc_json.get<std::string>());
        }
    }

    // =========================================================================
    // PHASE 1: Folder Naming Issues
    // =========================================================================
    std::map<fs::path, std::vector<Track>> updated_album_folders;
    
    for (auto& [folder_path, tracks] : album_folders) {
        fs::path current_folder_path = folder_path;
        
        // TODO: Implement folder naming validation based on RULES.md here.
        // Example placeholder logic:
        // fs::path proposed_folder_path = ...; // Generate using tags/audio properties
        // if (needs_rename && !is_dry_run) {
        //     fs::rename(current_folder_path, proposed_folder_path);
        //     current_folder_path = proposed_folder_path;
        //
        //     // CRITICAL: Update track file_paths so Phase 2 can still find them
        //     for (auto& t : tracks) {
        //         t.file_path = current_folder_path / t.filename;
        //     }
        // }
        
        updated_album_folders[current_folder_path] = std::move(tracks);
    }
    album_folders = std::move(updated_album_folders);

    // =========================================================================
    // PHASE 2: Metadata and Filename Issues
    // =========================================================================
    for (auto& [folder_path, tracks] : album_folders) {
        struct ProposedRename {
            Track* track;
            std::string expected_filename;
            std::vector<std::string> violations;
        };
        std::vector<ProposedRename> proposed_renames;
        std::vector<Track*> legacy_tag_tracks;

        for (auto& t : tracks) {
            std::vector<std::string> violations;
            
            if (rules.contains("forbidden_characters")) {
                for (const auto& fc : forbidden_chars) {
                    if (t.filename.find(fc) != std::string::npos) {
                        violations.push_back("Filename contains forbidden character: '" + fc + "'");
                    }
                }
            }

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
            size_t pos;
            while ((pos = expected.find("{TrackNumber}")) != std::string::npos) {
                expected.replace(pos, 13, track_str);
            }
            
            std::string title_str = t.title.empty() ? "Unknown Title" : t.title;
            // Clean title of forbidden characters
            for (const auto& fc : forbidden_chars) {
                size_t fc_pos;
                while ((fc_pos = title_str.find(fc)) != std::string::npos) {
                    title_str.replace(fc_pos, fc.length(), "");
                }
            }
            
            // Replace OS-invalid characters (and path separators) with underscores 
            // to prevent filesystem errors and match standard ripping conventions.
            std::string invalid_os_chars = "<>:\"|?*/\\";
            for (char c : invalid_os_chars) {
                std::replace(title_str.begin(), title_str.end(), c, '_');
            }

            while ((pos = expected.find("{Title}")) != std::string::npos) {
                expected.replace(pos, 7, title_str);
            }
            
            std::string ext_str = t.extension.empty() ? "" : t.extension.substr(1);
            while ((pos = expected.find("{ext}")) != std::string::npos) {
                expected.replace(pos, 5, ext_str);
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

        if (!proposed_renames.empty() || !legacy_tag_tracks.empty()) {
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
    }

    // Cleanup threads securely before returning
    discovery_thread.join();
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }
}

int main(int argc, char** argv) {
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
            scan_library(path, rules, dry_run, interactive);
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

#include "prompter.h"

#include <algorithm>
#include <filesystem>
#include <iostream>

#include <taglib/fileref.h>

#include "io_utils.h"

namespace fs = std::filesystem;

namespace {
bool confirm(const std::string& prompt, bool interactive) {
    if (!interactive) return false;
    std::cout << prompt;
    std::string response;
    std::getline(std::cin, response);
    return response == "y" || response == "Y";
}

void print_header(const fs::path& folder_path) {
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << "Directory: " << path_to_utf8(folder_path) << "\n";
    std::cout << "--------------------------------------------------------------------------------\n";
}
}

void prompt_folder_actions(Album& album, const FolderValidationResult& result, bool is_dry_run, bool interactive) {
    if (result.violations.empty() && result.subfolder_renames.empty()) return;

    print_header(album.folder_path);
    auto& folder_path = album.folder_path;
    auto& tracks = album.tracks;

    if (!result.violations.empty()) {
        std::cout << "  [VIOLATION DETECTED] Album folder name does not match convention.\n";
        std::cout << "    Reasons:\n";
        for (const auto& reason : result.violations) std::cout << "      - " << reason << "\n";
        std::cout << "    Current:  " << result.current_name << "\n";
        std::cout << "    Expected: " << result.expected_name << "\n";

        if (confirm("  [PROMPT] Rename folder? (y/n/skip): ", interactive)) {
            fs::path old_path = folder_path;
            fs::path new_folder_path = old_path.parent_path() / result.expected_name;
            if (!is_dry_run) {
                try {
                    fs::rename(old_path, new_folder_path);
                    std::cout << "      [SUCCESS] Folder renamed to " << result.expected_name << "\n";
                    album.folder_path = new_folder_path;
                    for (auto& t : tracks) t.file_path = new_folder_path / fs::relative(t.file_path, old_path);
                } catch (const std::exception& e) {
                    std::cerr << "      [ERROR] Failed to rename folder: " << e.what() << "\n";
                }
            } else {
                std::cout << "      [DRY RUN] Folder would be renamed to " << result.expected_name << "\n";
            }
        } else if (interactive) {
            std::cout << "  -> Skipped folder rename.\n";
        }
        std::cout << "\n";
    }

    if (result.subfolder_renames.empty()) return;
    std::cout << "  [VIOLATION DETECTED] " << result.subfolder_renames.size()
              << " sub-folder(s) do not match naming convention (e.g., 'Vol. 01 - Title').\n";
    std::cout << "    Proposed Changes:\n";
    for (const auto& op : result.subfolder_renames) {
        std::cout << "      - " << op.current_name << " -> " << op.expected_name << "\n";
    }

    if (confirm("  [PROMPT] Accept all sub-folder rename(s)? (y/n/skip): ", interactive)) {
        for (const auto& op : result.subfolder_renames) {
            fs::path new_subfolder_path = op.path.parent_path() / op.expected_name;
            if (!is_dry_run) {
                try {
                    fs::rename(op.path, new_subfolder_path);
                    std::cout << "      [SUCCESS] Sub-folder " << op.current_name << " -> " << op.expected_name << "\n";
                    for (auto& t : tracks) {
                        if (t.file_path.parent_path() == op.path) t.file_path = new_subfolder_path / t.file_path.filename();
                    }
                } catch (const std::exception& e) {
                    std::cerr << "      [ERROR] Failed to rename sub-folder " << op.current_name << ": " << e.what() << "\n";
                }
            } else {
                std::cout << "      [DRY RUN] Sub-folder " << op.current_name << " -> " << op.expected_name << "\n";
            }
        }
    } else if (interactive) {
        std::cout << "  -> Skipped sub-folder renames.\n";
    }
    std::cout << "\n";
}

void prompt_track_actions(const fs::path& folder_path, TrackValidationResult& result, bool is_dry_run, bool interactive) {
    if (result.legacy_tag_tracks.empty() && result.track_renames.empty()) return;
    print_header(folder_path);

    if (!result.legacy_tag_tracks.empty()) {
        std::cout << "  [WARNING] " << result.legacy_tag_tracks.size()
                  << " track(s) have legacy ID3v2 date frames (TDAT/TYER/TIME).\n";
        if (confirm("  [PROMPT] Update tags to standard ID3v2.4 for all tracks? (y/n/skip): ", interactive)) {
            bool has_errors = false;
            for (auto* t : result.legacy_tag_tracks) {
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
            std::cout << (has_errors ? "  -> Batch tag update completed with errors.\n" : "  -> Batch tag update applied successfully.\n");
        } else if (interactive) {
            std::cout << "  -> Skipped tag update.\n";
        }
        std::cout << "\n";
    }

    if (result.track_renames.empty()) return;
    std::cout << "  [VIOLATIONS DETECTED] " << result.track_renames.size() << " track(s) have filename issues.\n";
    std::vector<std::string> unique_violations;
    for (const auto& pr : result.track_renames) {
        for (const auto& v : pr.violations) {
            if (std::find(unique_violations.begin(), unique_violations.end(), v) == unique_violations.end()) unique_violations.push_back(v);
        }
    }
    std::cout << "    Reasons:\n";
    for (const auto& v : unique_violations) std::cout << "      - " << v << "\n";

    std::sort(result.track_renames.begin(), result.track_renames.end(), [](const auto& a, const auto& b) {
        if (a.track->track_number != b.track->track_number) return a.track->track_number < b.track->track_number;
        return a.track->filename < b.track->filename;
    });

    std::cout << "    Current Filenames:\n";
    for (const auto& pr : result.track_renames) std::cout << "      - " << pr.track->filename << "\n";
    std::cout << "    Proposed Filenames:\n";
    for (const auto& pr : result.track_renames) std::cout << "      - " << pr.expected_filename << "\n";

    if (!confirm("  [PROMPT] Accept all proposed rename(s)? (y/n/skip): ", interactive)) {
        if (interactive) std::cout << "  -> Skipped batch rename.\n";
        return;
    }

    bool has_errors = false;
    for (const auto& pr : result.track_renames) {
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
    std::cout << (has_errors ? "  -> Batch rename completed with errors.\n" : "  -> Batch rename applied successfully.\n");
    std::cout << "\n";
}

#include "scanner.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <vector>

#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/flacproperties.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>

#include "io_utils.h"
#include "tag_debug.h"

namespace fs = std::filesystem;

namespace {
bool is_audio_file(const fs::path& path) {
    std::string ext = path_to_utf8(path.extension());
    return ext == ".mp3" || ext == ".flac" || ext == ".wav";
}

bool is_disc_folder(const fs::path& path) {
    std::string name = path_to_utf8(path.filename());
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    return name.rfind("cd", 0) == 0 ||
           name.rfind("disc", 0) == 0 ||
           name.rfind("vol", 0) == 0;
}

bool has_audio_file(const fs::path& dir_path, fs::directory_options options) {
    for (const auto& entry : fs::directory_iterator(dir_path, options)) {
        if (entry.is_regular_file() && is_audio_file(entry.path())) {
            return true;
        }
    }
    return false;
}

void read_track_tags(const fs::path& path, Track& t) {
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
        TagLib::Tag* tag = f.tag();
        t.title = tag->title().toCString(true);
        t.artist = tag->artist().toCString(true);
        t.album = tag->album().toCString(true);
        t.release_year = tag->year();
        t.track_number = tag->track();

        if (f.file()) {
            auto props = f.file()->properties();
            if (props.contains("ORIGINALYEAR") && !props["ORIGINALYEAR"].isEmpty()) {
                try { t.year = std::stoul(props["ORIGINALYEAR"].front().to8Bit(true)); } catch (...) {}
            } else if (props.contains("ORIGINALDATE") && !props["ORIGINALDATE"].isEmpty()) {
                try {
                    std::string date = props["ORIGINALDATE"].front().to8Bit(true);
                    if (date.length() >= 4) t.year = std::stoul(date.substr(0, 4));
                } catch (...) {}
            }
        }
        if (t.year == 0) t.year = tag->year();
    }
    if (!f.isNull() && f.audioProperties()) {
        t.bitrate = f.audioProperties()->bitrate();
        t.sample_rate = f.audioProperties()->sampleRate();
        if (auto flac_file = dynamic_cast<TagLib::FLAC::File*>(f.file())) {
            t.bits_per_sample = flac_file->audioProperties()->bitsPerSample();
        }
    }
    t.has_legacy_date_frames = t_has_legacy_frames;
}
}

void discover_album_folders(const fs::path& library_path, ThreadSafeQueue<fs::path>& album_folders_queue) {
    auto dir_options = fs::directory_options::skip_permission_denied;
    std::vector<fs::path> all_dirs;

    try {
        for (const auto& entry : fs::recursive_directory_iterator(library_path, dir_options)) {
            if (entry.is_directory()) all_dirs.push_back(entry.path());
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Warning: Filesystem error during discovery: " << e.what() << std::endl;
    }
    all_dirs.push_back(library_path);
    std::sort(all_dirs.begin(), all_dirs.end());

    std::set<fs::path> processed_children;
    for (const auto& dir_path : all_dirs) {
        if (processed_children.count(dir_path)) continue;

        bool has_audio_in_root = false;
        bool has_subdirs = false;
        bool has_non_disc_subdirs = false;
        std::vector<fs::path> subdirs;

        try {
            for (const auto& entry : fs::directory_iterator(dir_path, dir_options)) {
                if (entry.is_directory()) {
                    has_subdirs = true;
                    subdirs.push_back(entry.path());
                    if (!is_disc_folder(entry.path())) has_non_disc_subdirs = true;
                } else if (entry.is_regular_file() && is_audio_file(entry.path())) {
                    has_audio_in_root = true;
                }
            }
        } catch (const fs::filesystem_error& e) {
            std::cerr << "Warning: Filesystem error scanning directory "
                      << path_to_utf8(dir_path) << ": " << e.what() << std::endl;
            continue;
        }

        bool has_audio_in_subdirs = false;
        if (!has_audio_in_root && has_subdirs && !has_non_disc_subdirs) {
            for (const auto& subdir_path : subdirs) {
                try {
                    if (has_audio_file(subdir_path, dir_options)) {
                        has_audio_in_subdirs = true;
                        break;
                    }
                } catch (const fs::filesystem_error&) {}
            }
        }

        if ((has_audio_in_root || has_audio_in_subdirs) && !has_non_disc_subdirs) {
            album_folders_queue.push(dir_path);
            for (const auto& sd : subdirs) processed_children.insert(sd);
        }
    }
}

Album parse_album_folder(
    const fs::path& album_path,
    const std::vector<std::string>& category_folders,
    const std::vector<std::string>& standalone_folders) {
    Album album;
    album.folder_path = album_path;

    for (auto p = album_path.parent_path(); p.has_filename(); p = p.parent_path()) {
        std::string dir_name = path_to_utf8(p.filename());
        std::transform(dir_name.begin(), dir_name.end(), dir_name.begin(), ::tolower);
        if (std::find(category_folders.begin(), category_folders.end(), dir_name) != category_folders.end()) {
            album.is_category_album = true;
            break;
        }
    }

    std::string album_dir_name = path_to_utf8(album_path.filename());
    std::transform(album_dir_name.begin(), album_dir_name.end(), album_dir_name.begin(), ::tolower);
    for (const auto& folder : standalone_folders) {
        if (album_dir_name.find(folder) != std::string::npos) {
            album.is_standalone_dir = true;
            break;
        }
    }

    std::vector<fs::path> files_to_process;
    auto options = fs::directory_options::skip_permission_denied;
    for (const auto& entry : fs::directory_iterator(album_path, options)) {
        if (entry.is_regular_file() && is_audio_file(entry.path())) {
            files_to_process.push_back(entry.path());
        } else if (entry.is_directory() && is_disc_folder(entry.path())) {
            album.is_multi_disc = true;
            for (const auto& sub_entry : fs::directory_iterator(entry.path(), options)) {
                if (sub_entry.is_regular_file() && is_audio_file(sub_entry.path())) {
                    files_to_process.push_back(sub_entry.path());
                }
            }
        }
    }

    for (const auto& path : files_to_process) {
        Track t;
        read_track_tags(path, t);
        album.tracks.push_back(std::move(t));
    }

    return album;
}

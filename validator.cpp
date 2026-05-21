#include "validator.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

#include "io_utils.h"

namespace fs = std::filesystem;

namespace {
std::string sanitize_name(std::string value) {
    size_t colon_pos = 0;
    while ((colon_pos = value.find(':', colon_pos)) != std::string::npos) {
        value.replace(colon_pos, 1, " - ");
        colon_pos += 3;
    }

    size_t space_pos = 0;
    while ((space_pos = value.find("  ", space_pos)) != std::string::npos) {
        value.replace(space_pos, 2, " ");
    }

    std::string invalid_os_chars = "<>\"|?*/\\";
    for (char c : invalid_os_chars) {
        std::replace(value.begin(), value.end(), c, '_');
    }
    return value;
}

std::string normalize_album_name(std::string album_name) {
    size_t open_paren = 0;
    while ((open_paren = album_name.find('(', open_paren)) != std::string::npos) {
        size_t close_paren = album_name.find(')', open_paren);
        if (close_paren == std::string::npos) break;

        std::string inside = album_name.substr(open_paren + 1, close_paren - open_paren - 1);
        std::transform(inside.begin(), inside.end(), inside.begin(), ::tolower);
        if (inside.find("version") != std::string::npos ||
            inside.find("edition") != std::string::npos ||
            inside.find("remaster") != std::string::npos ||
            inside.find("deluxe") != std::string::npos ||
            inside.find("bonus") != std::string::npos ||
            inside.find("explicit") != std::string::npos ||
            inside.find("clean") != std::string::npos) {
            album_name[open_paren] = '[';
            album_name[close_paren] = ']';
        }
        open_paren++;
    }
    return sanitize_name(album_name);
}

std::string format_sample_rate(int sample_rate) {
    std::stringstream ss;
    if (sample_rate % 1000 == 0) {
        ss << (sample_rate / 1000) << "kHz";
    } else {
        ss << std::fixed << std::setprecision(1)
           << (static_cast<double>(sample_rate) / 1000.0) << "kHz";
    }
    return ss.str();
}

std::string build_technical_info(const std::vector<Track>& tracks) {
    const auto& first_track = tracks[0];
    std::string codec = first_track.extension;
    if (!codec.empty()) {
        codec = codec.substr(1);
        std::transform(codec.begin(), codec.end(), codec.begin(),
                       [](unsigned char c) { return std::toupper(c); });
    }

    std::stringstream ss;
    if (codec == "FLAC") {
        ss << "[FLAC " << first_track.bits_per_sample << "bit "
           << format_sample_rate(first_track.sample_rate) << "]";
    } else if (codec == "MP3") {
        bool is_vbr = false;
        for (const auto& t : tracks) {
            if (t.bitrate != first_track.bitrate) {
                is_vbr = true;
                break;
            }
        }
        if (is_vbr) {
            long long total = 0;
            for (const auto& t : tracks) total += t.bitrate;
            ss << "[MP3 VBR ~" << static_cast<int>(total / tracks.size()) << "kbps]";
        } else {
            ss << "[MP3 " << first_track.bitrate << "kbps]";
        }
    } else {
        long long total = 0;
        for (const auto& t : tracks) total += t.bitrate;
        std::transform(codec.begin(), codec.end(), codec.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        ss << "[" << codec << " " << format_sample_rate(first_track.sample_rate)
           << " " << static_cast<int>(total / tracks.size()) << "kbps]";
    }
    return ss.str();
}

std::string build_expected_filename(const Track& t, const std::string& rule) {
    std::string expected = rule;
    std::string track_str = "00";
    if (t.track_number > 0) {
        std::ostringstream ss;
        ss << std::setw(2) << std::setfill('0') << t.track_number;
        track_str = ss.str();
    }

    size_t pos;
    while ((pos = expected.find("{TrackNumber}")) != std::string::npos) expected.replace(pos, 13, track_str);
    while ((pos = expected.find("{Title}")) != std::string::npos) {
        expected.replace(pos, 7, sanitize_name(t.title.empty() ? "Unknown Title" : t.title));
    }
    while ((pos = expected.find("{ext}")) != std::string::npos) {
        expected.replace(pos, 5, t.extension.empty() ? "" : t.extension.substr(1));
    }
    return expected;
}
}

std::string standalone_file_rule(const std::string& rule) {
    std::string expected = rule;
    size_t prefix_pos = expected.find("{TrackNumber} - ");
    if (prefix_pos != std::string::npos) {
        expected.replace(prefix_pos, 16, "");
    } else {
        size_t pos;
        while ((pos = expected.find("{TrackNumber}")) != std::string::npos) expected.replace(pos, 13, "");
    }
    return expected;
}

FolderValidationResult validate_album_folder(const Album& album) {
    FolderValidationResult result;
    const auto& tracks = album.tracks;
    if (tracks.empty()) return result;

    result.current_name = path_to_utf8(album.folder_path.filename());
    unsigned int album_year = tracks[0].year;
    std::string album_name = tracks[0].album;

    if (album_year > 0 && !album_name.empty()) {
        std::string clean_album_name = normalize_album_name(album_name);
        if (album.is_category_album) {
            result.expected_name = clean_album_name + " [" + std::to_string(album_year) + "]";
            if (result.current_name != result.expected_name) {
                result.violations.push_back("Category album folder name does not match 'Album Name [YYYY]' format.");
            }
        } else {
            std::string base_expected = std::to_string(album_year) + " - " + clean_album_name;
            result.expected_name = base_expected + " " + build_technical_info(tracks);
            bool has_tech_info = !result.current_name.empty() &&
                                 result.current_name.back() == ']' &&
                                 result.current_name.rfind(" [") != std::string::npos;

            if (!has_tech_info) result.violations.push_back("Required [Technical Info] block is missing.");
            std::string current_base = has_tech_info ? result.current_name.substr(0, result.current_name.rfind(" [")) : result.current_name;
            if (current_base != base_expected) result.violations.push_back("Folder name does not match tag-derived name (Year - Album).");
            if (result.violations.empty() && result.current_name != result.expected_name) {
                result.violations.push_back("Existing [Technical Info] block is incorrect or malformed.");
            }
        }
    }

    if (!album.is_multi_disc) return result;
    std::set<fs::path> processed_subfolders;
    for (const auto& t : tracks) {
        fs::path subfolder_path = t.file_path.parent_path();
        if (subfolder_path == album.folder_path || processed_subfolders.count(subfolder_path)) continue;
        processed_subfolders.insert(subfolder_path);

        std::string current_name = path_to_utf8(subfolder_path.filename());
        std::string lower_name = current_name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

        size_t prefix_len = lower_name.rfind("disc", 0) == 0 ? 4 :
                            lower_name.rfind("cd", 0) == 0 ? 2 :
                            lower_name.rfind("vol", 0) == 0 ? 3 : 0;
        if (prefix_len == 0) continue;

        size_t num_start = lower_name.find_first_of("0123456789", prefix_len);
        if (num_start == std::string::npos) continue;
        size_t num_end = lower_name.find_first_not_of("0123456789", num_start);
        if (num_end == std::string::npos) continue;

        size_t sep_pos = num_end;
        while (sep_pos < current_name.length() && std::isspace(static_cast<unsigned char>(current_name[sep_pos]))) sep_pos++;
        if (sep_pos >= current_name.length() || current_name[sep_pos] == '-') continue;

        std::string part1 = current_name.substr(0, sep_pos);
        part1.erase(part1.find_last_not_of(" \t") + 1);
        std::string part2 = current_name.substr(sep_pos + 1);
        part2.erase(0, part2.find_first_not_of(" \t"));
        result.subfolder_renames.push_back({subfolder_path, current_name, part1 + " - " + part2});
    }
    return result;
}

TrackValidationResult validate_album_tracks(Album& album, const std::string& file_naming_rule) {
    TrackValidationResult result;
    const std::string effective_rule = album.is_standalone_dir ? standalone_file_rule(file_naming_rule) : file_naming_rule;
    for (auto& t : album.tracks) {
        if (t.has_legacy_date_frames) result.legacy_tag_tracks.push_back(&t);

        std::string expected = build_expected_filename(t, effective_rule);
        if (t.filename == expected) continue;

        ProposedTrackRename rename{&t, expected, {}};
        std::string f_norm = t.filename;
        std::string e_norm = expected;
        auto remove_space_underscore = [](char c) { return c == '_' || c == ' '; };
        f_norm.erase(std::remove_if(f_norm.begin(), f_norm.end(), remove_space_underscore), f_norm.end());
        e_norm.erase(std::remove_if(e_norm.begin(), e_norm.end(), remove_space_underscore), e_norm.end());

        std::string reason = "Filename does not match rule: " + effective_rule;
        if (f_norm == e_norm) reason += " (Mismatch: Spaces vs Underscores)";
        rename.violations.push_back(reason);
        result.track_renames.push_back(rename);
    }
    return result;
}

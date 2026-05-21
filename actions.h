#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "models.h"

struct ProposedSubfolderRename {
    std::filesystem::path path;
    std::string current_name;
    std::string expected_name;
};

struct FolderValidationResult {
    std::vector<std::string> violations;
    std::string current_name;
    std::string expected_name;
    std::vector<ProposedSubfolderRename> subfolder_renames;
};

struct ProposedTrackRename {
    Track* track = nullptr;
    std::string expected_filename;
    std::vector<std::string> violations;
};

struct TrackValidationResult {
    std::vector<Track*> legacy_tag_tracks;
    std::vector<ProposedTrackRename> track_renames;
};

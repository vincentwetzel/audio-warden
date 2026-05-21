#pragma once

#include "actions.h"
#include "models.h"

void prompt_folder_actions(Album& album, const FolderValidationResult& result, bool is_dry_run, bool interactive);
void prompt_track_actions(const std::filesystem::path& folder_path, TrackValidationResult& result, bool is_dry_run, bool interactive);

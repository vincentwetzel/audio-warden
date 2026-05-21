#pragma once

#include <string>

#include "actions.h"
#include "models.h"

FolderValidationResult validate_album_folder(const Album& album);
TrackValidationResult validate_album_tracks(Album& album, const std::string& file_naming_rule);

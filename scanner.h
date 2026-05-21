#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "models.h"
#include "thread_safe_queue.h"

void discover_album_folders(
    const std::filesystem::path& library_path,
    ThreadSafeQueue<std::filesystem::path>& album_folders_queue);

Album parse_album_folder(
    const std::filesystem::path& album_path,
    const std::vector<std::string>& category_folders,
    const std::vector<std::string>& standalone_folders);

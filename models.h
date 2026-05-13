#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct Track {
    std::filesystem::path file_path;
    std::string filename;
    std::string extension;
    
    // TagLib Parsed Data
    std::string title;
    std::string artist;
    std::string album;
    unsigned int year = 0;
    int track_number = 0;
    
    // Quality Metrics
    int bitrate = 0;
    int sample_rate = 0;
    int bits_per_sample = 0;
    
    // Fixes
    bool has_legacy_date_frames = false;
};

struct Album {
    std::filesystem::path folder_path;
    std::vector<Track> tracks;
    bool is_multi_disc = false;
    bool is_category_album = false;
};
# System Architecture

AudioWarden is currently a single-binary C++17 CLI application with the scanner,
validator, prompter, and modifier behavior implemented in `main.cpp`. The
conceptual subsystem boundaries are still documented in `AGENTS.md`, but the
physical code has not yet been split into separate `scanner.cpp`,
`validator.cpp`, `prompter.cpp`, or `modifier.cpp` translation units.

## 1. Technology Stack

*   **Language:** Standard C++17, including `std::filesystem`, threads, mutexes,
    atomics, condition variables, and queues.
*   **Build System:** CMake 3.15+ with dependencies supplied by vcpkg manifest
    mode (`vcpkg.json`).
*   **Third-Party Libraries:**
    *   **TagLib (C++):** For reading and writing audio metadata securely across formats (MP3, FLAC, WAV).
    *   **nlohmann/json:** For parsing `rules.json`.
    *   **CLI11:** For robust command-line argument parsing and flag handling.

## 2. Core Data Models

To support batching and logic checks, the library is modeled in memory before any validation occurs.

```cpp
struct Track {
    std::filesystem::path file_path;
    std::string filename;
    std::string extension;
    
    // TagLib Parsed Data
    std::string title;
    std::string artist;
    std::string album;
    unsigned int year;
    int track_number;
    
    // Quality Metrics
    int bitrate;
    int sample_rate;
    int bits_per_sample;

    // Metadata cleanup checks
    bool has_legacy_date_frames;
};

struct Album {
    std::filesystem::path folder_path;
    std::vector<Track> tracks;
    bool is_multi_disc;
    bool is_category_album;
};
```

## 3. Runtime Inputs

AudioWarden currently reads two configuration files:

*   `settings.txt`: one library root per line. Blank lines and comments beginning
    with `#` are ignored.
*   `rules.json`: JSON rules consumed at runtime. The implemented checks use
    the `file_naming` template.

The CLI requires `--rules` / `-r` and supports:

*   `--interactive` / `-i`: prompt before applying batch file renames or tag
    saves.
*   `--dry-run` / `-d`: print approved changes without modifying files.

## 4. Current Execution Pipeline

1. **Initialization (`main.cpp`)**
   * Install a custom TagLib debug listener that detects legacy ID3 date frame
     warnings (`TDAT`, `TYER`, `TIME`) and suppresses raw TagLib warning output
     so prompts remain readable.
   * Parse CLI arguments via CLI11.
   * Load JSON rules and library roots from `settings.txt`.
2. **Discovery Thread**
   * Recursively traverses each configured library root with
     `std::filesystem::recursive_directory_iterator`.
   * Builds an alphabetized list of directories and queues album folders instead
     of individual files.
   * Treats directories as albums when they contain audio in the root, or when
     their only subdirectories are disc-like folders (`CD`, `Disc`, or `Vol`)
     containing audio.
   * Marks direct disc subfolders as processed children so multi-disc albums are
     handled as one album.
3. **Worker Threads**
   * Pop album folders and collect `.mp3`, `.flac`, and `.wav` tracks from the
     album root and recognized disc subfolders.
   * Use `TagLib::FileRef` to read title, artist, album, year, track number,
     bitrate, sample rate, FLAC bit depth, and legacy date-frame warnings.
   * Apply a hard-coded category-album heuristic for parent folders named
     `soundtracks`, `childrens`, or `various artists`.
   * Push populated `Album` objects into a thread-safe queue.
4. **Folder Validation and Prompts**
   * Builds expected standard album folder names from tag year, tag album name,
     and detected technical info: `YYYY - Album Name [Technical Info]`.
   * Builds category album folder names as `Album Name [YYYY]`.
   * Generates technical info blocks for FLAC, MP3, Opus, and other audio using
     available TagLib properties.
   * Normalizes album title punctuation for folder names, including converting
     common edition/version parentheticals to bracketed edition info and
     replacing colons with ` - `.
   * Detects missing, mismatched, or malformed technical-info blocks.
   * Detects multi-disc subfolder separator issues and proposes names using
     ` - ` instead of underscores or other separators.
5. **Track Validation and Prompts**
   * Builds expected filenames from the configured `file_naming` template,
     currently replacing `{TrackNumber}`, `{Title}`, and `{ext}`.
   * Sanitizes proposed title text by replacing colons with ` - `, collapsing
     double spaces, and replacing OS-invalid filename characters with
     underscores.
   * Reports space-vs-underscore differences as a specific mismatch reason when
     the normalized names otherwise match.
   * Reports legacy ID3v2 date frames as a batch warning.
6. **Approved Writes**
   * In interactive mode, approved folder, subfolder, and filename batches are
     applied with `std::filesystem::rename`.
   * After folder or subfolder renames, in-memory track paths are updated before
     file-level validation runs.
   * Approved legacy date-frame cleanup saves each affected file through
     TagLib, allowing TagLib to rewrite tags in the current format.
   * In dry-run mode, the same approved operations are printed but not written.

## 5. Planned Work

The following documented rule areas are not yet implemented in the current
single-file pipeline:

*   Configurable category-folder and disc-folder classification rules.
*   Standalone single detection.
*   `.lrc` detection, embedding, and deletion.
*   Embedded synced-lyrics offset adjustment.
*   Dedicated `Violation`, `ActionList`, rollback, and modifier abstractions.

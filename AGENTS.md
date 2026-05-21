# AudioWarden Conceptual Agents

To maintain a clean and modular C++ codebase, AudioWarden is divided into several conceptual "Agents" or Subsystems. Each subsystem has a single, strictly defined responsibility.

These are current design boundaries, with the implementation now split across
focused source files. The CLI bootstrap lives in `main.cpp`, shared data models
live in `models.h`, orchestration lives in `audio_warden.cpp`, discovery and tag
reading live in `scanner.cpp`, rule checks live in `validator.cpp`, and prompts
plus approved writes live in `prompter.cpp`.

## 1. The Scanner (`LibraryScanner`)
**Responsibility:** Discovery and categorization.
*   Recursively traverses the target directory using `std::filesystem`,
    streaming discovered album folders immediately to worker threads.
*   Currently identifies `.mp3`, `.flac`, and `.wav` files as valid audio tracks.
*   Currently identifies album folders before tag parsing so albums can be
    validated as single units.
*   Currently recognizes basic multi-disc structures when subfolders begin with
    `CD`, `Disc`, or `Vol`.
*   Currently marks category albums when any ancestor folder name matches a
    lower-cased entry in the `rules.json` `category_folders` list. If the rule is
    omitted, the fallback list is `soundtracks`, `childrens`, and
    `various artists`.
*   Currently marks folders named `singles`, `loose tracks`, or `standalone` as
    standalone directories, plus any lower-cased entry in the `rules.json`
    `standalone_folders` list, so their track filenames can omit the
    track-number prefix.
*   Planned: identify banned files (e.g., `.lrc`) and ignored files.
*   Planned: make disc-folder patterns configurable.
*   *Constraint:* Purely read-only. Parses paths and groups files logically into `Album` or `Track` objects.

## 2. The Validator (`RuleValidator`)
**Responsibility:** Inspection and violation detection.
*   Takes the parsed objects from the Scanner.
*   Uses `TagLib` to read embedded metadata and audio properties.
*   Currently reads track title, artist, album, year, track number, bitrate,
    sample rate, and FLAC bit depth.
*   Currently distinguishes original year from release year when
    `ORIGINALYEAR` or `ORIGINALDATE` is available, falling back to TagLib's
    standard year otherwise.
*   Currently cross-references filenames against the `rules.json`
    `file_naming` template.
*   Currently validates album folder names against standard album and category
    album conventions when enough tag data is available.
*   Currently checks multi-disc subfolder separators and proposes normalized
    names such as `Vol. 01 - Title`.
*   Currently detects legacy ID3v2 date-frame warnings (`TDAT`, `TYER`, `TIME`,
    `TORY`, `TRDA`) from TagLib debug output.
*   Planned: warn about likely re-release/remaster albums when the folder year
    is at least ten years earlier than the release year tag and the original
    year tag does not already match the folder year.
*   Planned: generate formal `Violation` objects (e.g., `MissingTechnicalInfo`, `BannedOffsetDetected`, `TrackNamingMismatch`).
*   *Constraint:* Purely read-only. It only suggests fixes; it does not apply them.

## 3. The Interrogator (`InteractivePrompter`)
**Responsibility:** User interaction and consent gathering.
*   Presents violations to the user via the CLI interface.
*   Groups related folder, subfolder, filename, and legacy-tag violations into
    directory-level **Batch Prompts**.
*   Accepts user input to approve or skip the suggested batch fixes.
*   Planned: manual adjustment of proposed fixes and formal `ActionList` output.
*   *Constraint:* Should not interact with the filesystem directly. In the
    current pipeline implementation, prompting and execution are still adjacent
    and should be separated during modularization.

## 4. The Modifier (`FileSystemModifier` / `TagModifier`)
**Responsibility:** Execution of approved changes.
*   The **only** agent permitted to write data.
*   Currently executes approved album folder and multi-disc subfolder renames
    with `std::filesystem::rename`.
*   Currently executes approved file renames with `std::filesystem::rename`.
*   Currently executes approved legacy tag cleanup with `TagLib::FileRef::save()`.
*   Planned: execute approved original-year tag updates and optional album
    edition suffix updates with TagLib property/tag writes.
*   Planned: receive heavily vetted and user-approved `ActionList` commands.
*   Planned: synced-lyrics updates, folder changes, rollback capabilities, and stronger mid-batch recovery.

## Communication Flow
1. `LibraryScanner` -> `Data Models`
2. `Data Models` -> `RuleValidator` -> `Violations`
3. `Violations` -> `InteractivePrompter` -> `Approved Actions`
4. `Approved Actions` -> `Modifier` -> **Disk**

## Current Threading Model

The implemented scanner uses one discovery thread to queue album folder paths
and a worker pool sized from `std::thread::hardware_concurrency()` to read tags
for all tracks in each album. Parsed `Album` objects are returned through a
thread-safe queue. The main thread presents folder-level violations immediately
as each album is popped from the queue, then proceeds to file-level and
legacy-tag violations after the folder pass completes.

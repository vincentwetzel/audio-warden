# AudioWarden Rules & Conventions

This document outlines the file naming, tagging, and folder structure rules enforced by AudioWarden.

## Implementation Status

AudioWarden currently enforces a focused subset of these rules:

*   Scans `.mp3`, `.flac`, and `.wav` files under each path in `settings.txt`.
*   Groups tracks by detected album folder before prompting.
*   Reads tags and audio properties with TagLib, including year and FLAC bit
    depth where available.
*   Reads original-year metadata from `ORIGINALYEAR` or `ORIGINALDATE`, while
    also retaining the normal release year reported by TagLib.
*   Validates track filenames against `rules.json` `file_naming`.
*   Validates standard album folders as `YYYY - Album Name [Technical Info]`
    when year and album tags are available.
*   Validates category album folders as `Album Name [YYYY] [Technical Info]`
    when an ancestor folder matches `rules.json` `category_folders`.
*   Allows standalone directories named `singles`, `loose tracks`, or
    `standalone` to omit the track-number prefix from filenames.
*   Detects basic multi-disc album structures using `CD`, `Disc`, and `Vol`
    subfolder prefixes.
*   Suggests multi-disc subfolder separator cleanup, such as changing
    underscores to ` - `.
*   Detects legacy ID3v2 date frames (`TDAT`, `TYER`, `TIME`, `TORY`, `TRDA`)
    when TagLib reports them.
*   Retains release-year metadata separately so future re-release/remaster
    checks can compare it with original-year metadata.
*   In interactive mode, can batch-rename folders, disc subfolders, files, and
    save affected tags after user approval.

Configurable disc-folder classification, loose standalone-file detection,
banned `.lrc` handling, original-year repair prompts, and synced-lyrics offset
repair are still planned rule areas.

## Core Philosophy: User Approval Required

AudioWarden operates on a strict **no autonomous changes** policy. The application will never automatically rename folders, modify files, or alter tags on its own.

*   **Interactive Prompts:** Every violation detected will result in a suggested fix that the user must explicitly approve.
*   **Batch Approvals:** To maintain a smooth user experience, the application will group logical changes. For example, if an entire album has incorrect track numbers, the user can approve the batch renaming of the entire album with a single confirmation, rather than confirming each file individually.
*   **Dry Run:** With `--dry-run`, AudioWarden prints approved changes without
    modifying files.
*   **Non-Interactive Runs:** Without `--interactive`, AudioWarden reports
    violations but does not apply the currently implemented rename or tag-save
    actions.

## File Naming

### Standard Album Tracks
The normal track name for items belonging to a cohesive album must follow the standard track number and title format:

`{TrackNumber} - {Title}.{ext}`

**Example:**
`01 - my song name.mp3`

### Current Filename Template Support

The implemented filename validator reads the `file_naming` value from
`rules.json`. The currently supported placeholders are:

*   `{TrackNumber}`: two-digit track number from embedded tags, or `00` when
    unavailable.
*   `{Title}`: embedded title, or `Unknown Title` when unavailable.
*   `{ext}`: the file extension without the leading dot.

Before proposing a filename, AudioWarden replaces colons in the title portion
with ` - `, collapses double spaces, and replaces OS-invalid filename
characters with underscores. This includes angle brackets, double quote, pipe,
question mark, asterisk, forward slash, and backslash.

### Singles & Non-Album Tracks
There will sometimes be files that are **not** a part of an album (e.g., loose singles, sound bites, or independent tracks). These files are permitted to ignore the strict album naming rule.

For these standalone files, the `{TrackNumber} - ` prefix is not required, and they will not trigger a ruleset violation for missing a track number.

**Status:** Basic standalone-directory support is implemented for album folders
named `singles`, `loose tracks`, or `standalone`. In those directories,
AudioWarden removes a configured `{TrackNumber} - ` prefix from the expected
filename before validation. Loose standalone files outside those directory names
are not specially detected yet.

## Folder Structure

### Album Folders
**Identifying an Album Folder:**
A folder is generally considered an album folder if it contains audio tracks and has **no subfolders**. 

*Exception for Multi-Disc Albums:* If a folder contains subfolders, it is **only** considered an album folder if those subfolders are exclusively disc/CD directories. Examples of permitted disc directories include:
* `CD01` / `CD 01`
* `Disc 1` / `Disc 01`

If the only subdirectories present are disc folders, the parent directory is treated as the main album folder and must follow the **Standard Album Folder** format described below.

#### Album Folder Naming Conventions

##### Standard Album Folders (within Artist directories)
The primary format for album folders is:

`YYYY - Album Name [Edition Info][Technical Info]`

*   **[Edition Info]**: Optional. Describes the version of the album (e.g., `[Deluxe Edition]`, `[Remastered]`).
*   **[Technical Info]**: **Required**. Describes the audio quality using the
    format generated by the current implementation.
    *   **FLAC example:** `[FLAC 16bit 44.1kHz]`
    *   **MP3 CBR example:** `[MP3 320kbps]`
    *   **MP3 VBR example:** `[MP3 VBR ~245kbps]`
    *   **Other codec fallback example:** `[wav 44.1kHz 1411kbps]`

**Full Example:** `2002 - Sentimento [Premium Edition] [FLAC 16bit 44.1kHz]`

**Automatic Generation of Technical Info:**
If the `[Technical Info]` is missing or incomplete (e.g., `[mp3]`), AudioWarden will analyze the audio files within the folder to determine the correct codec, bit depth, sample rate, and average bitrate. It will then suggest the correctly formatted folder name during an interactive scan.

**Status:** Basic folder naming validation and technical-info generation are
implemented for detected album folders. The current implementation derives
folder names from embedded original-year metadata when available, falling back
to the release year tag, then prompts before applying a folder rename in
interactive mode. It normalizes common edition/version parentheticals into
brackets, replaces colons with ` - `, normalizes typographic quotes, and
replaces OS-invalid folder characters with underscores.

##### Category Album Folders (within non-Artist directories)
If an album appears in a category folder (e.g., `soundtracks/`, `childrens/`) instead of an artist folder, an alternative naming format is permitted:

`Album Name [YYYY] [Technical Info]`

**Example:** The folder `Disney Travel Songs [1994] [FLAC 16bit 44.1kHz]` is valid inside `J:\Audio\Music\childrens\`.

**Status:** Category album validation is implemented for parent folders named
by `rules.json` `category_folders`. The current repository default includes
`soundtracks`, `childrens`, `various artists`, and `video game`. Matching is
case-insensitive and checks the album folder's ancestors.

### Multi-Disc Subfolders

AudioWarden currently treats album subfolders beginning with `CD`, `Disc`, or
`Vol` as disc folders. When all subfolders are disc folders, the parent is
processed as one multi-disc album.

Disc subfolder names may include a title after the disc number, but the
separator should be ` - `.

**Example:** `Vol. 01 - Live Set`

**Status:** Basic multi-disc detection and separator cleanup prompts are
implemented. The accepted disc-prefix patterns are currently hard-coded.
AudioWarden also trims likely disc suffixes from multi-disc album tags before
building the parent folder name, so tags such as `Album Title, Disc 1` can still
produce a parent folder named for `Album Title`.

## Banned Files

### Lyric Files (.lrc)
Standalone lyric files (such as `.lrc`) are strictly banned from the audio library. 

**Resolution & Embedding:**
If AudioWarden detects an `.lrc` file associated with an audio track (e.g., `song.flac` and `song.lrc` residing in the same directory), the application will flag this as a violation. 

During interactive mode, the app will prompt the user for permission to read the contents of the `.lrc` file and embed the lyrics directly into the metadata of the corresponding audio track. Upon successful embedding, the standalone `.lrc` file should be deleted.

**Status:** `.lrc` detection and embedding are planned. The current scanner only
uses `.mp3`, `.flac`, and `.wav` files when detecting and processing albums.

## Embedded Lyrics & Subtitles

### Time Offsets
Embedded subtitles and synced lyrics are not allowed to use global time offsets. For example, it is common for files to have a line like:

`[offset:-5000]`

This is banned. 

**Resolution & Adjustment:**
If a file has an offset like this, the script will prompt the user to allow it to update the file so that it bumps all timestamps in the file by the offset amount and then removes the offset line.

**Status:** Synced-lyrics offset detection and adjustment are planned.

## Legacy ID3 Date Frames

Legacy ID3v2 date frames (`TDAT`, `TYER`, `TIME`, `TORY`, and `TRDA`) are
treated as metadata cleanup warnings. When TagLib reports one of these frames
while reading a file, AudioWarden groups the affected tracks by directory and
prompts in interactive mode before saving them through TagLib.

In `--dry-run` mode, approved tag updates are printed without writing changes.

## Original Year vs Release Year

AudioWarden treats the album folder year as the original album year. The scanner
reads `ORIGINALYEAR` or `ORIGINALDATE` when available and retains the normal
TagLib year separately as the release year.

**Status:** Re-release/remaster detection and interactive `ORIGINALYEAR` repair
prompts are planned. The current validator uses the original-year value when
building expected folder names, but it does not yet offer tag repair actions.

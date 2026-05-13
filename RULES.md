# AudioWarden Rules & Conventions

This document outlines the file naming, tagging, and folder structure rules enforced by AudioWarden.

## Implementation Status

AudioWarden currently enforces a focused subset of these rules:

*   Scans `.mp3`, `.flac`, and `.wav` files under each path in `settings.txt`.
*   Reads tags and audio properties with TagLib.
*   Validates track filenames against `rules.json` `file_naming`.
*   Detects configured `forbidden_characters` in filenames.
*   Detects legacy ID3v2 date frames (`TDAT`, `TYER`, `TIME`) when TagLib reports
    them.
*   In interactive mode, can batch-rename files and save affected tags after
    user approval.

Folder naming, album classification, banned `.lrc` handling, and synced-lyrics
offset repair are still planned rule areas.

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

Before proposing a filename, AudioWarden removes configured forbidden characters
from the title portion and replaces OS-invalid filename characters with
underscores. This includes angle brackets, colon, double quote, pipe, question
mark, asterisk, forward slash, and backslash.

### Singles & Non-Album Tracks
There will sometimes be files that are **not** a part of an album (e.g., loose singles, sound bites, or independent tracks). These files are permitted to ignore the strict album naming rule.

For these standalone files, the `{TrackNumber} - ` prefix is not required, and they will not trigger a ruleset violation for missing a track number.

**Status:** Standalone single detection is not implemented yet. Current filename
validation is grouped by parent folder and applies the configured naming rule to
each scanned audio file.

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
*   **[Technical Info]**: **Required**. Describes the audio quality. The recommended format is `[codec bitdepth-samplerate bitrate]`.
    *   **Example:** `[flac 16-44 912kbps]`
    *   **For VBR (Variable Bitrate) files:** Since there is no single bitrate, the format should indicate VBR and may include the average bitrate, prefixed with a tilde (`~`). Example: `[mp3 VBR ~245kbps]`.

**Full Example:** `2002 - Sentimento [Premium Edition][flac 16-44 912kbps]`

**Automatic Generation of Technical Info:**
If the `[Technical Info]` is missing or incomplete (e.g., `[mp3]`), AudioWarden will analyze the audio files within the folder to determine the correct codec, bit depth, sample rate, and average bitrate. It will then suggest the correctly formatted folder name during an interactive scan.

**Status:** Folder naming validation and technical-info generation are planned.
The current pipeline includes a placeholder folder phase but does not propose
folder renames.

##### Category Album Folders (within non-Artist directories)
If an album appears in a category folder (e.g., `soundtracks/`, `childrens/`) instead of an artist folder, an alternative naming format is permitted:

`Album Name [YYYY]`

**Example:** The folder `Disney Travel Songs [1994]` is valid inside `J:\Audio\Music\childrens\`.

## Banned Files

### Lyric Files (.lrc)
Standalone lyric files (such as `.lrc`) are strictly banned from the audio library. 

**Resolution & Embedding:**
If AudioWarden detects an `.lrc` file associated with an audio track (e.g., `song.flac` and `song.lrc` residing in the same directory), the application will flag this as a violation. 

During interactive mode, the app will prompt the user for permission to read the contents of the `.lrc` file and embed the lyrics directly into the metadata of the corresponding audio track. Upon successful embedding, the standalone `.lrc` file should be deleted.

**Status:** `.lrc` detection and embedding are planned. The current scanner only
queues `.mp3`, `.flac`, and `.wav` files.

## Embedded Lyrics & Subtitles

### Time Offsets
Embedded subtitles and synced lyrics are not allowed to use global time offsets. For example, it is common for files to have a line like:

`[offset:-5000]`

This is banned. 

**Resolution & Adjustment:**
If a file has an offset like this, the script will prompt the user to allow it to update the file so that it bumps all timestamps in the file by the offset amount and then removes the offset line.

**Status:** Synced-lyrics offset detection and adjustment are planned.

## Legacy ID3 Date Frames

Legacy ID3v2 date frames (`TDAT`, `TYER`, and `TIME`) are treated as metadata
cleanup warnings. When TagLib reports one of these frames while reading a file,
AudioWarden groups the affected tracks by directory and prompts in interactive
mode before saving them through TagLib.

In `--dry-run` mode, approved tag updates are printed without writing changes.

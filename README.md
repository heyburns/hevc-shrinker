# HEVC-Shrinker

HEVC-Shrinker is a Bash script that re-encodes video files to HEVC (H.265) using FFmpeg and Avisynth+ filtering to improve compression, compares the size of the re-encoded file to the original, and retains the smaller version. It tracks processed files using a SQLite database. Designed for use in Git Bash on Windows, this script automates video optimization while preserving quality and reducing file sizes.

**NOTE** THIS SCRIPT IS UNFORGIVING AND DELETES FILES PERMANENTLY WITHOUT PROMPTING, SO BE SURE YOU UNDERSTAND WHAT IT DOES AND TEST IT BEFORE USING ON YOUR COLLECTION. While this script usually delivers visually transparent transcodes at the settings I've selected, it is designed for batch use on very large collections where file size and format consistency are the primary considerations and you can live with the occasional mistake. If quality is your top consideration, you're better off handling your files individually. Use at your own risk.

## Features

- **Recursive File Processing:** Scans directories for common video formats.
- **Video Re-encoding:** Uses FFmpeg with Avisynth+ filtering to re-encode video to HEVC.  
  - For most file types, the script loads video and audio separately and then combines them with `AudioDub()`.  
  - **WMV Files:** Due to A/V sync issues with the standard method, WMV files are loaded using `DirectShowSource("file.ext")` in a single step (which automatically loads both video and audio). This bypasses the need for stream identifiers.
  - If videos are UHD they are downscaled to 1080. If they are high frame rate (i.e., 50 or 60fps) the frame rate is halved - frame rate has minimal effect on file size but encoding speed is doubled with minimal visual difference.
- **Audio Processing:**  
  - If the audio is already AAC, it is copied directly.  
  - Otherwise, audio is re-encoded using QAAC.
- **Cover Art Detection:**  
  The script searches for cover art in the same directory as the video file. It first checks for a file named `poster.jpg`, `poster.png`, or `poster.webp`. If none is found, it then looks for an image file with the same base name as the video (e.g., for `video.mp4`, it looks for `video.jpg`, `video.png`, or `video.webp`).
- **Muxing:** Combines the processed video, audio, and optional cover art into an MKV container.
- **Size Comparison:**  
  For non-WMV files, after muxing the original file is remuxed to MKV for a fair size comparison—the smaller file is kept.  
  For WMV files, size comparison is skipped and the HEVC output is always used (due to incompatibility of WMV with Matroska containers).
- **Error Logging:** Logs errors to `error.log` and continues processing subsequent files.
- **Database Tracking:** Uses SQLite to record processed files, ensuring that files are not re-processed.

## Dependencies

### Avisynth+

Ensure Avisynth+ is installed and correctly configured. 

The script leverages Avisynth+ for pre-filtering via several plug-ins:

  [Avisynth+ on GitHub](https://github.com/AviSynth/AviSynthPlus)
- [LSMASHSource](http://avisynth.nl/index.php/LSMASHSource) For loading video and audio (used for non-WMV files).
- [DirectShowSource](http://avisynth.nl/index.php/DirectShowSource) Used to load WMV files in a single step (which avoids A/V sync issues).
- [LRemoveDust](https://forum.doom9.org/showthread.php?t=176245) A simple noise reduction function that is moderately destructive to fine detail but improves compressibility substantially.

### FFmpeg
- **Purpose:** Performs video and audio encoding/decoding, muxing, and remuxing.
- **Recommended Build:** Media Autobuild Suite (a user-friendly FFmpeg build with Avisynth+ support).
- **Download:** [Media Autobuild Suite on GitHub](https://github.com/m-ab-s/media-autobuild_suite)

### QAAC
- **Purpose:** Re-encodes audio to AAC when needed.
- **Note:** QAAC requires iTunes to be installed on Windows unless an alternative is available.
- **Download:** [QAAC GitHub Repository](https://github.com/nu774/qaac)

### SQLite3
- **Purpose:** Tracks processed files using a local SQLite database.
- **Download:** [SQLite Download Page](https://www.sqlite.org/download.html)

### MKVToolNix
- **Purpose:** Used for advanced MKV container operations (if needed in the workflow).
- **Download:** [MKVToolNix Official Site](https://mkvtoolnix.download/)

### Git for Windows
- **Purpose:** Provides a Git Bash environment required to run this script.
- **Download:** [Git for Windows](https://gitforwindows.org/)

## Configuration & Adjustments

- **x265 Encoding Quality (CRF):**  
  - **Default Value:** `23`  
  - Increase the CRF value to further compress the video (resulting in a smaller file at the expense of quality). I wouldn't go higher than 28 unless you really don't care about quality.
  
- **QAAC Audio Quality:**  
  - **Default QAAC VBR Setting:** `100`  
  - Lower the QAAC VBR value to increase audio compression (which may reduce audio quality).

These parameters can be adjusted in the script's configuration section at the top.

## Assumptions About the User's System

- **Operating Environment:** The script is intended for use in Git Bash on Windows. If you want to run it on a *nix system, it may require some path modifications. Qaac and AVS will need to run in a wine environment, or you can modify the script to use ffmpeg's built-in AAC encoder. If you're running *nix, I assume you know how to do these things.
- **Executable PATH:** The following binaries must be in your system's PATH:
  - `ffmpeg` and `ffprobe` (with Avisynth+ support, as provided by Media Autobuild Suite)
  - `sqlite3`
  - `qaac` (note: requires iTunes)
  - `mkvmerge`

## Usage

1. **Download and Setup:**  
   Clone this repository or download the script (e.g., `hevc-shrinker.sh`) into the directory containing your video files.
   Copy LRemoveDust.avsi, LimitChange.avsi, masktools2.dll, RemoveGrainHD.dll, and RGTools.dll to your AVISynth plugins directory (typically C:\Program Files (x86)\AviSynth+\plugins64+)
3. **Make Executable:**  
   Ensure the script is executable (only for *nix filesystems):
   ```bash
   chmod +x hevc-shrinker.sh
4. Run the Script:
   Open Git Bash, navigate to the directory, and run:
   ```bash
   ./hevc-shrinker.sh
5. Error Logging:
   Errors encountered during processing are logged to error.log. Review this file for troubleshooting.

## Contributing
Contributions, improvements, and bug fixes are welcome, but I make no promises and provide no support! I may or may not get around to it. 

## License
This project is licensed under the GNU General Public License v2 (GPL-2.0). See GPL-2.0 License for details.

## Disclaimer
HEVC-Shrinker is provided as-is, without any warranty. Use at your own risk. It is recommended to test the script on sample files before processing important data.

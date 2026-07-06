# HEVC Video Shrinker

This is a desktop application designed to help you intelligently clean up and shrink your video libraries. If you have a large collection of movies or TV recordings taking up hundreds of gigabytes on your hard drive, this tool will batch-convert your collection into highly efficient H.265 (HEVC) videos in an MKV container, reducing file size while keeping the visual quality intact. While it aims for visual transparency, this app prioritizes file size over quality and should not be used on collections where quality is the paramount concern, e.g. archival material.

The interface is built using C++ and Qt, which means it starts up quickly and runs natively on your system without any heavy web frameworks or background runtimes.

## Why use this instead of other converters?

Many video converters will blindly re-process every file you give them, which takes hours and degrades quality. This app is designed to be much smarter about how it handles your library:

*   In the case that the hevc file comes out larger than the original (it's rare, but it does happen), the transcoded file is deleted and the original is remuxed into mkv. This ensures that your files will always either shrink or stay the same and never grow.
*   It remembers what it has done by creating a small database file in your chosen folder. It calculates a unique fingerprint for each video so that if you run a scan later, it will instantly skip files that have already been shrunk.
*   If a video is already encoded in H.265 and has compatible AAC audio, the app recognizes this and skips it entirely.
*   If a video is already in H.265 format but has an older audio format (like AC3 or DTS), it will only transcode the audio to AAC and copy the video stream directly. This avoids unnecessary video re-compression, preserves 100% of the video quality, and finishes in seconds rather than hours.
*   Automatic deinterlacing: It can automatically clean up old TV recordings by automatically detecting combing and de-interlacing them on the fly. It can also downscale bulky 4K videos to a standard 1080p resolution to save even more space.
*   It can handle arcane formats like .flv, .avi, .asf, .wmv, and others, although they are always re-encoded and not size-checked against the source.
*   Even though the goal is file size, quality is still important. It uses ffmpeg options that aim to help with visual transparency, i.e., no visual quality loss to the human eye. It prefers the higher quality fdk_aac for audio if your ffmpeg is compiled with that option, and it falls back to ffmpeg's default encoder if fdk_aac isn't present.

## Prerequisites

Before running the application, you need to make sure you have FFmpeg and FFprobe installed. They do the actual heavy lifting of reading, analyzing, and writing the video files:

*   **Windows**: Download FFmpeg and FFprobe, and make sure the executables (ffmpeg.exe and ffprobe.exe) are either in your system's PATH, or placed in the same folder as this application's executable.
*   **Linux**: Install them via your package manager (e.g., `sudo apt install ffmpeg` on Debian/Ubuntu, `sudo pacman -S ffmpeg` on Arch, or emerge `media-video/ffmpeg` on Gentoo).
*   **macOS**: Can be installed via Homebrew (`brew install ffmpeg`), though please see the compatibility note below.

## How to use the application

1.  Launch the application.
2.  Click Browse and select the folder containing your videos.
3.  Click Scan Directory. The app will analyze all files in the folder and show you which ones are already compliant and which ones are pending.
4.  Adjust your settings on the right sidebar:
    *   CRF Quality: A slider to adjust compression. A higher value means smaller files but lower quality. The default is 28, which is generally the sweet spot for H.265.
    *   CPU Preset: Controls the CPU processing effort. Slower presets (like 'slow' or 'slower') will analyze the video in much more detail to produce higher quality output, but this extra detail will result in larger file sizes and much longer processing times. Faster presets process quickly but at the expense of quality. Medium is a good compromise.
    *   Filters: You can choose to downscale 4K files or enable double frame-rate de-interlacing (de-bob).
5.  Click Start Queue. The app will process your videos one by one, showing you real-time progress, speed, and estimated completion times.
6.  The original files will be safely moved to a hidden folder called .Trash inside your directory, so you can review the results before deleting them permanently. Any errors will be moved to .Errors.

## Building from source

To compile the application yourself, you will need a C++ compiler supporting C++17, CMake (version 3.16 or higher), and the Qt6 SDK (including Core, Widgets, and Sql modules).

### Windows
Open your terminal in the project root directory and run:
```bash
cmake -B build -S .
cmake --build build --config Release
```
If you are using MinGW (common with Qt installations on Windows), specify the makefile generator:
```bash
cmake -G "MinGW Makefiles" -B build -S .
cmake --build build --config Release
```

### Linux
Make sure you have Qt6 development packages installed:
*   **Debian/Ubuntu**: `sudo apt install build-essential cmake qt6-base-dev libqt6sql6-sqlite`
*   **Arch Linux**: `sudo pacman -S base-devel cmake qt6-base`
*   **Gentoo**: emerge `dev-build/cmake dev-qt/qtwidgets dev-qt/qtsql` (ensure the sqlite USE flag is enabled for Qt)

Then run the compilation commands in the root directory:
```bash
cmake -B build -S .
cmake --build build
```
Once compiled, you can launch the application with `./build/hevc_shrinker`.

### macOS Compatibility Note
While the code is written in cross-platform C++ and Qt6 and should theoretically build and run on macOS, it is **completely unsupported and untested** on Mac. The author does not own a Mac to verify functionality, package app bundles, or troubleshoot macOS-specific path/dependency issues.

## Packaging and distribution

### Windows
We have included two ways to package this application for distribution on Windows:

#### Portable version (ZIP)
If you want to run the app from a thumb drive or share it easily without an installer:
1. Compile the project in release mode.
2. Run the packaging script:
   ```powershell
   python package_portable.py
   ```
This script will collect the compiled executable, the required Qt6 libraries, compiler runtimes, and the embedded icon into a ZIP archive located at dist/hevc_shrinker_portable_win64.zip. You can drop ffmpeg.exe and ffprobe.exe directly inside this zip folder to make it fully self-contained.

#### Installer version
We have also included an Inno Setup script (setup.iss) to create a standard Windows installer wizard. Opening setup.iss in Inno Setup and compiling it will generate a setup executable in the dist/ folder. The installer sets up the program in Program Files, creates optional desktop shortcuts, and includes a standard uninstaller.

### Linux (Future Plans)
When testing on Linux, we plan to support packaging the application into `.deb` (for Debian/Ubuntu), `.rpm` (for Fedora/RedHat), and `AppImage` formats to make installation and distribution across different Linux distributions as easy and seamless as possible.

## License

This project is open-source and released under the GNU General Public License version 3 (GPL v3). See the [LICENSE](file:///c:/Users/Alan/Desktop/hevc_win/LICENSE) file in the root directory for the full terms and conditions.

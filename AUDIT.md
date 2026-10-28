**HEVC Video Shrinker audit — 27 October 2026**

This is a historical review of the code before the subsequent fixes and features. Findings and line references describe that earlier snapshot. See README.md and the regression tests for current behavior. The recommendation to invalidate path-based processing history was superseded by the owner's instruction to preserve the original skip rules.

The application is small enough to improve without a rewrite. Fix file replacement, database locking, stream selection, and cancellation before expanding it. For the stated priorities, the strongest tuning candidates are a faster preset with a higher CRF, optional light cleanup, explicit audio budgets, and avoiding unnecessary processing. AviSynth+ fits as an optional input backend.

This is an audit; application source and existing media were not modified. The review covers the C++/Qt application, build and packaging files, existing encoding experiments, and the untracked AviSynth prototype scripts. Reproductions used disposable files and an isolated database under `/tmp/hevc-audit-harness`. “Reproduced” below means observed on this Linux installation; “inspection” means established from the code path without reproducing the complete failure. Windows/macOS packaging was inspected, not executed.

**Findings requiring attention first**

1. **P1 — Existing outputs can be permanently overwritten. Reproduced.** [transcodeworker.cpp:429](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:429), [transcodeworker.cpp:549](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:549), [transcodeworker.cpp:620](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:620).

   `clip.mp4` and `clip.avi` both use `clip.tmp_out.mkv` and `clip.mkv`. Concurrent jobs can therefore write the same temporary file. Even sequential processing deletes an existing destination before promotion; the remux fallback writes directly to the destination with `-y`. A fixture containing an unrelated `clip.mkv` was overwritten. Reserve unique job temporary paths and final paths before dispatch; reject or resolve collisions without deleting unrelated files. Include case-insensitive collisions on Windows and collisions with other queued inputs. Exclude leftover job files from scanning.

2. **P1 — Failure to preserve an original causes permanent deletion. Reproduced.** [transcodeworker.cpp:557](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:557), [transcodeworker.cpp:601](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:601).

   Both success paths call `QFile::remove(filepath)` when moving to `.Trash` fails. A fixture with an unavailable trash directory lost its original while the job reported completion. This contradicts the README's preservation promise. Remove this fallback. Use a recoverable commit sequence: validate temporary output, reserve destination, move original to a unique backup, promote output, then record completion. If promotion fails, restore the original. Record recovery information for interruption between steps. The timestamp suffix used for trash collisions is not guaranteed unique either.

3. **P1 — Concurrent database writers can bypass SQLite locking. Reproduced.** [databasemanager.cpp:85](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/databasemanager.cpp:85).

   Unix connections append `?nolock=1`, although the database is now in the user's profile and multiple workers write independently. An isolated test acquired `BEGIN IMMEDIATE` through this class and then acquired a competing write transaction through a second connection to the same database. Corruption is a documented consequence when writes are not serialized. Use ordinary local-file connections with locking, a busy timeout, checked transactions, and optionally WAL for a suitable local filesystem. [SQLite URI documentation](https://www.sqlite.org/uri.html).

   There is a second portability defect: Qt reports `Unsupported option 'QSQLITE_OPEN_URI_ARGUMENTS'`. The documented name is `QSQLITE_OPEN_URI`. Whether URI handling still works depends on the linked SQLite configuration; it did here. The simplest fix is to remove this URI workaround. [Qt SQLite options](https://doc.qt.io/qt-6/sql-driver.html#qsqlite-for-sqlite-version-3-and-above).

4. **P1 — “Success” does not validate the resulting video. Inspection.** [transcodeworker.cpp:520](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:520), [transcodeworker.cpp:812](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:812).

   The acceptance criteria are exit code zero and file existence. There is no output probe, expected-stream check, duration comparison, or explicit `NormalExit` check. A decodable partial input can produce a successful but incomplete output. Before touching originals, check process exit status, nonempty output, expected streams, dimensions, duration/timeline tolerances, and required metadata. Keep full decode verification optional because it costs time. Cancellation must also prevent committing a just-finished output unless the commit has already entered a defined noninterruptible phase.

5. **P1 — Metadata can describe a different video stream from the one being encoded. Reproduced.** [transcodeworker.cpp:127](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:127), [transcodeworker.cpp:452](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:452).

   Every video stream overwrites `VideoMetadata`, leaving the last stream's dimensions, codec, and frame rate. Encoding and filtering target `v:0`. A two-video fixture with a 320×180 first stream and a 160×120 second stream was reported as 160×120. Cover art can create a similar mismatch. Select a real video stream explicitly, excluding attached pictures, retain its index, and use that index consistently for probing, interlace detection, filters, encoding, and preview. Define a policy for additional video streams instead of assuming every extra stream is cover art.

6. **P1 — Aborting leaves active jobs stranded; thread completion also has a lifetime race. Abort failure reproduced; lifetime race found by inspection.** [mainwindow.cpp:883](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/mainwindow.cpp:883), [mainwindow.cpp:1084](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/mainwindow.cpp:1084), [mainwindow.cpp:814](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/mainwindow.cpp:814), [transcodeworker.cpp:265](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:265).

   Abort disconnects signals before the worker can report `Cancelled`. The fixture remained `Processing`, with zero files queued to restart. Update job state explicitly or keep result delivery connected until cancellation finishes. Waiting for workers on the GUI thread also freezes the interface while probes/remuxing finish.

   The custom `finishedSignal` is emitted before `run()` returns, and its handler schedules deletion. The worker destructor only sets a flag. Use Qt's actual `QThread::finished` for lifetime cleanup and join where required; deleting a running subclassed QThread can crash. Also avoid resetting a pending stop request to true at the start of `run()`. [Qt thread lifetime documentation](https://doc.qt.io/qt-6/qthread.html#dtor.QThread).

**Other correctness and reliability findings**

7. **P2 — Skip decisions ignore transformations and changed files. Reproduced for replaced content; remaining paths inspected.** [mainwindow.cpp:637](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/mainwindow.cpp:637), [transcodeworker.cpp:62](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:62).

   A history match is based solely on path. Replacing a completed output with unrelated H.264 content still resulted in `Completed` and an empty queue. The stored hash belongs to the original input and is never checked. Codec-compliant MKVs are also skipped before downscale/deinterlace decisions, so an HEVC/AAC 4K file bypasses requested downscaling. This would likewise bypass future crop/cleanup requests.

   Cache source metadata separately from the decision to process. Validate completed output identity using size/mtime and a suitable fingerprint when needed, retain the settings/filter signature, and provide explicit “process selected files again.” Codec compliance should not override an explicitly requested transformation. Silent HEVC MKVs currently fail compliance because AAC is required, despite the worker treating absent audio as acceptable.

8. **P2 — The first audio track controls every audio track. Reproduced.** [transcodeworker.cpp:150](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:150), [transcodeworker.cpp:426](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:426), [transcodeworker.cpp:484](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:484).

   HEVC + AAC + AC3 remained HEVC + AAC + AC3 after processing, and the compliance function accepts any HEVC plus any AAC. In the opposite order, already-AAC tracks are unnecessarily recompressed. Store metadata per stream and choose `-c:a:N` independently. If AAC is a requirement, test every selected audio stream. If saving space is the real requirement, expose track selection and bitrate/channel policy rather than making AAC a universal proxy for a compact file.

9. **P2 — Ordinary MP4 subtitles cause otherwise valid jobs to fail. Reproduced.** [transcodeworker.cpp:496](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:496).

   `-c:s copy` cannot put MP4 `mov_text` subtitles into Matroska. The fixture failed with “Subtitle codec mov_text … is not supported” and its original was moved to `.Errors`. Plan subtitle handling by codec and destination: convert supported text subtitles to a Matroska-compatible format, preserve compatible subtitle codecs, and report unsupported streams before encoding. Apply the same compatibility checks to the fallback remux, whose `-map 0` also includes potentially unsupported data streams. Infrastructure/option failures should leave source media in place.

10. **P2 — Rotated videos receive the wrong display aspect ratio. Reproduced.** [transcodeworker.cpp:113](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:113), [transcodeworker.cpp:506](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:506).

    A 320×180 source carrying a 90° display rotation became 180×320 pixels but was forced to display at 16:9 with SAR 256:81. FFmpeg autorotation and the app's original-DAR override disagree. Probe the display matrix, calculate geometry after rotation, and derive the final SAR/DAR from the transformed clip. Reusing the original `-aspect` would also undo the intended display shape after future cropping.

11. **P2 — Remuxing and analysis have inappropriate timeout/cancellation behavior. Inspection.** [transcodeworker.cpp:166](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:166), [transcodeworker.cpp:548](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:548).

    Bare `waitForFinished()` has a 30-second default timeout. A legitimate large remux on a network drive can be marked failed, removed, and its source moved to `.Errors`. Probe timeout/failure is treated as noncompliance or progressive content. Unify subprocess handling, check errors and exit status, distinguish unknown from false, and support cancellation throughout analysis and remuxing. Cache capability detection once per executable instead of running `ffmpeg -encoders` for every file.

12. **P2 — Workspace SQL matching can affect another folder; migration can hide failures. Wildcard defect reproduced.** [databasemanager.cpp:263](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/databasemanager.cpp:263), [databasemanager.cpp:435](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/databasemanager.cpp:435), [databasemanager.cpp:459](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/databasemanager.cpp:459), [databasemanager.cpp:378](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/databasemanager.cpp:378).

    Bound parameters prevent SQL injection but do not escape LIKE wildcards. Resetting `/tmp/Show_A` deleted the history for `/tmp/ShowBA` in the test. Escape `%` and `_`, and handle case sensitivity consistently with the filesystem. Migration ignores insert/commit failures and renames the old database regardless; only mark it migrated after a successful checked transaction. Production callers also ignore `recordProcessedFile()` failures.

13. **P2 — Scanning can freeze or reenter the UI, and follows directory links without cycle detection. Inspection.** [mainwindow.cpp:561](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/mainwindow.cpp:561), [mainwindow.cpp:622](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/mainwindow.cpp:622).

    Probing each file blocks the GUI. Recursive traversal calls `processEvents()` without a scan-state guard, allowing workspace/scan actions to reenter the operation. Directory symlinks can loop or lead outside the intended workspace. Move scanning/probing to a cancellable worker, guard scan state, and explicitly decide whether to skip directory links or track canonical visited directories. Batch scan-cache writes in transactions.

14. **P2 — “Never grow” protection is incomplete and can undo requested work. Inspection.** [transcodeworker.cpp:422](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:422), [transcodeworker.cpp:527](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:527).

    Audio-only conversions and the “obsolete extension” branch bypass the size check. Container overhead can also enlarge fallback remuxes. The fallback discards the requested video transformation and records the source as processed, preventing an ordinary retry with different settings. Define separate outcomes for smaller output accepted, original retained, and required conversion completed despite growth. Apply the size policy to the total final file. Do not force video re-encoding merely because a container extension is old; decide from stream codecs, compatibility, and requested filters.

15. **P2 — Interlace/HDR handling needs an explicit media policy. Inspection.** [transcodeworker.cpp:163](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:163), [transcodeworker.cpp:302](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:302), [transcodeworker.cpp:462](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/transcodeworker.cpp:462).

    The first 360 frames decide interlacing for the entire file; a failed/ambiguous probe overrides interlaced metadata as “progressive.” Add confidence and optional samples from later positions. Separate deinterlacing, inverse telecine, and reducing progressive high frame rates in the UI. The present “de-bob” option halves any qualifying high frame rate, including genuinely progressive footage.

    The metadata model does not represent color range, primaries, transfer, HDR mastering information, or Dolby Vision. Ten-bit pixel format alone is not an HDR policy. Define preserve-HDR versus explicit SDR conversion, and validate the chosen FFmpeg build's behavior when resizing/filtering HDR. Do not assume all metadata is lost: recent FFmpeg versions can propagate some of it automatically, but this application does not verify the result.

**Encoding speed and final size**

The current default is `medium`, CRF 28, 10-bit 4:2:0, with `no-sao=1:selective-sao=0:aq-mode=1:pmode=1:pme=1`.

- **Fix the invalid profile option.** `profile=main10` inside `-x265-params` produced `Unknown option: profile`. Use FFmpeg's `-profile:v:0 main10`. The output still happened to be Main10 because the filter produces 10-bit 4:2:0; the current profile parameter is not enforcing it.
- **Benchmark simpler parameters.** Remove the redundant `selective-sao=0` when SAO is already disabled. Compare preset defaults against deliberate SAO/AQ changes. Avoid assuming detail-preservation settings minimize size. The current x265 documentation marks `pmode` and `pme` deprecated from 4.1; do not force them universally. [x265 options](https://x265.readthedocs.io/en/master/cli.html).
- **Try `fast`/`faster`, CRF 28/30/32, against `medium`.** Add the missing `faster` preset to the UI. A faster preset sacrifices compression efficiency; increasing CRF is a separate way to reduce size. Compare acceptable results by bytes and total runtime rather than assuming equal CRF guarantees identical quality across configurations. The README claim that slower presets necessarily produce larger files is misleading. [x265 preset documentation](https://x265.readthedocs.io/en/master/presets.html).
- **Retain 10-bit as the baseline.** Test an 8-bit speed option only if it materially helps the intended machines and banding remains acceptable. Avoid adopting `tune grain`, aggressive detail enhancement, long GOPs, extra B-frames, or expensive RDO settings as blanket defaults for this goal.
- **Avoid work that is immediately discarded.** When ordinary frame-rate deinterlacing is wanted, test `bwdif=mode=send_frame` instead of `send_field` followed by halving FPS. The current ordering can also scale twice as many frames as survive. Preserve anamorphic SAR when possible rather than always expanding pixels. Offer lower resolution targets, such as 720p, where acceptable.
- **Budget audio explicitly.** Let users retain selected languages, drop unwanted commentary, and choose per-channel bitrate targets or optional stereo downmix. AAC VBR quality settings do not give a predictable total audio budget. Copying an already-small compatible track is faster and avoids another generation of loss.
- **Treat concurrency as a throughput setting.** `pools=N` is not a process-wide CPU cap. Decoders, filters, and frame threads add work. Raising concurrency leaves existing workers at their old thread allocations. Measure aggregate throughput at one and two jobs, and allocate encoding/filtering resources together. [x265 threading documentation](https://x265.readthedocs.io/en/master/threading.html).
- **Add a short sample-encode action.** Show time, output bytes, and a before/after view before a library batch. Keep CRF as the fast default; offer two-pass target-size encoding only as an optional slower mode where size predictability warrants it.

A new preliminary benchmark used seconds 10–20 of the supplied 1440×1080 ProRes sample, four x265 pool threads, four decoder threads, two filter threads, no audio, and one serial run per variant. The first row uses the app's compression settings with the profile spelling corrected. Times include FFmpeg startup and decoding.

| Configuration | Seconds | Output MiB | Time versus current | Size versus current |
|---|---:|---:|---:|---:|
| Current parameters, medium CRF 28 | 10.61 | 0.753 | baseline | baseline |
| Preset defaults, medium CRF 28 | 9.28 | 0.782 | −12.5% | +3.8% |
| Preset defaults, fast CRF 28 | 7.83 | 0.804 | −26.2% | +6.7% |
| Preset defaults, fast CRF 30 | 8.44 | 0.648 | −20.5% | −14.0% |
| Light hqdn3d, default medium CRF 28 | 10.24 | 0.776 | −3.5% | +2.9% |

This supports trying fast/CRF 30, not declaring it universally optimal. Quality was not scored for these new encodes. Small timing differences need repeated measurements; the local FFmpeg build reports disabled FFmpeg optimizations. Denoising reduced bytes only slightly against its matching medium-default baseline and added runtime. Use multiple sources, especially noisy SD, clean HD, animation, motion, dark scenes, and HDR. [Exact benchmark commands/results](/tmp/hevc-audit-harness/benchmark/results.json).

Your existing 50.05-second comparison also contains useful evidence:

| Existing artifact | Size MiB | Logged encode seconds | Stored mean VMAF |
|---|---:|---:|---:|
| Grain-tuned baseline, CRF 23 | 17.40 | 25.36 | 95.85 |
| Variant 4, CRF 22 | 13.12 | 26.82 | 95.83 |
| Variant 4, CRF 23 | 11.34 | 25.71 | 95.33 |

Variant 4 at CRF 22 was 24.6% smaller with essentially the same stored VMAF, but that baseline uses `tune grain` and is not the current app. Several options changed together. These are previously saved measurements, not new reruns; they do not establish which individual option helped. Consolidate the experiment scripts into a parameterized runner that records complete commands, versions, runtime, bytes, failures, and properly aligned quality comparisons. Preserve a visual check for motion, grain, and smoothing artifacts.

**AviSynth+ as an advanced feature**

AviSynth+ is feasible here. The installed Linux FFprobe successfully opened a generated `.avs` and reported 10-bit 4:2:0 frames. FFmpeg supports AviSynth input when built accordingly; AviSynth+ itself supports Windows, Linux, and macOS, although individual plugin availability must be checked. [FFmpeg AviSynth support](https://ffmpeg.org/ffmpeg-all.html#AviSynth), [AviSynth+ platforms](https://github.com/AviSynth/AviSynthPlus#platform-availability).

I recommend one shared job pipeline with two filter backends:

1. **Built-in FFmpeg filters** for ordinary crop, resize, deblock, and light denoise. This avoids extra installation requirements for common cleanup. `hqdn3d` is one candidate designed to improve compressibility, but filtering still adds cost. Offer crop detection as a suggestion that users can preview and accept. [FFmpeg filter documentation](https://ffmpeg.org/ffmpeg-all.html#hqdn3d).
2. **Optional AviSynth+ backend** for advanced source/filter chains and user presets. Feed filtered video into the existing FFmpeg/libx265 encoder, taking audio, subtitles, attachments, chapters, and metadata from the original input. Keep script execution in the FFmpeg child process. This avoids embedding another runtime into the GUI and avoids a full intermediate encode.

For a duration-preserving CFR script and an original MKV with compatible auxiliary streams, the shape of the command would be:

```text
ffmpeg -i filtered.avs -i original.mkv
  -map 0:v:0 -map 1:a? -map 1:s? -map 1:t?
  -map_metadata 1 -map_chapters 1
  -c copy -c:v:0 libx265 -preset fast -crf 30
  -pix_fmt yuv420p10le -profile:v:0 main10 unique-job-output.mkv
```

The actual command builder must supply the per-stream codec decisions and unique output reservation described above. The script controls video filtering; the app controls job lifecycle and final-file commit.

The feature needs a typed filter/job description containing crop geometry, output dimensions, rational frame rate, duration/frame-count expectations, source stream, SAR, color policy, script content/version, and plugin requirements. Hash this configuration into the processing history. Use the same filter configuration for preview and encoding: today's live preview reads the unfiltered original, so it cannot validate cleanup.

Start with an opt-in per-file panel offering crop, a light denoise preset, deblock, a sample preview/encode, and an advanced script editor. Analyze actual script output before launching a full job. Do not silently combine a script's resize/deinterlace with the app's automatic versions. Keep source indexing files and temporary scripts in a job/cache directory, and handle paths containing quotes, Unicode, and separators correctly.

For the first version, require a preserved timeline. Trimming, speed changes, frame interpolation, inverse telecine, and VFR sources require explicit timing rules to keep original audio/subtitles synchronized. Do not assume a raw HEVC or Y4M stream carries all source timestamps and metadata. Determine whether each source plugin preserves timing or needs timecodes before accepting those cases.

Budget AviSynth prefetch threads and memory together with x265 concurrency. Its multithreading can multiply filter memory use; blindly assigning all cores to every filter and every encoder is counterproductive. [AviSynth+ multithreading](https://avisynthplus.readthedocs.io/en/latest/avisynthdoc/syntax/syntax_internal_functions_multithreading_new.html).

The existing [filter prototype](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/scripts/filter_template.avs:1) and [batch prototype](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/scripts/encode_all.sh:19) are experiments, not a ready integration:

- The full ProRes intermediate adds a lossy encoding stage, substantial disk use, and extra I/O. Reserve such staging for a specific conversion that needs it; ordinary cleanup should frameserve directly.
- `ConvertBits(10)` changes bit depth, not chroma subsampling. The ProRes input is 4:2:2, so the direct x265 path does not ensure Main10 4:2:0, unlike the FFmpeg fallback. Explicitly convert the desired pixel format and preserve color properties. [AviSynth format conversion](https://avisynthplus.readthedocs.io/en/latest/avisynthdoc/corefilters/convert.html).
- The FFmpeg fallback ignores `X265_EXTRA_ARGS`, so backends produce different settings.
- Template lookup is relative to the caller's working directory; substitution with `sed` does not safely handle all filenames. The template is tailored to one source/letterbox geometry.
- The batch script treats any existing final file as completed, including a partial output from an interrupted mux. Use a unique temporary final file and publish only after success.
- The raw-video mux path needs timing, metadata, chapters, and attachment handling. The test script's copied audio slice also needs alignment checks.

**Unused code, unnecessary complexity, packaging, and usability**

- `DatabaseManager(dbPath)` ignores its argument. The worker and scheduler carry an obsolete local database path that has no effect. Honor an injected path for testing or remove this plumbing. Connection ownership should be per instance or explicitly per thread; multiple managers on one thread currently share a name that either instance can close.
- `computeFastHash()` reads 4 MiB from every input but does not prevent repeat processing. Implement real identity checking or remove that I/O. Comments incorrectly describe 10 MiB and verification that does not occur.
- The scheduler gives each worker exactly one file, while each worker owns another queue loop. Simplify to one job per worker. Replace string-keyed `QVariantMap` settings and string job statuses with typed structs/enums before adding filter backends.
- Remove unused UI includes (`QTextEdit`, `QLineEdit`, `QSpinBox`, `QComboBox`, `QCheckBox`), unused `QDirIterator`/`QScrollBar` includes, duplicate comments/assignments, the unused `oldSize` callback parameter, and constructor initialization-order warnings. These are maintenance improvements, not meaningful encoding-speed gains.
- The icon is embedded in Qt resources but also copied beside the executable. `app_icon.jpg` has no reference in the reviewed build/UI/packaging paths. Keep platform icon resources that packaging actually uses.
- `findRowByFilepath()` scans the entire table for every update and progress replaces table items repeatedly. Maintain a path/job-to-row index or a table model. Throttle preview by elapsed wall time; the current three seconds of video time can generate frequent extra decoders for fast jobs.
- Progress uses human-readable stderr and recompiles regular expressions repeatedly. Use `-progress pipe:1 -nostats`, separate diagnostics, and structured job results. Remove unconditional debug-progress logging. `logMessage()` only calls `qDebug`, leaving most diagnostics unavailable in a normal Windows GUI launch. [FFmpeg progress interface](https://ffmpeg.org/ffmpeg.html#Advanced-options).
- Store user preferences with `QSettings`, retain failure details instead of replacing every result with “Finished,” and distinguish estimated compression savings from disk space actually freed while originals remain in `.Trash`. Preflight free space for concurrent temporary outputs.
- [package_portable.py:21](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/package_portable.py:21) copies most of the build tree instead of deploying a defined runtime set. It never runs `windeployqt`, and assumes the executable is at the build root rather than a multi-config `Release` directory. The installer shares that layout assumption. Add explicit deployment/staging and test on a clean Windows machine.
- [package_linux.py:72](/run/media/alan/5688F82988F8096F/Users/Alan/Desktop/hevc_win/package_linux.py:72) hard-codes a distro-specific qmake path. The AppImage stage can find an old artifact after a failed build, and packaging failures do not reliably reach the process exit status. Build DEB/RPM artifacts against supported target environments and derive dependencies instead of assuming one host binary is portable across distributions. Pin deployment-tool versions for reproducibility.
- Repository bloat is mostly artifacts: approximately 484 MiB in `dist`, 587 MiB in `test_data`, and 1.2 GiB in `.git` locally. Some old installers are tracked, and `.gitignore` has an empty “Generated distribution packages” section. Put release binaries in release storage, ignore generated encodes/logs, and keep small reproducible media fixtures. Existing Git history would require a separately planned cleanup; none was performed.
- Correct README claims about guaranteed preservation, guaranteed nongrowth, fingerprints, “identical” quality at CRF 23, preset behavior, and the obsolete sidebar instructions. Update the About text's GPU claim or provide an actual GPU backend.

**Validation and suggested implementation order**

A clean Release build succeeded with GCC 16.2.1 and Qt 6.11.2 using `-Wall -Wextra -Wpedantic`. Warnings were member initialization order and an unused parameter. Packaging Python parsed successfully; both prototype shell scripts passed `bash -n`. Runtime checks used FFmpeg 8.1.3/libx265 4.3+1. No application changes were made, no packages were installed, and original test media was only read.

Implement in this order: safe output commit and validation; database locking/identity; stream planning and rotation; cancellation/scan state; measured preset/audio improvements; shared filter model and FFmpeg cleanup; optional AviSynth+. Add regression coverage for the reproduced collisions, trash failure, abort/restart, replaced-file history, mixed tracks, mov_text, rotated video, SQL wildcard paths, and competing database writes. Include long remuxes, crash leftovers, silent clips, and filter timeline/metadata checks before enabling advanced filtering in unattended batches.

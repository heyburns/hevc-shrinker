#!/usr/bin/env bash
# Do not use set -e so that errors are caught and processing continues.
set -u
set -o pipefail

##########################################
# Configuration
##########################################

# Recognized video extensions (case-insensitive), including .m4v
VIDEO_EXT_REGEX=".*\.\(mp4\|mkv\|wmv\|avi\|mov\|flv\|mpeg\|mpg\|m4v\)$"

# x265 Options
# (The -tune option has been removed.)
X265_CRF=23               # --crf 23 (Increase for higher compression at the expense of quality)
X265_PROFILE="main10"     # --profile main10
X265_NO_SAO=1             # --no-sao => no-sao=1
X265_SEL_SAO=0            # --selective-sao=0

# SQLite DB for tracking processed files
DB_FILE="processed_files.db"

# Trash directory for original files being discarded.
TRASH_DIR="./.Trash"
if [ ! -d "$TRASH_DIR" ]; then
  mkdir "$TRASH_DIR"
fi

# Initialize the database if it doesn't exist.
if [ ! -f "$DB_FILE" ]; then
  sqlite3 "$DB_FILE" <<EOF
CREATE TABLE processed_files (
    filepath TEXT PRIMARY KEY,
    filehash TEXT,
    processed_at INTEGER
);
EOF
fi

##########################################
# Helper Functions
##########################################

# Check if the first video track is H.265 (HEVC) AND the first audio track is AAC.
is_already_x265_aac() {
  local file="$1"
  local video_codec audio_codec

  video_codec=$(
    ffprobe -hide_banner -loglevel error \
      -select_streams v:0 -show_entries stream=codec_name \
      -of csv=p=0 "$file" 2>/dev/null | head -n1 | tr '[:upper:]' '[:lower:]'
  )
  audio_codec=$(
    ffprobe -hide_banner -loglevel error \
      -select_streams a:0 -show_entries stream=codec_name \
      -of csv=p=0 "$file" 2>/dev/null | head -n1 | tr '[:upper:]' '[:lower:]'
  )
  if [[ "$video_codec" == "hevc" && "$audio_codec" == "aac" ]]; then
    return 0
  else
    return 1
  fi
}

# Return "copy" if video is H.265, otherwise "libx265".
decide_video_codec() {
  local file="$1"
  local vcodec
  vcodec=$(
    ffprobe -hide_banner -loglevel error \
      -select_streams v:0 -show_entries stream=codec_name \
      -of csv=p=0 "$file" 2>/dev/null | head -n1 | tr '[:upper:]' '[:lower:]'
  )
  if [[ "$vcodec" == "hevc" ]]; then
    echo "copy"
  else
    echo "libx265"
  fi
}

# Return "copy" if audio is AAC, otherwise "qaac".
decide_audio_codec() {
  local file="$1"
  local acodec
  acodec=$(
    ffprobe -hide_banner -loglevel error \
      -select_streams a:0 -show_entries stream=codec_name \
      -of csv=p=0 "$file" 2>/dev/null | head -n1 | tr '[:upper:]' '[:lower:]'
  )
  if [[ "$acodec" == "aac" ]]; then
    echo "copy"
  else
    echo "qaac"
  fi
}

# Build string for -x265-params.
x265_params() {
  echo "profile=${X265_PROFILE}:no-sao=${X265_NO_SAO}:selective-sao=${X265_SEL_SAO}"
}

# SQLite helper: Check if a file has been processed.
has_been_processed() {
  local filepath="$1"
  local esc_filepath="${filepath//\'/\'\'}"
  local count
  count=$(sqlite3 "$DB_FILE" "SELECT COUNT(*) FROM processed_files WHERE filepath = '$esc_filepath';")
  if [ "$count" -gt 0 ]; then
    return 0  # Processed.
  else
    return 1  # Not processed.
  fi
}

# SQLite helper: Mark a file as processed.
mark_as_processed() {
  local final_filepath="$1"
  local filehash="$2"
  local timestamp
  timestamp=$(date +%s)
  local esc_filepath="${final_filepath//\'/\'\'}"
  sqlite3 "$DB_FILE" "INSERT OR REPLACE INTO processed_files (filepath, filehash, processed_at) VALUES ('$esc_filepath', '$filehash', $timestamp);"
}

# Remux any file to an MKV container (lossless copy).
remux_to_mkv() {
  local infile="$1"
  local outfile="$2"
  ffmpeg -y -hide_banner -loglevel error -i "$infile" -c copy "$outfile"
  return $?
}

##########################################
# Main Script
##########################################

# Process each video file found recursively.
mapfile -d '' all_files < <(find . -type f -iregex "$VIDEO_EXT_REGEX" -print0)

for file in "${all_files[@]}"; do
  echo "============================================"
  echo "Found video file: $file"

  # Convert file path to an absolute path.
  abs_file=$(realpath "$file")
  if has_been_processed "$abs_file"; then
    echo "  [SKIP] Already processed. Skipping file."
    continue
  fi

  # Determine file directory, base name, and extension.
  file_dir=$(dirname "$abs_file")
  base_name=$(basename "$abs_file")
  base_noext="${base_name%.*}"
  ext_lower=$(echo "${abs_file##*.}" | tr '[:upper:]' '[:lower:]')

  # Gather video dimensions and average frame rate.
  orig_height=$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$abs_file")
  orig_width=$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$abs_file")
  fps_str=$(ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate \
            -of default=noprint_wrappers=1:nokey=1 "$abs_file")
  fps=$(awk -F'/' '{if($2!="0") printf "%.2f", $1/$2; else print 0}' <<< "$fps_str")
  bobbed=$(awk "BEGIN {print ($fps >= 50) ? 1 : 0}")

  # Determine if the file needs to be resized (vertical resolution > 1080).
  if [ "$orig_height" -gt 1080 ]; then
    scale_factor=$(awk "BEGIN {printf \"%.4f\", 1080 / $orig_height}")
    new_width=$(awk "BEGIN {w=int($orig_width * $scale_factor); if (w % 2 != 0) w--; print w}")
    resize_line="Lanczos4Resize(${new_width},1080)"
  else
    resize_line=""
  fi

  # Determine if frame rate is bobbed (fps >= 50).
  if [ "$bobbed" -eq 1 ]; then
    select_even_line="SelectEven()"
  else
    select_even_line=""
  fi

  # Decide whether to skip processing.
  # Skip only if the file is already H.265 + AAC AND its height is ≤1080 AND its frame rate is below 50 fps.
  if is_already_x265_aac "$abs_file"; then
    if [ "$orig_height" -le 1080 ] && [ "$bobbed" -eq 0 ]; then
      if [ "$(echo "${abs_file##*.}" | tr '[:upper:]' '[:lower:]')" != "mkv" ]; then
        echo "  File is already H.265 + AAC with acceptable resolution/frame rate but not in MKV. Remuxing..."
        final_out="${file_dir}/${base_noext}.mkv"
        remux_to_mkv "$abs_file" "$final_out"
        if [ $? -ne 0 ]; then
          echo "  [ERROR] Remuxing failed for $abs_file" >> error.log
          continue
        fi
        # Instead of deleting the original, move it to .Trash.
        mv "$abs_file" "$TRASH_DIR/"
      else
        echo "  File is already H.265 + AAC with acceptable resolution/frame rate in MKV. Skipping processing."
        final_out="$abs_file"
      fi
      NEW_HASH=$(sha1sum "$final_out" | awk '{print $1}')
      mark_as_processed "$final_out" "$NEW_HASH"
      rm -f "${abs_file}.lwi"
      echo "============================================"
      continue
    else
      echo "  File is already H.265 + AAC but does not meet resolution/frame rate requirements. Forcing re-encoding."
      want_video="libx265"
      want_audio="copy"  # Copy audio if already AAC.
    fi
  else
    want_video=$(decide_video_codec "$abs_file")
    want_audio=$(decide_audio_codec "$abs_file")
  fi

  echo "  Processing file: $base_name"

  # Flag if file is WMV, AVI, or FLV (new encoded output is always kept for these).
  if [[ "$ext_lower" == "wmv" || "$ext_lower" == "avi" || "$ext_lower" == "flv" ]]; then
    is_special=1
    echo "  Detected special file ($ext_lower); new encoded output will always be used."
  else
    is_special=0
  fi

  # Define the final output filename.
  final_out="${file_dir}/${base_noext}.mkv"

  ##############################
  # Re-encoding process:
  ##############################
  tmp_video="${file_dir}/${base_noext}.tmpvideo.mkv"
  tmp_audio="${file_dir}/${base_noext}.tmpaudio.m4a"

  # --- VIDEO PROCESSING ---
  if [[ "$want_video" == "copy" ]]; then
    echo "  Video already in H.265. Copying video track..."
    ffmpeg -y -hide_banner -loglevel error -i "$abs_file" -an -c:v copy "$tmp_video"
    if [ $? -ne 0 ]; then
      echo "  [ERROR] Failed to extract video track from $abs_file" >> error.log
      rm -f "$tmp_video"
      rm -f "${abs_file}.lwi"
      continue
    fi
  else
    tmp_avs="${file_dir}/${base_noext}.tmp.avs"
    # Convert absolute path to Windows-style path for Avisynth.
    win_file=$(cygpath -w "$abs_file")
    if [[ $(echo "$ext_lower" | tr '[:upper:]' '[:lower:]') == "wmv" || "$ext_lower" == "avi" || "$ext_lower" == "flv" ]]; then
      echo "  Re-encoding special file to H.265 using DirectShowSource..."
      {
        echo "video=DirectShowSource(\"$win_file\")"
        echo "ConvertBits(8)"
        echo "ConverttoYV12()"
        if [ -n "$resize_line" ]; then echo "$resize_line"; fi
        if [ -n "$select_even_line" ]; then echo "$select_even_line"; fi
        echo "LRemoveDust(17,4)"
        echo "Prefetch(12)"
      } > "$tmp_avs"
    else
      echo "  Re-encoding video to H.265 using Avisynth+ filter chain..."
      {
        echo "video=LWLibavVideoSource(\"$win_file\")"
        echo "audio=LWLibavAudioSource(\"$win_file\",stream_index=1)"
        echo "AudioDub(video,audio)"
        echo "ConvertBits(8)"
        echo "ConverttoYV12()"
        if [ -n "$resize_line" ]; then echo "$resize_line"; fi
        if [ -n "$select_even_line" ]; then echo "$select_even_line"; fi
        echo "LRemoveDust(17,4)"
        echo "Prefetch(12)"
      } > "$tmp_avs"
      orig_size=$(stat -c%s "$abs_file")
    fi

    ffmpeg -y -hide_banner -loglevel info -stats \
      -i "$tmp_avs" \
      -an \
      -c:v libx265 \
      -crf "$X265_CRF" \
      -x265-params "$(x265_params)" \
      "$tmp_video"
    ret=$?
    rm -f "$tmp_avs"
    if [ $ret -ne 0 ]; then
      if [[ $is_special -eq 1 ]]; then
        echo "  [ERROR] H.265 encoding via Avisynth+ failed for special file: $abs_file" >> error.log
      else
        echo "  [ERROR] H.265 encoding via Avisynth+ failed for file: $abs_file" >> error.log
      fi
      rm -f "$tmp_video"
      rm -f "${abs_file}.lwi"
      continue
    fi
  fi

  # --- AUDIO PROCESSING ---
  audio_extracted=""
  if [[ "$want_audio" == "copy" ]]; then
    echo "  Audio already AAC. Copying audio track..."
    ffmpeg -y -hide_banner -loglevel error -i "$abs_file" -vn -c:a copy "${file_dir}/${base_noext}.tmpaudio.aac"
    if [ $? -ne 0 ]; then
      echo "  [ERROR] Failed to extract AAC track from $abs_file" >> error.log
      rm -f "$tmp_video" "${file_dir}/${base_noext}.tmpaudio.aac"
      rm -f "${abs_file}.lwi"
      continue
    fi
    audio_extracted="${file_dir}/${base_noext}.tmpaudio.aac"
  else
    echo "  Re-encoding audio to AAC (QAAC VBR 100)..."
    wav_file="${file_dir}/${base_noext}.tmpaudio.wav"
    ffmpeg -y -hide_banner -loglevel error -i "$abs_file" -vn -acodec pcm_s16le "$wav_file"
    if [ $? -ne 0 ]; then
      echo "  [ERROR] Failed to extract audio as WAV from $abs_file" >> error.log
      rm -f "$tmp_video" "$wav_file"
      rm -f "${abs_file}.lwi"
      continue
    fi
    qaac --silent -V 100 "$wav_file" -o "$tmp_audio"
    if [ $? -ne 0 ]; then
      echo "  [ERROR] QAAC encoding failed for file: $abs_file" >> error.log
      rm -f "$tmp_video" "$wav_file" "$tmp_audio"
      rm -f "${abs_file}.lwi"
      continue
    fi
    rm -f "$wav_file"
    audio_extracted="$tmp_audio"
  fi

  ##############################
  # FINAL MUXING (to MKV) WITH COVER ART ATTACHMENT
  ##############################
  cover_file=""
  cover_ext=""
  # Check for cover art files named "poster.<ext>" or "<base_noext>-poster.<ext>".
  for ext in jpg png webp; do
    if [ -f "${file_dir}/poster.${ext}" ]; then
      cover_file="${file_dir}/poster.${ext}"
      cover_ext="${ext}"
      break
    elif [ -f "${file_dir}/${base_noext}-poster.${ext}" ]; then
      cover_file="${file_dir}/${base_noext}-poster.${ext}"
      cover_ext="${ext}"
      break
    fi
  done
  # If no cover art is found, check for an image with the same base name.
  if [ -z "$cover_file" ]; then
    for ext in jpg png webp; do
      if [ -f "${file_dir}/${base_noext}.${ext}" ]; then
        cover_file="${file_dir}/${base_noext}.${ext}"
        cover_ext="${ext}"
        break
      fi
    done
  fi

  cover_tmp=""
  if [ -n "$cover_file" ]; then
    cover_tmp="${file_dir}/cover.${cover_ext}"
    cp "$cover_file" "$cover_tmp"
    if [ "$cover_ext" == "jpg" ]; then
      cover_mimetype="image/jpeg"
    elif [ "$cover_ext" == "png" ]; then
      cover_mimetype="image/png"
    elif [ "$cover_ext" == "webp" ]; then
      cover_mimetype="image/webp"
    else
      cover_mimetype="application/octet-stream"
    fi
    echo "  Found cover image: $cover_file, temporary copy: $cover_tmp"
  fi

  out_temp="${file_dir}/${base_noext}.new.mkv"
  echo "  Muxing video and audio into MKV container..."
  ffmpeg -y -hide_banner -loglevel info -stats \
    -i "$tmp_video" \
    ${audio_extracted:+-i "$audio_extracted"} \
    ${cover_tmp:+-attach "$cover_tmp" -metadata:s:t mimetype=$cover_mimetype} \
    -c copy "$out_temp"
  if [ $? -ne 0 ]; then
    echo "  [ERROR] Final muxing failed for file: $abs_file" >> error.log
    rm -f "$tmp_video" "$audio_extracted" "$out_temp"
    [ -n "$cover_tmp" ] && rm -f "$cover_tmp"
    rm -f "${abs_file}.lwi"
    continue
  fi

  if [ -n "$cover_tmp" ] && [ -f "$cover_tmp" ]; then
    rm -f "$cover_tmp"
  fi

  # For special file types (wmv, avi, flv) always use the new encoded output.
  if [[ $is_wmv -eq 1 || "$ext_lower" == "avi" || "$ext_lower" == "flv" ]]; then
    final_out="${file_dir}/${base_noext}.mkv"
    mv "$out_temp" "$final_out"
  else
    orig_remux="${file_dir}/${base_noext}.orig.mkv"
    echo "  Remuxing original file into MKV for comparison..."
    remux_to_mkv "$abs_file" "$orig_remux"
    if [ $? -ne 0 ]; then
      echo "  [ERROR] Remuxing original file failed for $abs_file" >> error.log
      rm -f "$orig_remux"
      continue
    fi
    original_size=$(stat -c%s "$orig_remux")
    new_size=$(stat -c%s "$out_temp")
    if (( new_size < original_size )); then
      echo "  New file is smaller. Using new encoded file."
      mv "$out_temp" "$final_out"
      # Move the original file to TRASH.
      mv "$abs_file" "$TRASH_DIR/"
      rm -f "$orig_remux"
    else
      echo "  New file is larger. Keeping remuxed original."
      mv "$orig_remux" "$final_out"
      rm -f "$out_temp"
      # Move the original file to TRASH.
      mv "$abs_file" "$TRASH_DIR/"
    fi
  fi

  rm -f "$orig_remux" "$tmp_video" "$audio_extracted"
  NEW_HASH=$(sha1sum "$final_out" | awk '{print $1}')
  mark_as_processed "$final_out" "$NEW_HASH"
  echo "  File processed and recorded as: $final_out"

  if [ "$abs_file" != "$final_out" ]; then
    echo "  Original file moved to .Trash: $abs_file"
  fi

  rm -f "${abs_file}.lwi"
  echo "============================================"
done

echo "All files processed."

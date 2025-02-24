#!/usr/bin/env bash
# Do not use set -e to allow error handling to catch errors and continue.
set -u
set -o pipefail

##########################################
# Configuration
##########################################

# Recognized video extensions (case-insensitive)
VIDEO_EXT_REGEX=".*\.\(mp4\|mkv\|wmv\|avi\|mov\|flv\|mpeg\|mpg\)$"

# x265 Options
X265_TUNE="grain"         # --tune grain
X265_CRF=23               # --crf 23  (Increase this value for higher compression, at the cost of quality)
X265_PROFILE="main10"     # --profile main10
X265_NO_SAO=1             # --no-sao => no-sao=1
X265_SEL_SAO=0            # --selective-sao=0

# SQLite DB for tracking processed files
DB_FILE="processed_files.db"

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

# Checks if the first video track is x265 (HEVC) AND the first audio track is AAC.
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

# Return "copy" if video is x265, otherwise "libx265".
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
  # Escape any single quotes.
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

# Find all recognized video files (recursively).
mapfile -d '' all_files < <(find . -type f -iregex "$VIDEO_EXT_REGEX" -print0)

for file in "${all_files[@]}"; do
  echo "============================================"
  echo "Found video file: $file"

  if has_been_processed "$file"; then
    echo "  [SKIP] Already processed. Skipping file."
    continue
  fi

  # Determine directory, base filename, and extension.
  dir="$(dirname "$file")"
  base_name="$(basename "$file")"
  base_noext="${base_name%.*}"
  ext_lower=$(echo "${file##*.}" | tr '[:upper:]' '[:lower:]')

  # Flag if file is WMV.
  if [[ "$ext_lower" == "wmv" ]]; then
    is_wmv=1
    echo "  Detected WMV file; special processing will be applied."
  else
    is_wmv=0
  fi

  # Define the final output filename (always .mkv).
  final_out="${dir}/${base_noext}.mkv"

  ##############################
  # Process files already in x265+aac:
  ##############################
  if is_already_x265_aac "$file"; then
    if [ "$ext_lower" != "mkv" ]; then
      echo "  File is already x265 + AAC but not in MKV container. Remuxing to MKV..."
      remux_to_mkv "$file" "$final_out"
      if [ $? -ne 0 ]; then
        echo "  [ERROR] Remuxing failed for $file" >> error.log
        continue
      fi
      rm -f "$file"
    else
      echo "  File is already x265 + AAC in MKV container. Using existing file."
      final_out="$file"
    fi
    NEW_HASH=$(sha1sum "$final_out" | awk '{print $1}')
    mark_as_processed "$final_out" "$NEW_HASH"
    rm -f "${file}.lwi"
    echo "============================================"
    continue
  fi

  echo "  Processing file: $base_name"

  ##############################
  # Re-encoding process:
  ##############################
  want_video=$(decide_video_codec "$file")  # "libx265" or "copy"
  want_audio=$(decide_audio_codec "$file")    # "qaac" or "copy"

  tmp_video="${dir}/${base_noext}.tmpvideo.mkv"
  tmp_audio="${dir}/${base_noext}.tmpaudio.m4a"

  # --- VIDEO PROCESSING ---
  if [[ "$want_video" == "copy" ]]; then
    echo "  Video already in x265. Copying video track..."
    ffmpeg -y -hide_banner -loglevel error -i "$file" -an -c:v copy "$tmp_video"
    if [ $? -ne 0 ]; then
      echo "  [ERROR] Failed to extract video track from $file" >> error.log
      rm -f "$tmp_video"
      rm -f "${file}.lwi"
      continue
    fi
  else
    if [[ $is_wmv -eq 1 ]]; then
      echo "  Re-encoding WMV file to x265 with adjusted Avisynth+ audio stream (stream_index=0)..."
      tmp_avs="${dir}/${base_noext}.tmp.avs"
      cat <<EOF > "$tmp_avs"
video=LWLibavVideoSource("$file")
audio=LWLibavAudioSource("$file",stream_index=0)
AudioDub(video,audio)
ConvertBits(8)
ConverttoYV12()
LRemoveDust(17,4)
Prefetch(12)
EOF
    else
      echo "  Re-encoding video to x265 using Avisynth+ filter chain..."
      tmp_avs="${dir}/${base_noext}.tmp.avs"
      cat <<EOF > "$tmp_avs"
video=LWLibavVideoSource("$file")
audio=LWLibavAudioSource("$file",stream_index=1)
AudioDub(video,audio)
ConvertBits(8)
ConverttoYV12()
LRemoveDust(17,4)
Prefetch(12)
EOF
      # For non-WMV files, get the original file size for comparison.
      orig_size=$(stat -c%s "$file")
    fi

    ffmpeg -y -hide_banner -loglevel info -stats \
      -i "$tmp_avs" \
      -an \
      -c:v libx265 \
      -tune "$X265_TUNE" \
      -crf "$X265_CRF" \
      -x265-params "$(x265_params)" \
      "$tmp_video"
    ret=$?
    rm -f "$tmp_avs"
    if [ $ret -ne 0 ]; then
      if [[ $is_wmv -eq 1 ]]; then
        echo "  [ERROR] x265 encoding via Avisynth+ failed for WMV file: $file" >> error.log
      else
        echo "  [ERROR] x265 encoding via Avisynth+ failed for file: $file" >> error.log
      fi
      rm -f "$tmp_video"
      rm -f "${file}.lwi"
      continue
    fi
  fi

  # --- AUDIO PROCESSING ---
  audio_extracted=""
  if [[ "$want_audio" == "copy" ]]; then
    echo "  Audio already AAC. Copying audio track..."
    ffmpeg -y -hide_banner -loglevel error -i "$file" -vn -c:a copy "${dir}/${base_noext}.tmpaudio.aac"
    if [ $? -ne 0 ]; then
      echo "  [ERROR] Failed to extract AAC track from $file" >> error.log
      rm -f "$tmp_video" "${dir}/${base_noext}.tmpaudio.aac"
      rm -f "${file}.lwi"
      continue
    fi
    audio_extracted="${dir}/${base_noext}.tmpaudio.aac"
  else
    echo "  Re-encoding audio to AAC (QAAC VBR 100)..."
    wav_file="${dir}/${base_noext}.tmpaudio.wav"
    ffmpeg -y -hide_banner -loglevel error -i "$file" -vn -acodec pcm_s16le "$wav_file"
    if [ $? -ne 0 ]; then
      echo "  [ERROR] Failed to extract audio as WAV from $file" >> error.log
      rm -f "$tmp_video" "$wav_file"
      rm -f "${file}.lwi"
      continue
    fi
    qaac --silent -V 100 "$wav_file" -o "$tmp_audio"
    if [ $? -ne 0 ]; then
      echo "  [ERROR] QAAC encoding failed for file: $file" >> error.log
      rm -f "$tmp_video" "$wav_file" "$tmp_audio"
      rm -f "${file}.lwi"
      continue
    fi
    rm -f "$wav_file"
    audio_extracted="$tmp_audio"
  fi

  ##############################
  # FINAL MUXING (to MKV) WITH COVER ATTACHMENT
  ##############################
  cover_file=""
  cover_ext=""
  # First, search for a cover file named "poster" with common image extensions.
  for ext in jpg png webp; do
    if [ -f "${dir}/poster.${ext}" ]; then
      cover_file="${dir}/poster.${ext}"
      cover_ext="${ext}"
      break
    fi
  done
  # If not found, search for an image with the same base name as the video.
  if [ -z "$cover_file" ]; then
    for ext in jpg png webp; do
      if [ -f "${dir}/${base_noext}.${ext}" ]; then
        cover_file="${dir}/${base_noext}.${ext}"
        cover_ext="${ext}"
        break
      fi
    done
  fi

  cover_tmp=""
  if [ -n "$cover_file" ]; then
    cover_tmp="${dir}/cover.${cover_ext}"
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

  out_temp="${dir}/${base_noext}.new.mkv"
  echo "  Muxing video and audio into MKV container..."
  ffmpeg -y -hide_banner -loglevel info -stats \
    -i "$tmp_video" \
    ${audio_extracted:+-i "$audio_extracted"} \
    ${cover_tmp:+-attach "$cover_tmp" -metadata:s:t mimetype=$cover_mimetype} \
    -c copy "$out_temp"
  if [ $? -ne 0 ]; then
    echo "  [ERROR] Final muxing failed for file: $file" >> error.log
    rm -f "$tmp_video" "$audio_extracted" "$out_temp"
    [ -n "$cover_tmp" ] && rm -f "$cover_tmp"
    rm -f "${file}.lwi"
    continue
  fi

  if [ -n "$cover_tmp" ] && [ -f "$cover_tmp" ]; then
    rm -f "$cover_tmp"
  fi

  if [[ $is_wmv -eq 1 ]]; then
    # For WMV files, skip size comparison and always use the new encoded file.
    final_out="${dir}/${base_noext}.mkv"
    mv "$out_temp" "$final_out"
  else
    # Remux the original file into an MKV container for size comparison.
    orig_remux="${dir}/${base_noext}.orig.mkv"
    echo "  Remuxing original file into MKV for comparison..."
    remux_to_mkv "$file" "$orig_remux"
    if [ $? -ne 0 ]; then
      echo "  [ERROR] Remuxing original file failed for $file" >> error.log
      rm -f "$orig_remux"
      continue
    fi

    original_size=$(stat -c%s "$orig_remux")
    new_size=$(stat -c%s "$out_temp")
    if (( new_size < original_size )); then
      echo "  New file is smaller. Using new encoded file."
      mv "$out_temp" "$final_out"
      rm -f "$orig_remux"
    else
      echo "  New file is larger. Keeping remuxed original."
      mv "$orig_remux" "$final_out"
      rm -f "$out_temp"
    fi
  fi

  # Cleanup temporary files.
  rm -f "$orig_remux" "$tmp_video" "$audio_extracted"
  NEW_HASH=$(sha1sum "$final_out" | awk '{print $1}')
  mark_as_processed "$final_out" "$NEW_HASH"
  echo "  File processed and recorded as: $final_out"

  if [ "$file" != "$final_out" ]; then
    echo "  Removing original file: $file"
    rm -f "$file"
  fi

  rm -f "${file}.lwi"
  echo "============================================"
done

echo "All files processed."

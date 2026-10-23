#!/bin/zsh
set -eu
set -o pipefail
bridge_dir="${0:A:h}"
if [[ $# -lt 1 ]]; then
  print -u2 'Usage: ./transcribe.sh audio-file [transcribe.py options]'
  exit 2
fi
audio_file="$1"
shift
ffmpeg -hide_banner -loglevel error -i "$audio_file" -ar 48000 -ac 2 -f s16le - |
  "$bridge_dir/.venv/bin/python" "$bridge_dir/transcribe.py" "$@"

#!/bin/zsh
# Run manually from the Mac desktop with an explicitly public raw PCM sample.
# No physical microphone is recorded. The caller supplies the input and its hash.
set -eu
set -o pipefail
cd "${0:A:h}"
bridge_dir="${TERM50_DOUBAO_BRIDGE_DIR:-}"
if [[ -z "$bridge_dir" || $# -ne 2 ]]; then
    print -u2 '用法：TERM50_DOUBAO_BRIDGE_DIR=/path/to/bridge zsh verify-public-audio.command /path/to/public-48k-stereo-s16le.pcm <sha256>'
    exit 2
fi
audio_path="$1"
expected_sha256="$2"
"$bridge_dir/.venv/bin/python" - "$audio_path" "$expected_sha256" <<'PY'
import hashlib, pathlib, re, sys
p = pathlib.Path(sys.argv[1])
expected = sys.argv[2].lower()
assert re.fullmatch(r'[0-9a-f]{64}', expected), '请提供公开测试 PCM 的 SHA-256'
assert p.is_file() and p.stat().st_size > 0 and p.stat().st_size % 4 == 0, '需要非空的原始 48 kHz 双声道 16-bit PCM'
assert hashlib.sha256(p.read_bytes()).hexdigest() == expected, '测试 PCM 与指定摘要不符，停止验证'
print('正在调用豆包；请暂时不要切换接收窗口。结束后自动恢复输入法与麦克风。')
PY
umask 077
mkdir -p private
"$bridge_dir/.venv/bin/python" transcribe.py --bridge-dir "$bridge_dir" --pcm "$audio_path" | tee private/public-audio-result.jsonl

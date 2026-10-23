#!/bin/zsh
set -eu
companion_dir="${0:A:h}"
bridge_dir="${TERM50_DOUBAO_BRIDGE_DIR:-}"
if [[ -z "$bridge_dir" ]]; then
    print -u2 '请先设置 TERM50_DOUBAO_BRIDGE_DIR 为原豆包桥接目录。'
    exit 2
fi
bridge_python="$bridge_dir/.venv/bin/python"
if [[ "${1:-}" == --check ]]; then
    shift
    # A missing virtual environment must still produce an actionable report.
    if [[ ! -x "$bridge_python" ]]; then
        if ! bridge_python="$(command -v python3)"; then
            print -u2 '未找到 Python。请恢复豆包桥接 Python 环境，或安装 Python 3.13 及更新版本。'
            exit 1
        fi
    fi
    exec "$bridge_python" "$companion_dir/doctor.py" --bridge-dir "$bridge_dir" "$@"
fi
if [[ ! -x "$bridge_python" ]]; then
    print -u2 '未找到豆包桥接 Python 环境。请检查 TERM50_DOUBAO_BRIDGE_DIR，或运行 companion/run.sh --check。'
    exit 1
fi
exec "$bridge_python" "$companion_dir/server.py" --bridge-dir "$bridge_dir" "$@"

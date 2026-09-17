#!/bin/bash
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export IDF_PATH="$PROJECT_ROOT/.tools/esp-idf"
export IDF_TOOLS_PATH="$PROJECT_ROOT/.tools/idf-tools"
export IDF_PYTHON_ENV_PATH="$IDF_TOOLS_PATH/python_env/idf5.4_py3.11_env"
export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
eval "$(python "$IDF_PATH/tools/idf_tools.py" export --format=key-value)"
exec python "$IDF_PATH/tools/idf.py" -C "$PROJECT_ROOT/firmware" "$@"

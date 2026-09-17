#!/bin/bash
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"
PYTHON_BIN="${PYTHON_BIN:-python3.11}"
command -v "$PYTHON_BIN" >/dev/null
mkdir -p .tools
if [[ ! -d .tools/esp-idf ]]; then
    git clone --branch v5.4.3 --depth 1 --shallow-submodules --recursive \
        https://github.com/espressif/esp-idf.git .tools/esp-idf
fi
export IDF_TOOLS_PATH="$PROJECT_ROOT/.tools/idf-tools"
"$PYTHON_BIN" .tools/esp-idf/tools/idf_tools.py install --targets esp32s3
"$PYTHON_BIN" .tools/esp-idf/tools/idf_tools.py install-python-env
"$IDF_TOOLS_PATH/python_env/idf5.4_py3.11_env/bin/python" -m pip install cmake==3.30.5 ninja==1.11.1.4

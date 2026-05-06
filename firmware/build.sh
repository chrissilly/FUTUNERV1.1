#!/bin/bash
# Build the FUTUNER v2 firmware.
#
# Uses ESP-IDF v5.5 from $HOME/esp/esp-idf and the IDF Python venv from
# $HOME/.espressif. If you've installed IDF elsewhere, override:
#   IDF_PATH=/path/to/esp-idf ./build.sh

set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

export IDF_PATH=${IDF_PATH:-$HOME/esp/esp-idf}
PYTHON_PATH=${IDF_PYTHON:-$HOME/.espressif/python_env/idf5.5_py3.9_env/bin/python}

[ -d "$IDF_PATH" ] || { echo "IDF_PATH=$IDF_PATH not found"; exit 1; }
[ -x "$PYTHON_PATH" ] || PYTHON_PATH=python3

echo "IDF: $IDF_PATH"
echo "Python: $PYTHON_PATH"

"$PYTHON_PATH" "$IDF_PATH/tools/idf.py" build

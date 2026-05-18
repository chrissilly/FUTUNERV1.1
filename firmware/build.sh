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

# Bundle the split UI sources (ui/{html,css,js}) into the single
# firmware/futuner_control_panel.html that the dongle's flash partition
# serves. Deterministic — same inputs, same output bytes. See
# tools/bundle_ui.py and the Prompt-9 UI eval for the contract.
PROJECT_ROOT="$(cd "$DIR/.." && pwd)"
"$PYTHON_PATH" "$PROJECT_ROOT/tools/bundle_ui.py" \
    --in  "$PROJECT_ROOT/ui/control_panel.html" \
          "$PROJECT_ROOT/ui/control_panel.css" \
          "$PROJECT_ROOT/ui/control_panel.js" \
    --out "$DIR/futuner_control_panel.html"

"$PYTHON_PATH" "$IDF_PATH/tools/idf.py" build

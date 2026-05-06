# dtc — DTC read/clear feature for FUTUNER (UDS 0x19 0x02 + 0x14).
#
# IMPORTANT: this CMakeLists.txt is intentionally inert.
#
# firmware/src/ is a single ESP-IDF component (registered by
# firmware/src/CMakeLists.txt). Subdirectories under src/ — including this
# one — are NOT auto-registered as separate components. Source files in
# this directory are picked up by the parent CMakeLists.txt, which lists
# dtc/dtc_feature.c and dtc/dtc_uds.c explicitly under app_sources and
# adds dtc to INCLUDE_DIRS.
#
# This file exists for the same reason firmware/src/feature_manager/
# CMakeLists.txt exists:
#   (a) keep the module visibly grouped on disk, and
#   (b) leave a placeholder for the day the codebase is refactored from
#       "one big component at src/" to "one ESP-IDF component per module"
#       (per the project rule: that would be a sweeping refactor of every
#       module at once, not piecemeal).
#
# If you ever flip the project to per-module components, replace this
# block with an idf_component_register() call along the lines of:
#
#   idf_component_register(
#       SRCS "dtc_feature.c" "dtc_uds.c"
#       INCLUDE_DIRS "."
#       REQUIRES log freertos)

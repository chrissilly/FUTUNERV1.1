# feature_manager — central state arbiter for FUTUNER user-visible features.
#
# IMPORTANT: this CMakeLists.txt is intentionally inert.
#
# firmware/src/ is a single ESP-IDF component (registered by
# firmware/src/CMakeLists.txt). Subdirectories under src/ — including this
# one — are NOT auto-registered as separate components. Source files in
# this directory are picked up by the parent CMakeLists.txt, which lists
# feature_manager/feature_manager.c explicitly under app_sources and adds
# feature_manager to INCLUDE_DIRS.
#
# This file exists to:
#   (a) keep the module visibly grouped on disk, and
#   (b) leave a placeholder for the day the codebase is refactored from
#       "one big component at src/" to "one ESP-IDF component per module"
#       (per the project rule: that would be a sweeping refactor of every
#       module at once, not piecemeal — see review on 2026-05-05).
#
# If you ever flip the project to per-module components, replace this
# block with an idf_component_register() call along the lines of:
#
#   idf_component_register(
#       SRCS "feature_manager.c"
#       INCLUDE_DIRS "."
#       REQUIRES log freertos)

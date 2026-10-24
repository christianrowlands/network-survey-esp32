# NS Scout firmware. Build one board with:
#   idf.py -B build/<board> -DSDKCONFIG=build/<board>/sdkconfig \
#          -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/<board>.defaults" build
# or build every board with scripts/build-all-targets.sh.
cmake_minimum_required(VERSION 3.22)

set(PROJECT_VER "0.1.0")

# Build only what main needs (it lists every project component).
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# One committed lock file per chip, so builds for several targets do not
# rewrite each other's (component versions are pinned exactly in the manifests).
idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${IDF_TARGET})
project(network_survey_board)

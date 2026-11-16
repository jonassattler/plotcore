# SPDX-FileCopyrightText: 2026 Jonas Sattler
# SPDX-License-Identifier: BSD-3-Clause

# plotcore asserted with no file, no thread of its own and no window.
#
# Catch2 is whatever the including project found -- the viewer's comes from
# vcpkg -- and fetched at the version the viewer pins when plotcore is built
# on its own, so the two builds run the same framework.
find_package(Catch2 3 CONFIG QUIET)
if(NOT Catch2_FOUND)
  include(FetchContent)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.15.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
include(Catch)

# --- what the plot decides before a pixel is touched ----------------------
# Arithmetic only: projectLine takes doubles and fills two vectors, so this
# needs no window, no engine and no file. The four things the plot got wrong
# for as long as it drew through Qt Graphs are all decided here, and a grab of
# something rendered is a poor way to assert any of them.
add_executable(test_plotprojection test_plotprojection.cpp)
target_link_libraries(test_plotprojection PRIVATE plotcore::core Catch2::Catch2WithMain)
catch_discover_tests(test_plotprojection)

# --- which runs to hold, and which to read next ---------------------------
# The fold a line is reduced by, the policy over the runs it is reduced into,
# and the pyramid that holds it at every resolution at once. Large enough in
# places that the build spreads over the pool, which is the path worth
# asserting rather than only the one small enough to stay on this thread.
add_executable(test_plotlevels test_plotlevels.cpp)
target_link_libraries(test_plotlevels PRIVATE plotcore::core Catch2::Catch2WithMain)
catch_discover_tests(test_plotlevels)

# --- the runs a line is held at, read through a host -----------------------
# RunLadder, with the test as the host: it lends the pyramids, answers each
# read it is told to out of the elements, and counts what is given up.
add_executable(test_plotruns test_plotruns.cpp)
target_link_libraries(test_plotruns PRIVATE plotcore::core Catch2::Catch2WithMain)
catch_discover_tests(test_plotruns)

# --- the whole plot, as a host drives it ----------------------------------
# The store, the frame and the gestures, through the same calls a Qt, Python
# or Rust host makes and checked in the pixels and the elements they hand
# back, because a host sees nothing else.
add_executable(test_plotfigure test_plotfigure.cpp)
target_link_libraries(test_plotfigure PRIVATE plotcore::core Catch2::Catch2WithMain)
catch_discover_tests(test_plotfigure)

# --- the axes, as the viewer held them -------------------------------------
# Which ticks over which window, how many figures a label carries, when a power
# is cut up and what a base is: the rules the viewer's suite asked of its QML
# frame, asked of the arithmetic that is the frame now.
add_executable(test_plotaxes test_plotaxes.cpp)
target_link_libraries(test_plotaxes PRIVATE plotcore::core Catch2::Catch2WithMain)
catch_discover_tests(test_plotaxes)

# The viewer's answers, asked again: every question its parity suite asked of
# the QML and of these ports while there were two of them (viewer/README.md).
# Json.hpp is private to the library, and this is the one test that reads it.
add_executable(test_plotviewer test_plotviewer.cpp)
target_link_libraries(test_plotviewer PRIVATE plotcore::core Catch2::Catch2WithMain)
target_include_directories(test_plotviewer PRIVATE ${PROJECT_SOURCE_DIR}/cpp/src)
target_compile_definitions(test_plotviewer PRIVATE
  PLOTCORE_VIEWER_ANSWERS="${CMAKE_CURRENT_SOURCE_DIR}/viewer/answers.jsonl")
catch_discover_tests(test_plotviewer)

# --- points by an x and a y each ---------------------------------------------
# The point tree a scatter is drawn through, held to what its header promises
# by brute force over clouds where each promise matters.
add_executable(test_plotscatter test_plotscatter.cpp)
target_link_libraries(test_plotscatter PRIVATE plotcore::core Catch2::Catch2WithMain)
catch_discover_tests(test_plotscatter)

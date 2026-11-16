# SPDX-FileCopyrightText: 2026 Jonas Sattler
# SPDX-License-Identifier: BSD-3-Clause

# What each way of drawing a line costs, and how still it holds. Built with the
# tests so that it cannot rot, and run by them only in its smoke form, whose
# assertions are counts: a benchmark's durations measure the machine.
add_executable(plotcore-bench bench_drawing.cpp)
target_link_libraries(plotcore-bench PRIVATE plotcore::core)
add_test(NAME plotcore_bench_smoke COMMAND plotcore-bench --smoke)

# The same through the Qt widget, because under a software painter the painting
# is most of a frame, and a benchmark of the scene alone would not show it.
if(PLOTCORE_WITH_QT)
    add_executable(plotcore-bench-qt bench_drawing.cpp)
    target_compile_definitions(plotcore-bench-qt PRIVATE PLOTCORE_BENCH_QT)
    target_link_libraries(plotcore-bench-qt PRIVATE plotcore::core plotcore::qt)
endif()

# What a scatter costs to build and to draw, over clouds that tell the walk's
# cases apart; its smoke form asserts what a frame visits and draws.
add_executable(plotcore-bench-scatter bench_scatter.cpp)
target_link_libraries(plotcore-bench-scatter PRIVATE plotcore::core)
add_test(NAME plotcore_bench_scatter_smoke COMMAND plotcore-bench-scatter --smoke)
if(PLOTCORE_WITH_QT)
    add_executable(plotcore-bench-scatter-qt bench_scatter.cpp)
    target_compile_definitions(plotcore-bench-scatter-qt PRIVATE PLOTCORE_BENCH_QT)
    target_link_libraries(plotcore-bench-scatter-qt PRIVATE plotcore::core plotcore::qt)
endif()

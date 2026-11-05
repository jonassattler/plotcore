# SPDX-FileCopyrightText: 2026 Jonas Sattler
# SPDX-License-Identifier: BSD-3-Clause

# plotcore in C++: plotcore::core, the arithmetic and the Figure over it, and
# plotcore::qt, a QWidget over a Figure, with the demo that walks every
# feature on one. Which of them are built is the root's to say.

add_library(plotcore_core
  src/Budget.cpp
  src/Camera.cpp
  src/Figure.cpp
  src/Format.cpp
  src/Json.cpp
  src/Levels.cpp
  src/Palettes.cpp
  src/Parallel.cpp
  src/Plot.cpp
  src/Projection.cpp
  src/Pyramid.cpp
  src/RunLadder.cpp
  src/Ticks.cpp
)
add_library(plotcore::core ALIAS plotcore_core)
set_target_properties(plotcore_core PROPERTIES
  EXPORT_NAME core
  OUTPUT_NAME plotcore
  # A static archive that a shared library -- the C interface, a Python
  # module, a Rust crate -- can be linked into.
  POSITION_INDEPENDENT_CODE ON
)
target_compile_features(plotcore_core PUBLIC cxx_std_20)
target_include_directories(plotcore_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
# The pool's threads. PUBLIC because a static archive's dependencies are its
# user's to link.
target_link_libraries(plotcore_core PUBLIC Threads::Threads)

# --- the Qt adapter -------------------------------------------------------
# plotcore::Figure in a QWidget, and the demo that walks every feature on
# one. CI builds it against a stock Qt on Linux and Windows, runs the demo's
# self-test, and packs the demo for both (packaging/).
if(PLOTCORE_WITH_QT)
  # GL resolved the way a prebuilt Qt's own libraries resolve it, to
  # libGL.so.1. Qt's CMake package links every Widgets program against
  # OpenGL::GL, which CMake's FindOpenGL otherwise resolves through glvnd to
  # libOpenGL.so.0 and libGLX.so.0 -- which the demo never calls, which no
  # Qt library asks for, and which a stock RHEL 8 does not have
  # (libglvnd-opengl): the AppImage stopped there before main(). --as-needed
  # does not drop them, because they resolve Qt's own references to libGL.
  if(NOT DEFINED OpenGL_GL_PREFERENCE)
    set(OpenGL_GL_PREFERENCE LEGACY)
  endif()
  find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
  add_library(plotcore_qt STATIC
    qt/src/PlotWidget.cpp
    qt/include/plotcore/qt/PlotWidget.hpp
  )
  add_library(plotcore::qt ALIAS plotcore_qt)
  set_target_properties(plotcore_qt PROPERTIES EXPORT_NAME qt AUTOMOC ON)
  target_include_directories(plotcore_qt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/qt/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )
  target_link_libraries(plotcore_qt PUBLIC plotcore::core Qt6::Widgets)
  add_subdirectory(qt-demo)
endif()

if(PLOTCORE_BUILD_TESTS)
  add_subdirectory(tests)
  add_subdirectory(bench)
endif()

<!--
SPDX-FileCopyrightText: 2026 Jonas Sattler
SPDX-License-Identifier: BSD-3-Clause
-->

# plotcore

The plot [H5Scope](https://github.com/jonassattler/H5Scope) draws, as a library
for C++, C, Python and Rust.

- **No spike is ever thinned away.** A line is held as a pyramid of min/max
  envelopes, and every extreme is drawn at the sample it is, so a one-sample
  spike in a billion survives at any zoom.
- **A zoom reads nothing.** Every picture is a fold of what is already held:
  the cost of a frame follows the width of the pane, not the length of the
  line.
- **Scatters of millions, beside the lines.** Points given by an x and a y each
  share the pane, the axes and every setting a line has. They are held in a tree
  that keeps each region's topmost point, so a frame visits about as many nodes
  as the pane has pixels and no outlier is lost. Each point may be coloured by
  a value through a colour bar or by a colour of its own, and selected by a
  click or a band. A dense scatter reaches the host as one image of the pane.
- **The axes, gestures and colours are H5Scope's own,** ported function for
  function, and H5Scope's parity tests hold the two to each other. That
  includes logarithmic axes in any base, separate and stacked y axes, a flipped
  view, and the five colour cycles.
- **One plot in four languages.** A golden script of settings and gestures, and
  the digest of every scene it asks for, which the C, Python and Rust tests all
  have to reproduce exactly (`golden`).

## What is in here

One directory per language, and in each the library, its tests, and a demo
that walks every feature on one window.

| Path | What it is |
|---|---|
| `cpp` | The C++ library, `plotcore::core`: the arithmetic (`Projection`, `Levels`, `Pyramid`, `Scatter`, `Budget`), the frame and gestures (`Format`, `Ticks`, `Camera`, `Palettes`), the store (`Plot`), and `Figure`, the whole plot a host drives. Beside it `qt`, a `QWidget` over a `Figure` (`plotcore::qt`), and `qt-demo` |
| `c` | `Figure` as C, built as one shared library, `plotcore::c`, which the Python and Rust libraries are built on |
| `python` | The Python package: ctypes over the C interface, with a PyQt6 widget and a matplotlib host. Beside it `pyqt6-demo` |
| `rust` | The Rust workspace: `plotcore-sys`, the safe `plotcore` crate, `plotcore-egui`, and `egui-demo` |
| `golden` | The script the C, Python and Rust tests all run, and the digest they must all write |
| `packaging` | The demos as AppImages for Linux (`appimage`) and as folders for Windows (`windows`) |

## Building

**C++ and C.** CMake 3.26 or newer and a C++20 compiler. Nothing else.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build
cmake --install build --prefix /where/it/goes   # find_package(plotcore)
```

Add `-DPLOTCORE_WITH_QT=ON` for the Qt widget and its demo (Qt 6.5 or newer),
which is then `build/cpp/qt-demo/plotcore-qt-demo`.

**Python.** One wheel per platform serves every Python 3, because the package
links no Python:

```sh
pip install ./python              # or ./python[qt,matplotlib]
pip install -r python/pyqt6-demo/requirements.txt
python python/pyqt6-demo/demo.py
```

To develop against a C library you built yourself, set `PLOTCORE_LIBRARY` to
its path.

**Rust.** The toolchain is pinned in `rust/rust-toolchain.toml`.

```sh
cd rust
cargo test --workspace
cargo run --release -p egui-demo
```

Every demo takes `--self-test`, which walks every control and gesture itself
and says whether each did what it should. CI runs all three that way.

## Downloads

Each release is on the
[releases page](https://github.com/jonassattler/plotcore/releases), with
every demo for Linux and Windows and the Python wheels. Nothing is published
to PyPI or crates.io.

- **Python:** `pip install` the release's wheel for your platform, Linux
  x86-64 (glibc 2.28 and newer) or Windows x64. One wheel serves every
  Python 3.
- **Rust:** a git dependency on the release's tag, which builds the C++ itself
  and so needs a C++20 compiler:
  `cargo add plotcore plotcore-egui --git https://github.com/jonassattler/plotcore --tag v0.2.1`
- **C++ and C:** build the release's source with CMake, as above.

None of the demos needs anything installed. On Linux it is an AppImage: the
download loses its executable bit, so `chmod +x` it first. The AppImages are
built on AlmaLinux 8, so like H5Scope they start on RHEL 8 (glibc 2.28) and
anything newer. On Windows it is a zipped folder with
`plotcore-<toolkit>-demo.exe` in it.

CI builds the same on every commit and uploads it to the run as artefacts:

| | Linux | Windows |
|---|---|---|
| The SDK: headers, libraries, CMake package | `plotcore-sdk-Linux` | `plotcore-sdk-Windows` |
| The Python wheel | `plotcore-python-Linux` | `plotcore-python-Windows` |
| The Qt demo | `plotcore-qt-demo-Linux` | `plotcore-qt-demo-Windows` |
| The egui demo | `plotcore-egui-demo-Linux` | `plotcore-egui-demo-Windows` |
| The PyQt6 demo | `plotcore-pyqt6-demo-Linux` | `plotcore-pyqt6-demo-Windows` |
| The source of the AppImages' system libraries | `plotcore-appimage-sources` | |

A tag `v<version>` is a release: `.github/workflows/release.yml` runs CI at
that commit, then publishes what it built on the releases page. The version
is written in `CMakeLists.txt`, `python/pyproject.toml` and `rust/Cargo.toml`,
and the tag has to name it.

## Licence

BSD-3-Clause. See [LICENSE](LICENSE). Releases up to 0.2.1 were
GPL-3.0-only.

Each demo carries, beside plotcore's licence, a `licenses` folder with the
licences of everything else in it: the Rust crates the egui demo is built
from, the Python and the distributions the PyQt6 demo runs on, the
third-party code Qt builds in, and the libraries an AppImage takes from
AlmaLinux. `packaging/notices.py` writes it, and CI fails a package it does
not cover. The Qt and PyQt6 demos carry Qt, which they convey under the
LGPL-3.0, and the PyQt6 demo carries PyQt6, which is GPL-3.0 alone, so that
demo's packages are distributed under the GPL-3.0 as a whole. A release
publishes the source of the AppImages' AlmaLinux libraries beside them.

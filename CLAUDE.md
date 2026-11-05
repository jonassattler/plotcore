# plotcore — working notes for Claude

The plot H5Scope draws, as a library: C++20 with no dependencies, a C
interface over it, and Python and Rust libraries over that. One directory per
language, each holding its library, its tests and a demo that walks every
feature. BSD-3-Clause; up to 0.2.1 it was GPL-3.0-only, and both copyright
holders agreed to the change.

"The viewer" everywhere in this repository is
[H5Scope](https://github.com/jonassattler/H5Scope), which pins this repository
as a git submodule at `plotcore/` and is where the library came from. The
history before 2026-10-18 is H5Scope's, filtered down to these files, and a
`jonassattler/H5Scope#N` in an old commit message is that repository's pull
request.

The code is heavily commented on purpose — most files open with an argument
for why they are the way they are, usually naming the failure that motivated
them. Read those headers before changing the code under them; they are the
design record. `Projection.hpp`, `Levels.hpp` and `Pyramid.hpp` above all.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # no Qt, no Python, no Rust
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

- `-DPLOTCORE_WITH_QT=ON -DCMAKE_PREFIX_PATH=<qt>` adds the widget and the Qt
  demo, whose `--self-test` is the `plotcore_qt_demo` ctest test; run it
  with `QT_QPA_PLATFORM=offscreen`.
- **Python:** `pip install ./python` builds the wheel (scikit-build-core, from
  the CMake project one directory up), then `pytest python/tests` from
  *outside* the source tree, so that what is imported is what was installed.
  Against a C library built by hand: `PLOTCORE_LIBRARY=build/libplotcore_c.so`.
- **Rust:** in `rust/`, `cargo fmt --all --check`,
  `cargo clippy --workspace --all-targets --locked -- -D warnings`,
  `cargo test --workspace --locked`. CI runs exactly those three.
- **Demo packages:** `packaging/appimage/{qt,egui,pyqt6}-demo.sh` build one
  AppImage each and `verify.sh` checks one. They run anywhere, but only hold
  the RHEL 8 floor when run inside manylinux_2_28, which is what
  `in-container.sh` is for. Off a floor-sized machine, `verify.sh` fails on the
  floor and nothing else; the system libraries' notices need `rpm` or `dpkg`
  to say whose a library is. `packaging/windows/{qt,egui,pyqt6}-demo.ps1` build
  one Windows folder each and `verify.ps1` checks one; they need Windows.

CI is `.github/workflows/ci.yml`: C++ and C, the Qt widget, Python and Rust,
each built and tested on Linux and Windows; then every demo packaged for both,
one artefact per demo per platform. It builds, tests and uploads artefacts;
it publishes nothing.

## Releasing

A tag `v<version>` is a release, and `.github/workflows/release.yml` is all of
it: it runs `ci.yml` at the tagged commit, then publishes what that built as a
GitHub release — the demos (the Windows folders zipped), both wheels and the
source RPMs of the AppImages' system libraries, with a `SHA256SUMS`. **Nothing goes to PyPI or crates.io**, and nothing should
without the user asking for it: a version uploaded there cannot be taken
back.

- **The version** is in `CMakeLists.txt` (`project(plotcore VERSION …)`),
  `python/pyproject.toml`, and `rust/Cargo.toml` twice — `[workspace.package]`
  and the version beside each path in `[workspace.dependencies]`. The first
  job fails the release when the tag is not the first three; cargo fails the
  build when the fourth is not the first. It is also `__version__` in
  `python/src/plotcore/__init__.py`, which nothing checks; `rust/Cargo.lock`,
  which `cargo update -w` rewrites and `--locked` fails on until it has; the
  version the Python and Rust tests assert the library reports
  (`test_api.py`, `plotcore/src/lib.rs`); the `find_package(plotcore 0.x)`
  of the two install checks in `ci.yml`, which an installed package refuses
  across a minor (`SameMinorVersion`, because a minor before 1.0 is a break);
  and the tag in the `cargo add` line of `README.md` and `rust/README.md`.
- **The notes** are the tag's annotation, from a file so that git's cleanup
  keeps the Markdown headings:
  `git tag -a --cleanup=whitespace -F notes.md v<version>`. A lightweight tag
  has none.
- **Python** is the release's wheels. There is no sdist: the wheel's CMake
  project is the whole repository, so one of `python/` could not build it.
  The wheel carries the licence in its `dist-info`, installed by the `SKBUILD`
  block of `CMakeLists.txt`.
- **Rust** is a git dependency on the tag
  (`cargo add plotcore --git … --tag v<version>`), which works because
  `plotcore-sys` reaches the C++ through `cpp/` and `c/` beside its manifest:
  symbolic links into the repository. `build.rs` reads through them, and from
  the repository directly where a checkout has the links as text files (Git
  for Windows' default). cargo follows them when it packages the crate, and
  `include` keeps the package to the core and the C interface, so the crates
  are publishable as they stand; CI's rust job runs `cargo package` on every
  commit to keep them so.

## Layout

One directory per language, each with its library, its tests and its demo.
The root `CMakeLists.txt` says what is built and installed; `cpp/` and `c/`
each define their own targets.

| Path | What it is |
|---|---|
| `cpp/include/plotcore/`, `cpp/src/` | `plotcore::core`. The arithmetic: `Projection` (where a sample lands, gaps, the thinning, strokes as triangles), `Levels` (the fold and its seams, and the policy over held runs), `RunLadder` (the runs a host that reads on a thread holds under that policy, and the read it is to make next), `Pyramid` (what every run is folded out of), `Budget` (what may be held), `Parallel` (the pool a large fold is spread over). The frame: `Format`, `Ticks`, `Camera`, `Palettes`. Then `LineSource` (what a figure asks of its lines), `Plot` (lines held, and one such source) and `Figure` (the whole plot a host drives). `cpp/src/Json.*` is private. |
| `cpp/tests/` | The Catch2 suites: `test_plotprojection`, `test_plotlevels`, `test_plotruns`, `test_plotfigure`, `test_plotaxes` (the rules the viewer's suite held its QML axes to), and `test_plotviewer`, which asks again every question H5Scope's parity suite asked and holds the answers to the ones recorded in `viewer/answers.jsonl`. |
| `cpp/qt/`, `cpp/qt-demo/` | `plotcore::qt`, a `QWidget` over a `Figure`, and the Qt demo. |
| `c/` | `plotcore.h`, `Figure` as C, built as `plotcore_c`, and `tests/test_capi.c`. |
| `python/` | The package (`src/plotcore`: `_native.py`, `figure.py`, `scene.py`, `qt.py`, `mpl.py`), its tests, and `pyqt6-demo/` with the demo's pinned `requirements.txt`. |
| `rust/` | `plotcore-sys`, `plotcore`, `plotcore-egui`, `egui-demo`, and `README.md`, which each crate also names as its own. |
| `golden/` | The script the C, Python and Rust tests all run, and its digest. |
| `packaging/` | The icon; `appimage/`, the demos for Linux; `windows/`, the demos for Windows; `notices.py` and `licenses/`, the licences of what each demo carries beside plotcore. |

## plotcore and H5Scope

H5Scope builds this with `add_subdirectory(plotcore)` and links
`plotcore::core`; its own build also runs this repository's C++ tests, on its
RHEL 8 and MSVC toolchains. It uses the public headers and nothing else. Three
consequences:

- **A change here reaches the viewer when the viewer moves its pin.** That is
  an H5Scope pull request of its own (`git -C plotcore checkout <sha>`, commit
  the pointer), and it is where H5Scope's suite — `test_cost`,
  `test_customplot`, and the QML suite that drives the plot as a reader does —
  runs against the change. A change that
  needs the viewer to change with it lands here first, then in H5Scope with
  the pin.
- **The viewer's plot is `Figure`.** H5Scope hosts it in `gui::FigureItem`,
  a `LineSource` over the lines it reads from a file on a thread, and keeps no
  plot of its own: **a change to how the plot is numbered, ruled, zoomed,
  coloured or drawn is a change to the viewer's picture**, and its QML suite
  asserts that picture when the pin moves. `Format`, `Ticks`, `Camera` and
  `Palettes` began as ports of its `PlotFrame.qml`, `PlotSurface.qml` and
  `Theme.qml`, held to them by a parity suite for as long as both existed.
  The QML is gone; every answer that suite was given is recorded here
  (`cpp/tests/viewer/`, `test_plotviewer`), the rules the viewer's suite held
  its axes to are `test_plotaxes`, and the argument for each rule is written
  over its function in `Camera.cpp`, `Ticks.cpp` and `Palettes.cpp`, in the
  viewer's own words.
- **The rules of the plot itself are written in H5Scope's CLAUDE.md** — the
  separate axes, the logarithmic axis and its edges, why a log x is folded per
  column, why an extreme is drawn at its element, what a zoom may read — and
  comments here point there. They are the viewer's behaviour, and this library
  is how the viewer has it.

## plotcore as a library

Four layers above the arithmetic, each the only place its question is
answered:

- **`Format`, `Ticks`, `Camera`, `Palettes`** — the ports above.
- **`Plot`** holds lines: copied in (the pyramid *is* the copy) or streamed
  through `PyramidBuilder`, at the base the budget affords, with an optional
  `LineReader` below it. Every picture is a fold of what is held; with no
  reader, a window finer than the base is drawn *at* the base rather than from
  the whole-line summary. It is one **`LineSource`**: what a `Figure` asks of
  its lines, and all it asks. A host that holds its lines its own way — the
  viewer, reading a file on a thread under a shared budget — hands the figure
  a source of its own (`Figure::setSource`), and holds what it has read below
  its pyramids in a `RunLadder`, which `Plot` has no need of because its reader
  answers in the call. A line then also says which
  *series* it is, so a line keeps its colour when the host leaves one before it
  out. A figure asks each line what it spans once a frame, in `refresh()`:
  the viewer's Plot tab is ten thousand lines, and a question asked per line
  inside a loop over the lines is a hundred million.
- **`Figure`** is the whole plot a host drives: lines, settings, the camera,
  the gestures, and a `Scene` out in pane pixels with y down, flipping already
  applied: the lines, and the **marks** to paint under and over them in order —
  segments, boxes, circles and text, every colour, width and radius decided.
  A host measures text, forwards events and translates each mark into its
  toolkit's call; it decides nothing. The hosts used to draw the crosshair, the
  legend and the band themselves, and no two drew the same ones. A mark's
  *layer* (grid, frame, overlay) is for a host that draws some of it its own
  way: matplotlib draws its own frame and paints the overlay. The picture is
  **the viewer's, exactly** — its frame, its crosshair, PlotOverlayLegend's
  legend and PlotBand's readout, `FigureStyle`'s defaults being Theme's — and
  what a host may decline is a setting rather than a fork: `readout` (the
  reading in the pane; the viewer prints it in its footer), `bandReadout`,
  `paneMessage` (the viewer says it its own way) and `lineGestures`. Three things are this layer's own, all from
  jonassattler/H5Scope#30 and all about a line: **moved and scaled**
  (`LineSettings::scale`/`shift`, Alt-drag, Alt+Control+wheel, Control-click to
  put back), **stacked** in bands, and **a time per line**. A moved line stays
  in the common axis's extent and is numbered on it — an axis appearing beside
  the pane on the first pixel of a drag widened the gutter and moved every x
  under the pointer — and only a line on an axis of its own, or stacked, is
  numbered beside the pane. A scene is asked for with a `SceneRequest`: the
  forms of a line a host draws (`SceneOptions`), and whether it is a
  **picture** — the pane laid out again at another size, in the page's colours,
  with a crosshair pinned to a sample — which asks the lines for nothing and
  leaves the pane as it was. Beside the marks, a scene says what each axis
  was numbered and ruled at in its own values (`SceneAxis`): for a host that
  draws its own frame, and for a test that asks which values rather than which
  rounded pixels. Every frame shares `kMaxVertices` between its lines
  (`maxColumnsFor`). `state()`/`restore()` write a view as JSON
  (`cpp/src/Json.*`), passing over what they cannot read as the viewer's saved
  views do.
- **`c/plotcore.h`** is `Figure` as C, built as one shared library
  (`plotcore_c`, exporting nothing else, its C++ runtime linked in). Settings
  are **named, not laid out** — `plotcore_set_bool(f, "xLog", 1)`, the names a
  saved view writes — so a new setting is a new name and not a new ABI.
  Nothing is borrowed across it, every failure is a status with
  `plotcore_last_error()` beside it, and `test_capi.c` is compiled as C.

**Bars** (`FigureSettings::bars`, off until a host turns it on) draw a line as
one rectangle per device column, from the column's smallest value to its
largest, in `DrawnLine::bars` and in place of its stroke. A stroke's zigzag
leaves slits in a dense band that the data fills; a bar does not, and the seam
is what makes two neighbours always touch with nothing joining them. Columns
are a `ColumnGrid` fixed in x (`Levels.hpp` argues it), so a bar moves a whole
device pixel at a time, a pan folds only what it uncovers (`Plot::drawColumns`)
and a y zoom folds nothing. A line **falls back to its stroke** wherever a bar
would be untrue or unaffordable: a column inside the line with no element
(under a sample a pixel, the sparse end of a log x, a pause in a time base), a
column finer than the base, a time base that doubles back, a line's share of
`kMaxVertices` under a column a pixel, a host that did not ask for `FORM_BARS`,
and a source that does not answer `LineSource::drawColumns`, which is every
source but `Plot` today, the viewer's included. So a line has runs or bars and
a host paints both. It is on or off for now; choosing between the two along a
line, by how tall its bars are, is the likely next step, and one line holding
both forms is already what a host paints.

`golden/` is what holds the three languages to one plot: a script of
settings and gestures, and the digest of every scene it asks for. `test_capi`
writes it (`--write`) and checks it; the Python and Rust tests check the same
file through their own bindings. **A changed line in `expected.txt` is a
changed picture**, so read the diff before committing one.

`cpp/qt/` is a `QWidget` over a `Figure`, and `cpp/qt-demo` walks every
feature on one window; `--self-test` walks them itself offscreen.

`python/` is ctypes over the C interface (`_native.py` declares the header line
for line and checks the ABI version before anything is called), so it links no
Python and one `py3-none` wheel per platform serves every version.
scikit-build-core builds it from the CMake project one directory up
(`cmake.source-dir = ".."`, the `SKBUILD` block in `CMakeLists.txt`), which is
also why the wheel is built from the repository and not from an sdist. Two
rules from the boundary: **a scene is copied out whole** into numpy, so nothing
Python holds points into the library; and **a reader's callback is kept by the
token the C side holds**, released when the C side says it has let go of it,
which is the only moment index bookkeeping in Python could not get wrong.
`plotcore.qt` is the PyQt6 widget, `plotcore.mpl` draws on matplotlib's own
axes from each run's data-unit values (`plotcore_scene_line_run_values`) with
the figure's gestures under them, and `python/pyqt6-demo/demo.py --self-test`
is the Qt demo's walk, run by `python/tests/test_qt.py`.

`rust/` is the workspace: `plotcore-sys` compiles `cpp/src/` and `c/` with the
`cc` crate and declares the header by hand (no libclang), and its test holds
every structure's size to what the C compiler that built the library makes of
it, because working them out by hand got two of them wrong; `plotcore` is the
safe wrapper, whose readers and text measure are boxed and handed to the C side
as contexts it gives back; `plotcore-egui` paints a scene as plotcore's
triangles or as feathered polylines and hands egui's raw input events over as
gestures; `egui-demo` walks every feature, and its `--self-test` drives egui
headless, so `cargo test` needs no display. `plotcore-sys` reaches `cpp/src/`
and `c/` through links beside its manifest, which is how a packaged crate
carries them (see "Releasing").

## The floor

What this repository ships for Linux starts on RHEL 8 — glibc 2.28 — as
H5Scope does, and it is held in three places:

- **`plotcore_c`** links its C++ runtime in (`-static-libstdc++`), because a
  wheel's library is loaded by whatever Python is on the machine. The wheel is
  built in manylinux_2_28 and repaired to that tag by auditwheel.
- **The AppImages** are built in manylinux_2_28 against libraries from
  AlmaLinux 8 and EPEL 8, on Qt 6.8 (6.11's binaries need glibc 2.34) and on
  PyQt6 6.9 (6.10's wheels are manylinux_2_34). They link the C++ runtime
  dynamically, as a prebuilt Qt does, and gcc-toolset is what makes that safe:
  it builds against the host's libstdc++ plus a static archive of what is
  newer. `check-floor.sh` reads every ELF file in each image back against
  glibc 2.28 and GCC 8's libstdc++, and `try-on-rhel8.sh` runs each on a stock
  AlmaLinux 8 under X11.
- **What an image leaves to the host is one list**, `HOST_LIBRARIES` in
  `packaging/appimage/common.sh` — glibc, the GL and driver stack, core X11 and
  the Wayland client, fontconfig — and linuxdeploy's own list is overruled by
  it. libxcb-cursor is the library both Qt images exist to carry: Qt 6.5 and
  newer link the xcb plugin against it, and RHEL 8 has it only in EPEL.

## The demos on Windows

Each is a folder, `plotcore-<toolkit>-demo`, that starts on a Windows with
nothing installed — the AppImage's claim, made by different means:

- **Qt:** `windeployqt` beside the executable, plus what it leaves out — the
  offscreen platform plugin, without which `--self-test` stops on a message
  box that nothing on a runner dismisses, and the Visual C++ runtime.
- **egui:** `+crt-static`, which the `cc` crate reads too and so compiles
  plotcore with `/MT`: one executable that needs nothing beside it. Release
  builds are a Windows-subsystem program, so no console opens beside it.
- **PyQt6:** PyInstaller, around the wheel CI tested and the same pins as the
  AppImage (`python/pyqt6-demo/requirements.txt`). `--collect-binaries
  plotcore` is what brings the C library, which no import names.

The runner has the Visual C++ redistributable installed, so a bundle that is
missing it still starts there. `verify.ps1` therefore reads every import in
the folder with `dumpbin` and fails on a runtime DLL the folder does not
carry, before it runs the self-test with Qt, Python and the toolchains off
the `PATH`.

## The demos' licences

Every package carries plotcore's `LICENSE` and a `licenses/` folder with the
licences of everything else in it, which `packaging/notices.py` writes, one
section per kind of thing: `rust` (the crates `cargo tree` says the egui demo
links, and the standard library), `python` (every distribution beside the
interpreter, run under that interpreter), `qt` (the third-party code Qt builds
in, from the SBOMs Qt installs in `sbom/`, cut to the shipped binaries) and
`system` (an AppImage's AlmaLinux libraries, each under the RPM that installed
it). `notices.py check` is how `verify.sh` and `verify.ps1` read a package
back: a Qt binary or a system library its folder does not name fails it.

- **PyQt6's Qt is trimmed** to what the demo loads — QtCore, QtGui,
  QtWidgets and QtSvg, the platform and image-format plugins of qtbase, qtsvg
  and qtwayland — on both platforms (`trim_pyqt6` in `pyqt6-demo.sh`, the
  end of `pyqt6-demo.ps1`). PyQt6-Qt6 is all of Qt, FFmpeg and pdfium
  included, with only the LGPL text; trimmed, its notices are three Qt
  repositories' and no FFmpeg source has to be published. It carries no SBOM,
  so its Qt section is rendered from Qt's own for the same version and kept
  in `packaging/licenses/pyqt6-qt6/<version>-<platform>/`. **Bumping
  PyQt6-Qt6** means rendering it again: install that Qt with aqt (`--archives
  qtbase qtsvg qtwayland`, and `qtbase qtsvg` for Windows) and run
  `notices.py qt --lean` over its `sbom/` with the binaries the package
  keeps; the build fails on a version it has no rendering for.
- **What the sections cannot be written without** is kept under
  `packaging/licenses/` and listed, with where each came from, at the top of
  `notices.py`: SPDX texts for the licences Qt's SBOMs name, ICU's licence,
  and the licence files of projects whose crates ship none. A new crate or
  Qt licence without one fails the build and says what to add.
- **The source** of every system library is published with the release:
  `in-container.sh` downloads the source RPM of each exact build the images
  carry into `plotcore-<version>-appimage-sources.tar`, which the system
  sections name. Qt's and PyQt6's sections point at their upstream source for
  the version carried.

## Conventions

- SPDX header on every file: `SPDX-FileCopyrightText: 2026 Jonas Sattler` (and
  `2026 Andreas Martin` beside it on files derived from his work in
  jonassattler/H5Scope#30) and `SPDX-License-Identifier: BSD-3-Clause`.
- `.clang-format` (LLVM base, 100 columns, 4 spaces) and `.clang-tidy` — match
  them. `cargo fmt` and `rustfmt.toml` for Rust.
- Doc comments are `///` on the declaration; `//` blocks above a decision
  explain *why*, and name the failure that prompted it when there was one.
  Never strip an existing comment to "clean up" — those paragraphs are
  load-bearing.
- Test names are sentences: `TEST_CASE("a spike on a bucket's last sample is
  drawn at that sample")`, `def test_the_golden_script_draws_the_c_digest()`.
- Assert behaviour in counts, not durations: a duration measures the machine, a
  count measures the program.
- Commit messages: a lower-case-ish imperative sentence as the subject, then a
  body that argues the change — what was observed, what was measured, what was
  decided against. Wrapped at ~72.

## Gotchas

- **Never ask a figure for a scene inside a closure that holds egui's
  context** — the text measure calls back into `ctx.fonts_mut`, which takes the
  same lock.
- The Rust toolchain is pinned in `rust/rust-toolchain.toml` (egui 0.37's
  minimum), so that CI and a reader's machine run the same clippy.
- A Qt target here says `AUTOMOC ON` itself. H5Scope turns it on for every
  target it builds, which is how the Qt example linked there and nowhere else
  until this repository built it alone.
- Headless Qt needs `QT_QPA_PLATFORM=offscreen`; every AppImage runs with
  `APPIMAGE_EXTRACT_AND_RUN=1` wherever there is no FUSE, CI included.

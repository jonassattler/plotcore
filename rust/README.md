<!--
SPDX-FileCopyrightText: 2026 Jonas Sattler
SPDX-License-Identifier: BSD-3-Clause
-->

# plotcore for Rust

This is the plot [H5Scope](https://github.com/jonassattler/H5Scope) draws, as a
Rust library.

- **No spike is ever thinned away.** A line is held as a pyramid of min/max
  envelopes, so a one-sample spike survives at any zoom, and a zoom folds what
  is already held instead of reading again.
- **The same axes, gestures and colours.** They are the viewer's own, and its
  parity tests hold the two to each other.
- **Scatters of millions, beside the lines.** Points given by an x and a y each
  share the pane and every setting a line has, are coloured by a value through
  a colour bar or by a colour of their own, are selected by a click or a band,
  and reach the host as one image of the pane when they are dense.
- **The same plot from Rust, C++ and Python.** All three libraries are one
  plot, and a golden script of settings and gestures is what they all have to
  draw identically.

```rust
let mut figure = plotcore::Figure::new();
let sine: Vec<f64> = (0..1_000_000).map(|i| (i as f64 / 1000.0).sin()).collect();
figure.add_line(&sine, "sine").unwrap();
figure.set_size(800.0, 500.0, 1.0).unwrap();
figure.wheel(400.0, 250.0, 360.0, plotcore::Modifiers::NONE).unwrap(); // three notches in
let scene = figure.scene().unwrap(); // the lines and what to paint around them
```

## The crates

| Crate | What it is |
|---|---|
| `plotcore` | The figure: lines in, settings by name, gestures forwarded, a scene out. |
| `plotcore-egui` | A figure in egui 0.37: its scene painted, egui's input handed over as gestures. |
| `plotcore-sys` | The C interface the other two are built on. |

The crates are not on crates.io. Use them from the
[repository](https://github.com/jonassattler/plotcore), at a release's tag:

```sh
cargo add plotcore plotcore-egui --git https://github.com/jonassattler/plotcore --tag v0.2.1
```

`plotcore-sys` compiles plotcore's C++ itself, with the `cc` crate, so building
it needs a C++20 compiler and nothing else: no CMake, no libclang. The egui
demo in the repository uses every feature on one window:

```sh
cd rust
cargo run --release -p egui-demo
```

## Licence

BSD-3-Clause.

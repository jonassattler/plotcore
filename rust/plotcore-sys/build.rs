// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-License-Identifier: BSD-3-Clause

//! Compiles plotcore -- the core and its C interface -- into a static library
//! linked into whatever depends on this crate. No CMake and no libclang: the
//! sources are listed here and the bindings are checked in (src/lib.rs).

use std::path::PathBuf;

const SOURCES: &[&str] = &[
    "cpp/src/Budget.cpp",
    "cpp/src/Camera.cpp",
    "cpp/src/Figure.cpp",
    "cpp/src/Format.cpp",
    "cpp/src/Json.cpp",
    "cpp/src/Levels.cpp",
    "cpp/src/Palettes.cpp",
    "cpp/src/Parallel.cpp",
    "cpp/src/Plot.cpp",
    "cpp/src/Projection.cpp",
    "cpp/src/Pyramid.cpp",
    "cpp/src/Scatter.cpp",
    "cpp/src/Ticks.cpp",
    "c/plotcore.cpp",
];

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("cargo sets it"));
    // The sources are this crate's cpp/ and c/: links to the repository's own,
    // which cargo follows when it packages the crate, so a packaged crate
    // carries copies and needs no repository around it. A checkout made without
    // symbolic links -- Git for Windows' default -- has each link as a text
    // file naming its target, and is reached from rust/plotcore-sys instead.
    let root = if manifest.join("c/plotcore.h").is_file() {
        manifest.clone()
    } else {
        manifest.join("../..")
    };
    let version = std::env::var("CARGO_PKG_VERSION").expect("cargo sets it");

    let mut build = cc::Build::new();
    build
        .cpp(true)
        .std("c++20")
        .include(root.join("cpp/include"))
        .include(root.join("c"))
        .define("PLOTCORE_VERSION_STRING", Some(format!("\"{version}\"").as_str()))
        .warnings(false);
    if build.get_compiler().is_like_msvc() {
        build.flag("/utf-8");
    }
    for source in SOURCES {
        let path = root.join(source);
        println!("cargo:rerun-if-changed={}", path.display());
        build.file(path);
    }
    // Not plotcore: the C sizes of its structures, which the test holds the
    // checked-in bindings to.
    let sizes = manifest.join("csrc/sizes.cpp");
    println!("cargo:rerun-if-changed={}", sizes.display());
    build.file(sizes);
    for header in ["cpp/include/plotcore", "c"] {
        println!("cargo:rerun-if-changed={}", root.join(header).display());
    }
    build.compile("plotcore");

    // The pool's threads.
    let target = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target == "linux" {
        println!("cargo:rustc-link-lib=pthread");
    }
}

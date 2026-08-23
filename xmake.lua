-- Sage Package Manager build configuration
-- Build system: xmake (https://xmake.io)
-- Standards: Modern C++23 with full C++23 Modules (.cppm)
-- Linking: Fully dynamically linked to system shared libraries

set_project("sage")
set_version("0.2.0")
set_license("BSD-2-Clause")
set_description("Sage: High-performance, modular, multi-layer universal Linux package manager")

set_languages("c++23")
set_warnings("all", "extra")

-- Enable optimization for release mode
if is_mode("release") then
    set_optimize("fastest")
    set_strip("all")
elseif is_mode("debug") then
    set_symbols("debug")
    set_optimize("none")
end

add_rules("mode.release", "mode.debug")

-- Sage CLI executable target
target("sage")
    set_kind("binary")
    add_files("src/**.cppm")
    add_files("src/main.cpp")
    add_links("lmdb", "zstd", "curl")
    set_default(true)

-- Integration suite as a separate binary, built on demand (`xmake build sage-tests`)
target("sage-tests")
    set_kind("binary")
    set_default(false)
    add_files("src/**.cppm")
    add_files("tests/**.cppm")
    add_files("tests/main.cpp")
    add_links("lmdb", "zstd", "curl")

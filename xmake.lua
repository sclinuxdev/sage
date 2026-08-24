-- Sage Package Manager build configuration
-- Build system: xmake (https://xmake.io)
-- Standards: Modern C++23 with full C++23 Modules (.cppm)
-- Linking: Fully dynamically linked to system shared libraries

-- 版本号在这里写一遍，set_version 与注入给 C++ 的 SAGE_VERSION 都取它。
-- 手写第二处的后果已经出现过：0.1.3 -> 0.2.0 那次 xmake.lua 升了，
-- CLI banner 没跟上，于是 `sage --version` 与包元数据各说各的。
local SAGE_VERSION = "0.2.0"

set_project("sage")
set_version(SAGE_VERSION)
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
    add_defines("SAGE_VERSION=\"" .. SAGE_VERSION .. "\"")
    add_files("src/**.cppm")
    add_files("src/main.cpp")
    add_links("lmdb", "zstd", "curl")
    add_cxxflags("-msha", "-msse4.1", "-mssse3", "-maes", "-mpclmul")
    set_default(true)

-- Integration suite as a separate binary, built on demand (`xmake build sage-tests`)
target("sage-tests")
    set_kind("binary")
    set_default(false)
    add_defines("SAGE_VERSION=\"" .. SAGE_VERSION .. "\"")
    add_files("src/**.cppm")
    add_files("tests/**.cppm")
    add_files("tests/main.cpp")
    add_links("lmdb", "zstd", "curl")
    add_cxxflags("-msha", "-msse4.1", "-mssse3", "-maes", "-mpclmul")

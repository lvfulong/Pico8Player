#!/bin/bash

# 同步子模块 URL（把 .gitmodules 的变更刷新到 .git/config）
git submodule sync --recursive

# 更新子模块
git submodule update --init --recursive --force

# out-of-source build（单独给编辑器一个构建目录）
mkdir -p pc_pico_editor_build
cmake -S . -B pc_pico_editor_build -DBACKEND=PC
cmake --build pc_pico_editor_build --target pico_editor --config Release -j 8


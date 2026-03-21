#!/bin/bash

# 自动添加 GitHub 到 known_hosts，避免交互式提示
#mkdir -p ~/.ssh
#ssh-keyscan -t rsa,ecdsa,ed25519 github.com >> ~/.ssh/known_hosts 2>/dev/null

# 同步子模块 URL（把 .gitmodules 的变更刷新到 .git/config）
git submodule sync --recursive

# 更新子模块
git submodule update --init --recursive --force

# out-of-source build
mkdir -p pc_pico_build
cmake -S . -B pc_pico_build -DBACKEND=PC
cmake --build pc_pico_build --config Release -j 8
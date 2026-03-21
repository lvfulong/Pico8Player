cmake_minimum_required(VERSION 3.13)

add_executable(p8_export
	p8_export.cpp
	stb_image_write.h
)

target_compile_features(p8_export PRIVATE cxx_std_17)

if(MSVC)
	# 允许源码/字符串按 UTF-8 编译（避免中文路径/输出时乱码引发编译问题）
	target_compile_options(p8_export PRIVATE /utf-8)
endif()


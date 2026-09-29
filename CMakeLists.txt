cmake_minimum_required(VERSION 4.3)

# ---- Options ----

option(COPY_BUILD "Copy the build output to the Fallout 4 directory." OFF)

# ---- Cache build vars ----

macro(set_from_environment VARIABLE)
	if (NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
		set(${VARIABLE} $ENV{${VARIABLE}})
	endif ()
endmacro()

set_from_environment(Fallout4Path)
set_from_environment(VCPKG_ROOT)
set_from_environment(CommonLibF4Path)
set_from_environment(TOMLPLUSPLUS_PATH)

if (DEFINED VCPKG_ROOT)
	set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
	set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
else ()
	message(
		WARNING
		"Variable VCPKG_ROOT is not set. Continuing without vcpkg."
	)
endif ()

set(Boost_USE_STATIC_RUNTIME OFF CACHE BOOL "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")

# ---- Project ----

project(
	PerkConditionsFramework
	VERSION 1.0.0
	LANGUAGES CXX
)

set(PROJECT_VERSION_TWEAK 0)

set(COMMONLIB_PLUGIN_NAME "PerkConditionsFramework")
set(COMMONLIB_PLUGIN_AUTHOR "SilentlyGayming")
set(COMMONLIB_PLUGIN_LICENSE "MIT License")
set(COMMONLIB_PLUGIN_DESCRIPTION "Perk Conditions Framework")

set(COMMONLIB_RUNTIME_VR OFF CACHE BOOL "" FORCE)
set(COMMONLIB_RUNTIME_OG OFF CACHE BOOL "" FORCE)
set(COMMONLIB_RUNTIME_NG ON CACHE BOOL "" FORCE)
set(COMMONLIB_RUNTIME_AE ON CACHE BOOL "" FORCE)

if (NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "The Fallout 4 DLL requires Windows x64 and the MSVC toolchain.")
endif ()

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.h.in
	${CMAKE_CURRENT_BINARY_DIR}/hdr/Version.h
	@ONLY
)

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
	${CMAKE_CURRENT_BINARY_DIR}/version.rc
	@ONLY
)

# ---- Include guards ----

if (PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
	message(
		FATAL_ERROR
		"In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
	)
endif ()

# ---- Globals ----

if (NOT ${CMAKE_GENERATOR} STREQUAL "Ninja")
	add_compile_options(
		/MP	# Build with Multiple Processes
	)
endif ()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

set(Boost_USE_STATIC_LIBS ON)

# ---- Dependencies ----

if (NOT DEFINED CommonLibF4Path)
	message(FATAL_ERROR "CommonLibF4Path is not set.")
endif ()

if (NOT EXISTS "${CommonLibF4Path}/CMakeLists.txt")
	message(FATAL_ERROR "CommonLibF4 AV was not found at: ${CommonLibF4Path}")
endif ()

if (NOT DEFINED TOMLPLUSPLUS_PATH)
	message(FATAL_ERROR "TOMLPLUSPLUS_PATH is not set.")
endif ()

if (NOT EXISTS "${TOMLPLUSPLUS_PATH}/toml.hpp")
	message(FATAL_ERROR "toml.hpp was not found at: ${TOMLPLUSPLUS_PATH}")
endif ()

include("${CommonLibF4Path}/cmake/GenerateXsePluginInfo.cmake")
generate_xse_plugin_info("${CommonLibF4Path}")

if (NOT TARGET CommonLibF4)
	add_subdirectory("${CommonLibF4Path}" CommonLibF4)
endif ()

find_package(fmt REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)
find_package(ZLIB REQUIRED)

# ---- Add source files ----

include(cmake/sourcelist.cmake)

source_group(
	TREE ${CMAKE_CURRENT_SOURCE_DIR}
	FILES ${SOURCES}
)

source_group(
	TREE ${CMAKE_CURRENT_BINARY_DIR}
	FILES ${CMAKE_CURRENT_BINARY_DIR}/hdr/Version.h
)

# ---- Create DLL ----

add_library(
	${PROJECT_NAME}
	SHARED
	${SOURCES}
	${XSE_PLUGIN_INFO_CPP_TARGET}
	${CMAKE_CURRENT_BINARY_DIR}/version.rc
	.clang-format
)

target_compile_definitions(
	${PROJECT_NAME}
	PRIVATE
		_UNICODE
)

target_compile_features(
	${PROJECT_NAME}
	PRIVATE
		cxx_std_23
)

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}/hdr
		${CMAKE_CURRENT_BINARY_DIR}/include
		${CMAKE_CURRENT_SOURCE_DIR}/hdr
		${TOMLPLUSPLUS_PATH}
)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
		CommonLibF4::CommonLibF4
		fmt::fmt
		spdlog::spdlog
		ZLIB::ZLIB
)

target_precompile_headers(
	${PROJECT_NAME}
	PRIVATE
		hdr/PCH.h
)

target_compile_options(
	${PROJECT_NAME}
	PRIVATE
		/sdl	# Enable Additional Security Checks
		/utf-8	# Set Source and Executable character sets to UTF-8
		/Zi	# Debug Information Format

		/permissive-	# Standards conformance
		/Zc:preprocessor	# Enable preprocessor conformance mode

		/W4	# Warning level
		/wd5311
		/WX	# Warning level (warnings are errors)

		"$<$<CONFIG:RELEASE>:/Zc:inline;/JMC->"
)

target_link_options(
	${PROJECT_NAME}
	PRIVATE
		/WX	# Treat Linker Warnings as Errors

		"$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
		"$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
)

# ---- File copying ----

if (COPY_BUILD)
	if (DEFINED Fallout4Path)
		add_custom_command(
			TARGET ${PROJECT_NAME}
			POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> ${Fallout4Path}/Data/F4SE/Plugins/
			COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> ${Fallout4Path}/Data/F4SE/Plugins/
		)
	else ()
		message(
			WARNING
			"Variable Fallout4Path is not defined. Skipping post-build copy command."
		)
	endif ()
endif ()

find_package(Git REQUIRED)

function(pcf_checkout PATH URL COMMIT)
	if (NOT EXISTS "${PATH}/.git")
		if (EXISTS "${PATH}")
			message(FATAL_ERROR "Not a Git checkout: ${PATH}")
		endif ()
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" clone --no-checkout "${URL}" "${PATH}"
			COMMAND_ERROR_IS_FATAL ANY
		)
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" -C "${PATH}" cat-file -e "${COMMIT}^{commit}"
			RESULT_VARIABLE found ERROR_QUIET
		)
		if (NOT found STREQUAL "0")
			execute_process(
				COMMAND "${GIT_EXECUTABLE}" -C "${PATH}" fetch origin "${COMMIT}"
				COMMAND_ERROR_IS_FATAL ANY
			)
		endif ()
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" -C "${PATH}" checkout --detach "${COMMIT}"
			COMMAND_ERROR_IS_FATAL ANY
		)
		set(changed ON)
	else ()
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" -C "${PATH}" diff --quiet --ignore-submodules=all HEAD --
			RESULT_VARIABLE dirty
		)
		if (NOT dirty STREQUAL "0")
			message(FATAL_ERROR "Dependency has tracked changes or is invalid: ${PATH}")
		endif ()
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" -C "${PATH}" rev-parse --verify HEAD
			OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE
			COMMAND_ERROR_IS_FATAL ANY
		)
		set(changed OFF)
		if (NOT head STREQUAL COMMIT)
			execute_process(
				COMMAND "${GIT_EXECUTABLE}" -C "${PATH}" cat-file -e "${COMMIT}^{commit}"
				RESULT_VARIABLE found ERROR_QUIET
			)
			if (NOT found STREQUAL "0")
				execute_process(
					COMMAND "${GIT_EXECUTABLE}" -C "${PATH}" fetch origin "${COMMIT}"
					COMMAND_ERROR_IS_FATAL ANY
				)
			endif ()
			execute_process(
				COMMAND "${GIT_EXECUTABLE}" -C "${PATH}" checkout --detach "${COMMIT}"
				COMMAND_ERROR_IS_FATAL ANY
			)
			set(changed ON)
		endif ()
	endif ()
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" -C "${PATH}" rev-parse --verify HEAD
		OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE
		COMMAND_ERROR_IS_FATAL ANY
	)
	if (NOT head STREQUAL COMMIT)
		message(FATAL_ERROR "Dependency commit mismatch: ${PATH}")
	endif ()
	set(PCF_CHECKOUT_CHANGED "${changed}" PARENT_SCOPE)
endfunction()

set(deps "${CMAKE_CURRENT_LIST_DIR}/../.deps")
cmake_path(NORMAL_PATH deps)
file(MAKE_DIRECTORY "${deps}")
set(CommonLibF4Path "${deps}/CommonLibF4")
set(TOMLPLUSPLUS_PATH "${deps}/tomlplusplus")
set(VCPKG_ROOT "${deps}/vcpkg")

message(STATUS "Checking CommonLibF4 AV")
pcf_checkout("${CommonLibF4Path}" "https://github.com/LucaDotGit/CommonLibF4.git" "16b9943e0136bdecad8845b87b2de5ddb7f34f77")
execute_process(
	COMMAND "${GIT_EXECUTABLE}" -C "${CommonLibF4Path}" submodule update --init --recursive
	COMMAND_ERROR_IS_FATAL ANY
)

message(STATUS "Checking toml++")
pcf_checkout("${TOMLPLUSPLUS_PATH}" "https://github.com/marzer/tomlplusplus.git" "30172438cee64926dc41fdd9c11fb3ba5b2ba9de")

message(STATUS "Checking vcpkg")
pcf_checkout("${VCPKG_ROOT}" "https://github.com/microsoft/vcpkg.git" "a1cae005c39be7b18ba319fced856b68d7276271")
if (PCF_CHECKOUT_CHANGED OR NOT EXISTS "${VCPKG_ROOT}/vcpkg.exe")
	execute_process(
		COMMAND cmd.exe /d /c bootstrap-vcpkg.bat -disableMetrics
		WORKING_DIRECTORY "${VCPKG_ROOT}"
		COMMAND_ERROR_IS_FATAL ANY
	)
endif ()
if (NOT EXISTS "${VCPKG_ROOT}/vcpkg.exe" OR NOT EXISTS "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
	message(FATAL_ERROR "vcpkg bootstrap incomplete: ${VCPKG_ROOT}")
endif ()

set(ENV{VCPKG_ROOT} "${VCPKG_ROOT}")
set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain" FORCE)
set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")

cmake_minimum_required(VERSION 3.25)

project(WoundMeshes VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT MSVC)
	message(FATAL_ERROR "Build this with Visual Studio (MSVC), x64.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(LIB_F4     "${CMAKE_CURRENT_SOURCE_DIR}/lib/commonlibf4")
set(LIB_SHARED "${LIB_F4}/lib/commonlib-shared")

# Compiler flags CommonLibF4 expects (taken from its xmake build).
set(XSE_FLAGS
	/EHsc /permissive- /utf-8 /bigobj /MP
	/Zc:preprocessor /Zc:enumTypes /Zc:templateScope /Zc:__cplusplus
	/fp:contract /fp:except- /guard:cf-
	/external:W0 /diagnostics:caret
	/wd4200 /wd4201 /wd4324
	/we4715
)

# ---------------------------------------------------------------- spdlog (bundled)
set(SPDLOG_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT ON  CACHE BOOL "" FORCE)
set(SPDLOG_WCHAR_SUPPORT  ON  CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/spdlog EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------- commonlib-shared
file(GLOB_RECURSE SHARED_SOURCES CONFIGURE_DEPENDS "${LIB_SHARED}/src/*.cpp")
add_library(commonlib_shared STATIC ${SHARED_SOURCES})
target_include_directories(commonlib_shared PUBLIC "${LIB_SHARED}/include")
target_compile_options(commonlib_shared PUBLIC ${XSE_FLAGS})
target_link_libraries(commonlib_shared PUBLIC
	spdlog::spdlog
	advapi32 bcrypt d3d11 d3dcompiler dbghelp dxgi ole32 shell32 user32 version ws2_32
)
target_precompile_headers(commonlib_shared PRIVATE "${LIB_SHARED}/src/REX/PCH.h")

# ---------------------------------------------------------------- commonlibf4
file(GLOB_RECURSE F4_SOURCES CONFIGURE_DEPENDS "${LIB_F4}/src/*.cpp")
add_library(commonlibf4 STATIC ${F4_SOURCES})
target_include_directories(commonlibf4 PUBLIC "${LIB_F4}/include")
target_link_libraries(commonlibf4 PUBLIC commonlib_shared)
target_precompile_headers(commonlibf4 PRIVATE "${LIB_F4}/include/F4SE/Impl/PCH.h")

# ---------------------------------------------------------------- the plugin
configure_file(cmake/plugin.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/plugin_version.cpp" @ONLY)
configure_file(cmake/plugin.rc.in  "${CMAKE_CURRENT_BINARY_DIR}/generated/plugin.rc" @ONLY)

add_library(WoundMeshes SHARED
	src/main.cpp
	src/pch.h
	"${CMAKE_CURRENT_BINARY_DIR}/generated/plugin_version.cpp"
	"${CMAKE_CURRENT_BINARY_DIR}/generated/plugin.rc"
)
target_include_directories(WoundMeshes PRIVATE src)
target_link_libraries(WoundMeshes PRIVATE commonlibf4)
target_precompile_headers(WoundMeshes PRIVATE src/pch.h)

# After every build, copy the DLL into the "output" folder next to this file.
add_custom_command(TARGET WoundMeshes POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/output"
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:WoundMeshes>" "${CMAKE_CURRENT_SOURCE_DIR}/output/"
	COMMENT "Copying WoundMeshes.dll to the output folder"
)

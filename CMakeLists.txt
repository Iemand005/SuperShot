cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
project(SuperShot)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows 7 targeting is set globally in root CMakeLists.txt via add_compile_definitions
if(MSVC)
    set(CMAKE_SYSTEM_VERSION 6.1) # Corresponds to Windows 7 internal version
endif()

# Ensure the Windows SDK exposes modern API structs like AudioClientProperties to SDL3
add_compile_definitions(AUDIO_CLIENT_PROPERTIES_ENUM_DEFINED)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(KISSFFT_TEST OFF CACHE BOOL "Disable KissFFT tests" FORCE)

set(KISSFFT_STATIC ON CACHE BOOL "Build static library" FORCE)

set(FE_EXCLUDE_JOLT OFF CACHE BOOL "Disable JOLT" FORCE)

option(SUPERSHOT_USE_VULKAN "Use Vulkan for SuperShot rendering" ON)

if(EMSCRIPTEN)
	set(SUPERSHOT_USE_VULKAN OFF CACHE BOOL "Use Vulkan for SuperShot rendering" FORCE)
endif()

set(ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../FenixEngine")

if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
endif()

if(NOT TARGET Engine)
    message(STATUS "Engine target not found. Looking for engine source...")

    if(EXISTS "${ENGINE_DIR}/CMakeLists.txt")
        add_subdirectory("${ENGINE_DIR}" FenixEngine)
    else()
        message(FATAL_ERROR "FenixEngine not found!")
    endif()
endif()

set(IMGUI_DIR ../FenixEngine/external/imgui)

file(GLOB_RECURSE IMGUI_SOURCE_FILES
	${IMGUI_DIR}/imgui.cpp
	${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
	${IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp
	${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
	${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
	${IMGUI_DIR}/imgui_tables.cpp
	${IMGUI_DIR}/imgui_widgets.cpp
	${IMGUI_DIR}/imgui_demo.cpp
	${IMGUI_DIR}/imgui_draw.cpp
)

set_source_files_properties(${IMGUI_SOURCE_FILES} PROPERTIES
	SKIP_UNITY_BUILD_INCLUSION ON
)

set(SUPERSHOT_SOURCES
	${IMGUI_SOURCE_FILES}
	main.cpp
)

if(ANDROID)
	add_library(main SHARED ${SUPERSHOT_SOURCES})
	set(SUPERSHOT_TARGET main)
	target_link_libraries(${SUPERSHOT_TARGET} PRIVATE android log)
elseif(EMSCRIPTEN)
	add_executable(SuperShot ${SUPERSHOT_SOURCES})
	set(SUPERSHOT_TARGET SuperShot)
else()
	add_executable(SuperShot
		WIN32
		${SUPERSHOT_SOURCES}
	)
	set(SUPERSHOT_TARGET SuperShot)
endif()

target_include_directories(${SUPERSHOT_TARGET} PRIVATE
	include
	${ENGINE_DIR}
	${ENGINE_DIR}/include
	${ENGINE_DIR}/external
	${ENGINE_DIR}/external/imgui
	${ENGINE_DIR}/external/kissfft
	${ENGINE_DIR}/external/SDL/include
	${ENGINE_DIR}/src
)

target_link_libraries(${SUPERSHOT_TARGET} PRIVATE
	Engine
)

if(SUPERSHOT_USE_VULKAN AND (ANDROID OR EMSCRIPTEN OR FE_EXCLUDE_VULKAN))
	message(FATAL_ERROR "SUPERSHOT_USE_VULKAN requires the desktop Vulkan backend.")
endif()

# The Engine defines FE_HAS_VULKAN inside its own directory, so those defines do
# not reach this sibling target. Keep the backend available to game code even
# when OpenGL is selected as the active renderer.
if(NOT ANDROID AND NOT EMSCRIPTEN AND NOT FE_EXCLUDE_VULKAN)
	target_compile_definitions(${SUPERSHOT_TARGET} PRIVATE FE_HAS_VULKAN)
endif()
if(SUPERSHOT_USE_VULKAN)
	target_compile_definitions(${SUPERSHOT_TARGET} PRIVATE SUPERSHOT_USE_VULKAN)
endif()

target_precompile_headers(${SUPERSHOT_TARGET} PRIVATE
	"$<$<COMPILE_LANGUAGE:CXX>:${ENGINE_DIR}/src/pch.hpp>"
)

if(NOT WIN32 AND NOT EMSCRIPTEN AND NOT ANDROID)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
target_include_directories(${SUPERSHOT_TARGET} PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
target_link_libraries(${SUPERSHOT_TARGET} PRIVATE ${PIPEWIRE_LIBRARIES})
target_compile_options(${SUPERSHOT_TARGET} PRIVATE ${PIPEWIRE_CFLAGS_OTHER})

find_package(X11 REQUIRED)

target_link_libraries(${SUPERSHOT_TARGET} PRIVATE X11::X11)
endif()

if(MSVC)
	target_compile_options(${SUPERSHOT_TARGET} PRIVATE /EHsc)
	target_link_options(${SUPERSHOT_TARGET} PRIVATE $<$<CONFIG:Debug>:/DEBUG>)

	add_compile_options(/RTC1)
endif()

set_property(TARGET ${SUPERSHOT_TARGET} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# ---------------------------------------------------------------
# Vulkan runtime resources + GLSL -> SPIR-V shader compilation
# ---------------------------------------------------------------
if(NOT ANDROID)
	set(RESOURCE_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/resources)
	set(RESOURCE_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/resources)

	add_custom_target(CopyResources_SuperShot ALL
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${RESOURCE_SRC_DIR} ${RESOURCE_OUT_DIR}
		COMMENT "Copying SuperShot resources to ${RESOURCE_OUT_DIR}"
	)
	add_dependencies(${SUPERSHOT_TARGET} CopyResources_SuperShot)

	if(SUPERSHOT_USE_VULKAN)
		if(Vulkan_GLSLC_EXECUTABLE AND EXISTS "${Vulkan_GLSLC_EXECUTABLE}")
			set(GLSLC "${Vulkan_GLSLC_EXECUTABLE}")
		else()
			if(GLSLC AND NOT EXISTS "${GLSLC}")
				unset(GLSLC CACHE)
			endif()
			find_program(GLSLC glslc HINTS
				$ENV{VULKAN_SDK}/Bin
				$ENV{VULKAN_SDK}/bin
			)
		endif()

		if(GLSLC)
			message(STATUS "Found glslc: ${GLSLC}")

			set(SHADER_SRC_DIR ${RESOURCE_SRC_DIR}/shaders)
			set(SHADER_OUT_DIR ${RESOURCE_OUT_DIR}/shaders)
			file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

			set(SHADER_SOURCES
				${SHADER_SRC_DIR}/VertexShader_vk.glsl
				${SHADER_SRC_DIR}/FragmentShader_vk.glsl
				${SHADER_SRC_DIR}/VertexShader_vk_foxcraft.glsl
				${SHADER_SRC_DIR}/FragmentShader_vk_array.glsl
			)

			foreach(SHADER ${SHADER_SOURCES})
				get_filename_component(SHADER_NAME ${SHADER} NAME_WE)

				if(${SHADER_NAME} MATCHES ".*Fragment.*")
					set(SHADER_STAGE frag)
				else()
					set(SHADER_STAGE vert)
				endif()

				set(SPIRV_OUTPUT ${SHADER_OUT_DIR}/${SHADER_NAME}.spv)

				add_custom_command(
					OUTPUT ${SPIRV_OUTPUT}
					COMMAND ${GLSLC} -fshader-stage=${SHADER_STAGE} --target-env=vulkan1.0 -DFE_VULKAN ${SHADER} -o ${SPIRV_OUTPUT}
					DEPENDS ${SHADER}
					COMMENT "Compiling ${SHADER_NAME} -> ${SHADER_NAME}.spv"
					VERBATIM
				)

				list(APPEND SPIRV_OUTPUTS ${SPIRV_OUTPUT})
			endforeach()

			add_custom_target(SPIRV_ShadersSuperShot ALL DEPENDS ${SPIRV_OUTPUTS})
			add_dependencies(SPIRV_ShadersSuperShot CopyResources_SuperShot)
			add_dependencies(${SUPERSHOT_TARGET} SPIRV_ShadersSuperShot)
		else()
			message(FATAL_ERROR "SUPERSHOT_USE_VULKAN requires glslc. Set VULKAN_SDK or install the Vulkan SDK.")
		endif()
	endif()

	# Keep resources next to the executable too, so SDL_GetBasePath() resolves the
	# SPIR-V regardless of the launcher's working directory (VS, VS Code, exe).
	add_custom_command(TARGET ${SUPERSHOT_TARGET} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory ${RESOURCE_OUT_DIR} $<TARGET_FILE_DIR:${SUPERSHOT_TARGET}>/resources
		COMMENT "Copying SuperShot resources next to the executable"
	)

	# The shared voxel block textures live in the repo-root Assets folder. Copy
	# them next to the executable so ChunkMesher::BuildBlockTextureImages() can
	# resolve "Assets/Grass.png" via ImageLoader's executable-directory fallback
	# regardless of the working directory.
	set(ASSETS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../Assets)
	if(EXISTS ${ASSETS_SRC_DIR})
		add_custom_command(TARGET ${SUPERSHOT_TARGET} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_directory ${ASSETS_SRC_DIR} $<TARGET_FILE_DIR:${SUPERSHOT_TARGET}>/Assets
			COMMENT "Copying shared Assets next to the executable"
		)
	endif()
endif()

include(${CMAKE_SOURCE_DIR}/EmscriptenConfig.cmake)
if(EMSCRIPTEN)
	file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../Assets" SUPERSHOT_ASSETS_PATH)
	setup_emscripten_target(SuperShot
		RESOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/resources
		EXTRA_LINK_OPTIONS
			--preload-file=${SUPERSHOT_ASSETS_PATH}@Assets
	)

	if(CMAKE_BUILD_TYPE STREQUAL "Release")
		set(SUPERSHOT_PAGES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../FenixWeb/docs/SuperShot)
		add_custom_command(TARGET SuperShot POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E make_directory "${SUPERSHOT_PAGES_DIR}"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE_DIR:SuperShot>/SuperShot.wasm" "${SUPERSHOT_PAGES_DIR}/SuperShot.wasm"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE_DIR:SuperShot>/SuperShot.js" "${SUPERSHOT_PAGES_DIR}/SuperShot.js"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE_DIR:SuperShot>/SuperShot.data" "${SUPERSHOT_PAGES_DIR}/SuperShot.data"
			COMMENT "Publishing SuperShot build artifacts to ${SUPERSHOT_PAGES_DIR} (Release only)"
		)
	endif()
endif()

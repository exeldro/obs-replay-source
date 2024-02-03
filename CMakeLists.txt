# --- Detect if the plugin is build out of tree or not ---
if(CMAKE_PROJECT_NAME STREQUAL "obs-studio")
  set(BUILD_OUT_OF_TREE OFF)
  if(OBS_CMAKE_VERSION VERSION_GREATER_EQUAL 3.0.0)
    legacy_check()
  endif()
else()
  set(BUILD_OUT_OF_TREE ON)
  cmake_minimum_required(VERSION 3.18)
endif()

project(replay-source VERSION 1.7.0)
set(PROJECT_FULL_NAME "Replay Source")

# Set new UUIDs when you start to create a new plugin.
set(MACOS_PACKAGE_UUID "A0B79A76-08BC-420B-AEB9-D1334C3C6244")
set(MACOS_INSTALLER_UUID "22902D34-AB9A-49EE-8012-F60DCAA89EB8")

add_library(${PROJECT_NAME} MODULE)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/version.h)

target_sources(${PROJECT_NAME} PRIVATE
	replay.c
	replay-source.c
	replay-filter.c
	replay-filter-audio.c
	replay-filter-async.c
	replay.h
	version.h)

if(BUILD_OUT_OF_TREE)
  find_package(libobs REQUIRED)
  find_package(obs-frontend-api REQUIRED)
  include(cmake/ObsPluginHelpers.cmake)
  set(OBS_FRONTEND_API_NAME "obs-frontend-api")
else()
	set(OBS_FRONTEND_API_NAME "frontend-api")
endif()

if(OS_WINDOWS)
	get_filename_component(ISS_FILES_DIR "${CMAKE_BINARY_DIR}\\..\\package" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_FILES_DIR}" ISS_FILES_DIR)
	get_filename_component(ISS_PACKAGE_DIR "${CMAKE_PACKAGE_PREFIX}\\.." ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_PACKAGE_DIR}" ISS_PACKAGE_DIR)
	get_filename_component(ISS_SOURCE_DIR "${PROJECT_SOURCE_DIR}" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_SOURCE_DIR}" ISS_SOURCE_DIR)
	configure_file("installer.iss.in"
			"${PROJECT_BINARY_DIR}/installer.iss"
		)
	configure_file(${CMAKE_CURRENT_SOURCE_DIR}/resource.rc.in ${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.rc)
	target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.rc)
	
	find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil)
	include_directories(${FFMPEG_INCLUDE_DIRS})
	
	find_package(libdshowcapture)
	if(OBS_CMAKE_VERSION VERSION_LESS 3.0.0)
		find_package(libdshowcapture-external)
	endif()
	target_sources(${PROJECT_NAME} PRIVATE
		win-dshow-replay.cpp
		../win-dshow/ffmpeg-decode.c
		replay-source.rc
		../win-dshow/ffmpeg-decode.h)

	target_link_libraries(${PROJECT_NAME}
		OBS::w32-pthreads
		OBS::libdshowcapture
		setupapi
		strmiids
		ksuser
		winmm
		wmcodecdspuuid
		FFmpeg::avcodec
		FFmpeg::avutil)
	if(OBS_CMAKE_VERSION VERSION_LESS 3.0.0)
		target_link_libraries(${PROJECT_NAME} OBS::libdshowcapture-external)
	endif()
elseif(OS_MACOS)
		set(MACOS_BUNDLEID "com.exeldro.${PROJECT_NAME}")
		set(MACOSX_PLUGIN_GUI_IDENTIFIER "${MACOS_BUNDLEID}")
		set(MACOSX_PLUGIN_BUNDLE_VERSION "${PROJECT_VERSION}")
		set(MACOSX_PLUGIN_SHORT_VERSION_STRING "1")
		configure_file(cmake/bundle/macos/installer-macos.pkgproj.in ${CMAKE_BINARY_DIR}/installer-macos.generated.pkgproj)
		target_compile_options(${PROJECT_NAME} PRIVATE -Wall)
endif()

target_link_libraries(${PROJECT_NAME}
	OBS::${OBS_FRONTEND_API_NAME}
	OBS::libobs)

if(BUILD_OUT_OF_TREE)
    if(NOT LIB_OUT_DIR)
        set(LIB_OUT_DIR "/lib/obs-plugins")
    endif()
    if(NOT DATA_OUT_DIR)
        set(DATA_OUT_DIR "/share/obs/obs-plugins/${PROJECT_NAME}")
    endif()
    set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
    install(TARGETS ${PROJECT_NAME}
        LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/${LIB_OUT_DIR})
    install(DIRECTORY data/locale
        DESTINATION ${CMAKE_INSTALL_PREFIX}/${DATA_OUT_DIR})
    setup_plugin_target(${PROJECT_NAME})
else()
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/UI/obs-frontend-api")
    if(OBS_CMAKE_VERSION VERSION_GREATER_EQUAL 3.0.0)
        set_target_properties_obs(${PROJECT_NAME} PROPERTIES FOLDER "plugins/exeldro" PREFIX "")
    else()
        set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "plugins/exeldro")
        setup_plugin_target(${PROJECT_NAME})
    endif()
endif()

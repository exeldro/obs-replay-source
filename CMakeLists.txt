project(replay-source VERSION 1.6.8)
set(PROJECT_FULL_NAME "Replay Source")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/version.h)

if(MSVC)
	set(replay-source_PLATFORM_DEPS
		w32-pthreads)
endif()

set(replay-source_HEADERS
	replay.h
	version.h)
set(replay-source_SOURCES
	replay.c
	replay-source.c
	replay-filter.c
	replay-filter-audio.c
	replay-filter-async.c)

if(WIN32)
	get_filename_component(ISS_FILES_DIR "${CMAKE_BINARY_DIR}\\..\\package" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_FILES_DIR}" ISS_FILES_DIR)
	get_filename_component(ISS_PACKAGE_DIR "${CMAKE_PACKAGE_PREFIX}\\.." ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_PACKAGE_DIR}" ISS_PACKAGE_DIR)
	get_filename_component(ISS_SOURCE_DIR "${PROJECT_SOURCE_DIR}" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_SOURCE_DIR}" ISS_SOURCE_DIR)
	configure_file("installer.iss.in"
			"${PROJECT_BINARY_DIR}/installer.iss"
		)
	configure_file(resource.rc.in replay-source.rc)
	find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil)
	include_directories(${FFMPEG_INCLUDE_DIRS})
	
	list(APPEND replay-source_HEADERS
		../win-dshow/ffmpeg-decode.h)
	list(APPEND replay-source_SOURCES
		win-dshow-replay.cpp
		../win-dshow/ffmpeg-decode.c
		replay-source.rc)

	list(APPEND replay-source_PLATFORM_DEPS
		strmiids
		ksuser
		wmcodecdspuuid)

	set(libdshowcapture_SOURCES
		../win-dshow/libdshowcapture/source/capture-filter.cpp
		../win-dshow/libdshowcapture/source/output-filter.cpp
		../win-dshow/libdshowcapture/source/dshowcapture.cpp
		../win-dshow/libdshowcapture/source/dshowencode.cpp
		../win-dshow/libdshowcapture/source/device.cpp
		../win-dshow/libdshowcapture/source/encoder.cpp
		../win-dshow/libdshowcapture/source/dshow-base.cpp
		../win-dshow/libdshowcapture/source/dshow-demux.cpp
		../win-dshow/libdshowcapture/source/dshow-enum.cpp
		../win-dshow/libdshowcapture/source/dshow-formats.cpp
		../win-dshow/libdshowcapture/source/dshow-media-type.cpp
		../win-dshow/libdshowcapture/source/dshow-encoded-device.cpp
		../win-dshow/libdshowcapture/source/log.cpp)

	set(libdshowcapture_HEADERS
		../win-dshow/libdshowcapture/dshowcapture.hpp
		../win-dshow/libdshowcapture/source/external/IVideoCaptureFilter.h
		../win-dshow/libdshowcapture/source/capture-filter.hpp
		../win-dshow/libdshowcapture/source/output-filter.hpp
		../win-dshow/libdshowcapture/source/device.hpp
		../win-dshow/libdshowcapture/source/encoder.hpp
		../win-dshow/libdshowcapture/source/dshow-base.hpp
		../win-dshow/libdshowcapture/source/dshow-demux.hpp
		../win-dshow/libdshowcapture/source/dshow-device-defs.hpp
		../win-dshow/libdshowcapture/source/dshow-enum.hpp
		../win-dshow/libdshowcapture/source/dshow-formats.hpp
		../win-dshow/libdshowcapture/source/dshow-media-type.hpp
		../win-dshow/libdshowcapture/source/log.hpp)
endif()

add_library(replay-source MODULE
	${replay-source_HEADERS}
	${replay-source_SOURCES}
	${libdshowcapture_SOURCES}
	${libdshowcapture_HEADERS})
target_link_libraries(replay-source
	obs-frontend-api
	libobs
	${FFMPEG_LIBRARIES}
	${replay-source_PLATFORM_DEPS})

set_target_properties(replay-source PROPERTIES FOLDER "plugins/exeldro")
install_obs_plugin_with_data(replay-source data)

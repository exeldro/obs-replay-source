project(replay-source)

if(MSVC)
	set(replay-source_PLATFORM_DEPS
		w32-pthreads)
endif()

find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil)
include_directories(${FFMPEG_INCLUDE_DIRS})

set(replay-source_HEADERS
	replay.h
	../win-dshow/ffmpeg-decode.h)
set(replay-source_SOURCES
	replay.c
	replay-source.c
	replay-filter.c
	replay-filter-audio.c
	replay-filter-async.c
	win-dshow-replay.cpp
	../win-dshow/ffmpeg-decode.c)

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
	../win-dshow/libdshowcapture/source/IVideoCaptureFilter.h
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

add_library(replay-source MODULE
	${replay-source_HEADERS}
	${replay-source_SOURCES}
	${libdshowcapture_SOURCES}
	${libdshowcapture_HEADERS})
target_link_libraries(replay-source
	obs-frontend-api
	libobs
	strmiids
	ksuser
	wmcodecdspuuid
	${FFMPEG_LIBRARIES}
	${replay-source_PLATFORM_DEPS})

install_obs_plugin_with_data(replay-source data)

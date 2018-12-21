project(replay-source)

if(MSVC)
	set(replay-source_PLATFORM_DEPS
		w32-pthreads)
endif()

set(replay-source_HEADERS
	replay.h)
set(replay-source_SOURCES
	replay.c
	replay-source.c
	replay-filter.c
	replay-filter-audio.c
	replay-filter-async.c)

add_library(replay-source MODULE
	${replay-source_HEADERS}
	${replay-source_SOURCES})
target_link_libraries(replay-source
	obs-frontend-api
	libobs
	${replay-source_PLATFORM_DEPS})

install_obs_plugin_with_data(replay-source data)

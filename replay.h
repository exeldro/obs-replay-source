#pragma once
#include <obs-module.h>
#include <util/circlebuf.h>

struct replay_filter {

	/* contains struct obs_source_frame* */
	struct circlebuf               video_frames;

	/* stores the audio data */
	struct circlebuf               audio_frames;
	struct obs_audio_data          audio_output;

	uint64_t                       duration;
	bool                           reset_video;
	bool                           reset_audio;
	obs_source_t *src;
};

void obs_source_frame_copy(struct obs_source_frame * dst,const struct obs_source_frame *src);
void free_audio_packet(struct obs_audio_data *audio);

#define REPLAY_FILTER_ID               "replay_filter"
#define TEXT_FILTER_NAME               obs_module_text("ReplayFilter")
#define REPLAY_SOURCE_ID               "replay_source"
#define SETTING_DURATION               "duration"
#define TEXT_DURATION                  obs_module_text("Duration")
#define SETTING_LOOP                   "loop"
#define TEXT_LOOP                      obs_module_text("Loop")
#define SETTING_SOURCE                 "source"
#define TEXT_SOURCE                    obs_module_text("Source")

#ifndef SEC_TO_NSEC
#define SEC_TO_NSEC 1000000000ULL
#endif

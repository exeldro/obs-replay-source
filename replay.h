#pragma once
#include <obs-module.h>
#include <util/circlebuf.h>

struct replay_filter {

	/* contains struct obs_source_frame* */
	struct circlebuf               video_frames;

	/* stores the audio data */
	struct circlebuf               audio_frames;
	struct obs_audio_data          audio_output;

	struct obs_video_info ovi;
	struct obs_audio_info oai;

	gs_texrender_t* texrender;
	gs_stagesurf_t* stagesurface;

	uint32_t known_width;
	uint32_t known_height;

	uint8_t* video_data;
	uint32_t video_linesize;
	video_t* video_output;

	uint64_t                       duration;
	bool                           reset_video;
	bool                           reset_audio;
	obs_source_t *src;
};

void obs_source_frame_copy(struct obs_source_frame * dst,const struct obs_source_frame *src);
void free_audio_packet(struct obs_audio_data *audio);

#define REPLAY_FILTER_ID               "replay_filter"
#define TEXT_FILTER_NAME               obs_module_text("ReplayFilter")
#define REPLAY_FILTER_ASYNC_ID         "replay_filter_async"
#define TEXT_FILTER_ASYNC_NAME         obs_module_text("ReplayFilterAsync")
#define REPLAY_SOURCE_ID               "replay_source"
#define SETTING_DURATION               "duration"
#define TEXT_DURATION                  obs_module_text("Duration")
#define SETTING_SPEED                  "speed_percent"
#define SETTING_VISIBILITY_ACTION      "visibility_action"
#define SETTING_START_DELAY            "start_delay"
#define TEXT_START_DELAY               "Start delay (ms)"
#define SETTING_END_ACTION             "end_action"
#define SETTING_SOURCE                 "source"
#define TEXT_SOURCE                    obs_module_text("Source")
#define SETTING_NEXT_SCENE             "next_scene"
#define TEXT_NEXT_SCENE                obs_module_text("NextScene")

#ifndef SEC_TO_NSEC
#define SEC_TO_NSEC 1000000000ULL
#endif

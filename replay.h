#pragma once
#include <obs-module.h>
#include <util/circlebuf.h>
#include <util/threading.h>

struct replay_filter {

	/* contains struct obs_source_frame* */
	struct circlebuf               video_frames;

	/* stores the audio data */
	struct circlebuf               audio_frames;

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
	obs_source_t *src;
	pthread_mutex_t    mutex;
	int64_t timing_adjust;
};

void obs_source_frame_copy(struct obs_source_frame * dst,const struct obs_source_frame *src);
void free_audio_packet(struct obs_audio_data *audio);
struct obs_audio_data *replay_filter_audio(void *data,struct obs_audio_data *audio);
void free_video_data(struct replay_filter *filter);
void free_audio_data(struct replay_filter *filter);
void obs_enum_scenes(bool (*enum_proc)(void*, obs_source_t*),void *param);
obs_properties_t *replay_filter_properties(void *unused);

#define REPLAY_FILTER_ID               "replay_filter"
#define TEXT_FILTER_NAME               obs_module_text("ReplayFilter")
#define REPLAY_FILTER_AUDIO_ID         "replay_filter_audio"
#define TEXT_FILTER_AUDIO_NAME         obs_module_text("ReplayFilterAudio")
#define REPLAY_FILTER_ASYNC_ID         "replay_filter_async"
#define TEXT_FILTER_ASYNC_NAME         obs_module_text("ReplayFilterAsync")
#define REPLAY_SOURCE_ID               "replay_source"
#define SETTING_DURATION               "duration"
#define TEXT_DURATION                  obs_module_text("Duration")
#define SETTING_SPEED                  "speed_percent"
#define SETTING_BACKWARD               "backward"
#define SETTING_VISIBILITY_ACTION      "visibility_action"
#define SETTING_START_DELAY            "start_delay"
#define TEXT_START_DELAY               "Start delay (ms)"
#define SETTING_END_ACTION             "end_action"
#define SETTING_SOURCE                 "source"
#define TEXT_SOURCE                    "Video source"
#define SETTING_SOURCE_AUDIO           "source_audio"
#define TEXT_SOURCE_AUDIO              "Audio source"
#define SETTING_NEXT_SCENE             "next_scene"
#define TEXT_NEXT_SCENE                obs_module_text("NextScene")
#define SETTING_DIRECTORY              "directory"
#define SETTING_FILE_FORMAT            "file_format"
#define SETTING_LOSSLESS               "lossless"

#ifndef SEC_TO_NSEC
#define SEC_TO_NSEC 1000000000ULL
#endif

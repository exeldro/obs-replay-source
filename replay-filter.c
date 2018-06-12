#include <obs-module.h>
#include <util/circlebuf.h>
#include <util/threading.h>
#include "replay.h"

#ifndef SEC_TO_NSEC
#define SEC_TO_NSEC 1000000000ULL
#endif

static void free_video_data(struct replay_filter *filter,
		obs_source_t *parent)
{
	while (filter->video_frames.size) {
		struct obs_source_frame *frame;

		circlebuf_pop_front(&filter->video_frames, &frame,
				sizeof(struct obs_source_frame*));
		//obs_source_release_frame(parent, frame);
		if (os_atomic_dec_long(&frame->refs) <= 0) {
			obs_source_frame_destroy(frame);
			frame = NULL;
		}
	}
}

static inline void free_audio_packet(struct obs_audio_data *audio)
{
	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		bfree(audio->data[i]);
	memset(audio, 0, sizeof(*audio));
}

static void free_audio_data(struct replay_filter *filter)
{
	while (filter->audio_frames.size) {
		struct obs_audio_data audio;

		circlebuf_pop_front(&filter->audio_frames, &audio,
				sizeof(struct obs_audio_data));
		free_audio_packet(&audio);
	}
}

static const char *replay_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_FILTER_NAME;
}

static void replay_filter_update(void *data, obs_data_t *settings)
{
	struct replay_filter *filter = data;
	
	uint64_t new_duration = (uint64_t)obs_data_get_int(settings, SETTING_DURATION) * SEC_TO_NSEC;

	if (new_duration < filter->duration)
		free_video_data(filter, obs_filter_get_parent(filter->src));

	filter->reset_audio = true;
	filter->reset_video = true;
	filter->duration = new_duration;
}


static void *replay_filter_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(source);

	struct replay_filter *context = bzalloc(sizeof(struct replay_filter));
	struct obs_audio_info oai;
	context->src = source;

	replay_filter_update(context, settings);
	obs_get_audio_info(&oai);
	context->samplerate = oai.samples_per_sec;

	return context;
}

static void replay_filter_destroy(void *data)
{
	struct replay_filter *filter = data;

	free_audio_packet(&filter->audio_output);
	circlebuf_free(&filter->video_frames);
	circlebuf_free(&filter->audio_frames);
	
	bfree(data);
}

static obs_properties_t *replay_filter_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
	
	obs_properties_add_int(props, SETTING_DURATION, TEXT_DURATION, 1, 200, 1);

	return props;
}

static void replay_filter_remove(void *data, obs_source_t *parent)
{
	struct replay_filter *filter = data;

	free_video_data(filter, parent);
	free_audio_data(filter);
}

static struct obs_source_frame *replay_filter_video(void *data,
		struct obs_source_frame *frame)
{
	struct replay_filter *filter = data;
	obs_source_t *parent = obs_filter_get_parent(filter->src);
	struct obs_source_frame *output;
	uint64_t cur_duration;

	if (filter->reset_video) {
		free_video_data(filter, parent);
		filter->reset_video = false;
	}

	struct obs_source_frame *new_frame = obs_source_frame_create(frame->format, frame->width, frame->height);
	new_frame->refs = 1;
	obs_source_frame_copy(new_frame, frame);

	circlebuf_push_back(&filter->video_frames, &new_frame,
			sizeof(struct obs_source_frame*));

	
	circlebuf_peek_front(&filter->video_frames, &output,
			sizeof(struct obs_source_frame*));

	cur_duration = frame->timestamp - output->timestamp;
	if (cur_duration > filter->duration){

		circlebuf_pop_front(&filter->video_frames, NULL,
			sizeof(struct obs_source_frame*));

		//obs_source_release_frame(parent, output);
		if (os_atomic_dec_long(&output->refs) <= 0) {
			obs_source_frame_destroy(output);
			output = NULL;
		}
	}
	return frame;
}
static struct obs_audio_data *replay_filter_audio(void *data,
		struct obs_audio_data *audio)
{
	struct replay_filter *filter = data;
	struct obs_audio_data cached = *audio;
	return audio;
	return NULL;
	return &filter->audio_output;
}


struct obs_source_info replay_filter_info = {
	.id             = REPLAY_FILTER_ID,
	.type           = OBS_SOURCE_TYPE_FILTER,
	.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC,
	.create         = replay_filter_create,
	.destroy        = replay_filter_destroy,
	.update         = replay_filter_update,
	.get_name       = replay_filter_get_name,
	.get_properties = replay_filter_properties,
	.filter_video   = replay_filter_video,
	.filter_audio   = replay_filter_audio,
	.filter_remove  = replay_filter_remove
};

#include <obs-module.h>
#include <util/circlebuf.h>
#include <util/threading.h>
#include "replay.h"
#include "obs-internal.h"

static const char *replay_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_FILTER_ASYNC_NAME;
}

static void replay_filter_update(void *data, obs_data_t *settings)
{
	struct replay_filter *filter = data;

	const uint64_t new_duration = (uint64_t)obs_data_get_int(settings, SETTING_DURATION) * SEC_TO_NSEC;

	if (new_duration < filter->duration){
		pthread_mutex_lock(&filter->mutex);
		free_video_data(filter);
		pthread_mutex_unlock(&filter->mutex);
	}
	filter->duration = new_duration;
}


static void *replay_filter_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(source);

	struct replay_filter *context = bzalloc(sizeof(struct replay_filter));
	context->src = source;
	pthread_mutex_init(&context->mutex, NULL);

	replay_filter_update(context, settings);

	return context;
}

static void replay_filter_destroy(void *data)
{
	struct replay_filter *filter = data;

	pthread_mutex_lock(&filter->mutex);
	free_video_data(filter);
	free_audio_data(filter);
	pthread_mutex_unlock(&filter->mutex);
	circlebuf_free(&filter->video_frames);
	circlebuf_free(&filter->audio_frames);
	pthread_mutex_destroy(&filter->mutex);
	bfree(data);
}


static void replay_filter_remove(void *data, obs_source_t *parent)
{
	struct replay_filter *filter = data;
	pthread_mutex_lock(&filter->mutex);
	free_video_data(filter);
	free_audio_data(filter);
	pthread_mutex_unlock(&filter->mutex);
}

static inline uint64_t uint64_diff(uint64_t ts1, uint64_t ts2)
{
	return (ts1 < ts2) ?  (ts2 - ts1) : (ts1 - ts2);
}

static struct obs_source_frame *replay_filter_video(void *data,
		struct obs_source_frame *frame)
{
	struct replay_filter *filter = data;
	struct obs_source_frame *output;

	struct obs_source_frame *new_frame = obs_source_frame_create(frame->format, frame->width, frame->height);
	new_frame->refs = 1;
	obs_source_frame_copy(new_frame, frame);
	const uint64_t timestamp = frame->timestamp;
	uint64_t adjusted_time = timestamp + filter->timing_adjust;
	const uint64_t os_time = os_gettime_ns();
	if(filter->timing_adjust && uint64_diff(os_time, timestamp) < MAX_TS_VAR)
	{
		adjusted_time = timestamp;
		filter->timing_adjust = 0;
	} else if(uint64_diff(os_time, adjusted_time) > MAX_TS_VAR)
	{
		filter->timing_adjust = os_time - timestamp;
		adjusted_time = os_time;
	}
	new_frame->timestamp = adjusted_time;

	pthread_mutex_lock(&filter->mutex);
	circlebuf_push_back(&filter->video_frames, &new_frame,
			sizeof(struct obs_source_frame*));

	
	circlebuf_peek_front(&filter->video_frames, &output,
			sizeof(struct obs_source_frame*));

	uint64_t cur_duration = new_frame->timestamp - output->timestamp;
	while (cur_duration > 0 && cur_duration > filter->duration){

		circlebuf_pop_front(&filter->video_frames, NULL,
			sizeof(struct obs_source_frame*));

		if (os_atomic_dec_long(&output->refs) <= 0) {
			obs_source_frame_destroy(output);
			output = NULL;
		}
		if(filter->video_frames.size){
			circlebuf_peek_front(&filter->video_frames, &output, sizeof(struct obs_source_frame*));
			cur_duration = new_frame->timestamp - output->timestamp;
		}
		else
		{
			cur_duration = 0;
		}
	}
	pthread_mutex_unlock(&filter->mutex);
	return frame;
}

struct obs_source_info replay_filter_async_info = {
	.id             = REPLAY_FILTER_ASYNC_ID,
	.type           = OBS_SOURCE_TYPE_FILTER,
	.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_ASYNC,
	.create         = replay_filter_create,
	.destroy        = replay_filter_destroy,
	.update         = replay_filter_update,
	.get_name       = replay_filter_get_name,
	.get_properties = replay_filter_properties,
	.filter_video   = replay_filter_video,
	.filter_audio   = replay_filter_audio,
	.filter_remove  = replay_filter_remove,
};

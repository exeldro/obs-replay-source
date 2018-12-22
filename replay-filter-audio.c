#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>
#include <media-io/audio-resampler.h>
#include <util/circlebuf.h>
#include "replay.h"
#include "obs-internal.h"

static const char *replay_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_FILTER_AUDIO_NAME;
}

static void replay_filter_update(void *data, obs_data_t *settings)
{
	struct replay_filter *filter = data;


	const uint64_t new_duration = (uint64_t)obs_data_get_int(settings, SETTING_DURATION) * SEC_TO_NSEC;

	if (new_duration < filter->duration)
		free_video_data(filter);

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

	free_video_data(filter);
	free_audio_data(filter);
}

struct obs_source_info replay_filter_audio_info = {
	.id             = REPLAY_FILTER_AUDIO_ID,
	.type           = OBS_SOURCE_TYPE_FILTER,
	.output_flags   = OBS_SOURCE_AUDIO,
	.create         = replay_filter_create,
	.destroy        = replay_filter_destroy,
	.update         = replay_filter_update,
	.get_name       = replay_filter_get_name,
	.get_properties = replay_filter_properties,
	.filter_remove  = replay_filter_remove,
	.filter_audio   = replay_filter_audio,
};

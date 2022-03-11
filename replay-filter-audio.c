#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>
#include <media-io/audio-math.h>
#include <media-io/audio-resampler.h>
#include <util/circlebuf.h>
#include "replay.h"

static const char *replay_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ReplayFilterAudio");
}

static void replay_filter_update(void *data, obs_data_t *settings)
{
	struct replay_filter *filter = data;

	const uint64_t new_duration =
		(uint64_t)obs_data_get_int(settings, SETTING_DURATION) *
		MSEC_TO_NSEC;

	if (new_duration < filter->duration)
		free_video_data(filter);

	filter->duration = new_duration;
	const double db =
		obs_data_get_double(settings, SETTING_AUDIO_THRESHOLD);
	filter->threshold = db_to_mul((float)db);
}

static void *replay_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct replay_filter *context = bzalloc(sizeof(struct replay_filter));
	context->src = source;
	pthread_mutex_init(&context->mutex, NULL);
	context->last_check = obs_get_video_frame_time();

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

static obs_properties_t *replay_filter_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *prop = obs_properties_add_int(
		props, SETTING_DURATION, obs_module_text("Duration"),
		SETTING_DURATION_MIN, SETTING_DURATION_MAX, 1000);
	obs_property_int_set_suffix(prop, "ms");
	obs_properties_add_float_slider(props, SETTING_AUDIO_THRESHOLD,
					obs_module_text("ThresholdDb"),
					SETTING_AUDIO_THRESHOLD_MIN,
					SETTING_AUDIO_THRESHOLD_MAX, 0.1);

	return props;
}

static void replay_filter_tick(void *data, float seconds){
	replay_filter_check(data);
}

struct obs_source_info replay_filter_audio_info = {
	.id = REPLAY_FILTER_AUDIO_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.create = replay_filter_create,
	.destroy = replay_filter_destroy,
	.update = replay_filter_update,
	.load = replay_filter_update,
	.video_tick = replay_filter_tick,
	.get_name = replay_filter_get_name,
	.get_properties = replay_filter_properties,
	.filter_remove = replay_filter_remove,
	.filter_audio = replay_filter_audio,
};

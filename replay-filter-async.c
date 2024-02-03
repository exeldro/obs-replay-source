#include <obs-module.h>
#include <util/threading.h>
#include "media-io/audio-math.h"
#include "replay.h"

static const char *replay_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ReplayFilterAsync");
}

static void replay_filter_update(void *data, obs_data_t *settings)
{
	struct replay_filter *filter = data;

	const uint64_t new_duration =
		(uint64_t)obs_data_get_int(settings, SETTING_DURATION) *
		MSEC_TO_NSEC;

	if (new_duration < filter->duration) {
		pthread_mutex_lock(&filter->mutex);
		free_video_data(filter);
		pthread_mutex_unlock(&filter->mutex);
	}
	filter->duration = new_duration;
	filter->internal_frames =
		obs_data_get_bool(settings, SETTING_INTERNAL_FRAMES);
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
	UNUSED_PARAMETER(parent);
	struct replay_filter *filter = data;
	pthread_mutex_lock(&filter->mutex);
	free_video_data(filter);
	free_audio_data(filter);
	pthread_mutex_unlock(&filter->mutex);
}

static inline uint64_t uint64_diff(uint64_t ts1, uint64_t ts2)
{
	return (ts1 < ts2) ? (ts2 - ts1) : (ts1 - ts2);
}

struct async_frame {
	struct obs_source_frame *frame;
	long unused_count;
	bool used;
};

static struct obs_source_frame *
replay_filter_video(void *data, struct obs_source_frame *frame)
{
	struct replay_filter *filter = data;
	struct obs_source_frame *output;

	uint64_t last_timestamp = 0;

	obs_source_t *target = filter->internal_frames
				       ? obs_filter_get_parent(filter->src)
				       : NULL;
	const uint64_t os_time = obs_get_video_frame_time();
	struct obs_source_frame *new_frame = NULL;

	pthread_mutex_lock(&filter->mutex);
	if (filter->video_frames.size) {
		circlebuf_peek_back(&filter->video_frames, &output,
				    sizeof(struct obs_source_frame *));
		last_timestamp = output->timestamp;
	}
	if (target) {
		if (filter->target_offset == 0) {
			if (obs_get_version() <
			    MAKE_SEMANTIC_VERSION(30, 0, 0)) {
				filter->target_offset = 2000;
			} else {
				filter->target_offset = 2008;
			}
		}
		struct darray *async_cache =
			(struct darray *)((uint8_t *)target +
					  filter->target_offset);
		pthread_mutex_t *async_mutex =
			(pthread_mutex_t *)((uint8_t *)target +
					    filter->target_offset +
					    sizeof(struct darray) * 2);
		pthread_mutex_lock(async_mutex);
		for (size_t i = 0; i < async_cache->num; i++) {
			struct obs_source_frame *extra_frame =
				((struct async_frame *)async_cache->array)[i]
					.frame;
			if (extra_frame->timestamp + filter->timing_adjust >
			    last_timestamp) {
				new_frame = obs_source_frame_create(
					extra_frame->format, extra_frame->width,
					extra_frame->height);
				new_frame->refs = 1;
				obs_source_frame_copy(new_frame, extra_frame);
				const uint64_t timestamp =
					extra_frame->timestamp;
				uint64_t adjusted_time =
					timestamp + filter->timing_adjust;
				if (filter->timing_adjust &&
				    uint64_diff(os_time, timestamp) <
					    MAX_TS_VAR) {
					adjusted_time = timestamp;
					filter->timing_adjust = 0;
				} else if (uint64_diff(os_time, adjusted_time) >
					   MAX_TS_VAR) {
					filter->timing_adjust =
						os_time - timestamp;
					adjusted_time = os_time;
				}
				new_frame->timestamp = adjusted_time;
				last_timestamp = adjusted_time;
				circlebuf_push_back(
					&filter->video_frames, &new_frame,
					sizeof(struct obs_source_frame *));
			}
		}
		pthread_mutex_unlock(async_mutex);
	}
	if (frame->timestamp + filter->timing_adjust > last_timestamp) {
		new_frame = obs_source_frame_create(frame->format, frame->width,
						    frame->height);
		new_frame->refs = 1;
		obs_source_frame_copy(new_frame, frame);
		const uint64_t timestamp = frame->timestamp;
		uint64_t adjusted_time = timestamp + filter->timing_adjust;
		if (filter->timing_adjust &&
		    uint64_diff(os_time, timestamp) < MAX_TS_VAR) {
			adjusted_time = timestamp;
			filter->timing_adjust = 0;
		} else if (uint64_diff(os_time, adjusted_time) > MAX_TS_VAR) {
			filter->timing_adjust = os_time - timestamp;
			adjusted_time = os_time;
		}
		new_frame->timestamp = adjusted_time;
		last_timestamp = adjusted_time;
		circlebuf_push_back(&filter->video_frames, &new_frame,
				    sizeof(struct obs_source_frame *));
	}
	if (!last_timestamp) {
		pthread_mutex_unlock(&filter->mutex);
		return frame;
	}
	circlebuf_peek_front(&filter->video_frames, &output,
			     sizeof(struct obs_source_frame *));

	uint64_t cur_duration = last_timestamp - output->timestamp;
	while (cur_duration > 0 && cur_duration > filter->duration) {

		circlebuf_pop_front(&filter->video_frames, NULL,
				    sizeof(struct obs_source_frame *));

		if (os_atomic_dec_long(&output->refs) <= 0) {
			obs_source_frame_destroy(output);
			output = NULL;
		}
		if (filter->video_frames.size) {
			circlebuf_peek_front(&filter->video_frames, &output,
					     sizeof(struct obs_source_frame *));
			cur_duration = last_timestamp - output->timestamp;
		} else {
			cur_duration = 0;
		}
	}
	pthread_mutex_unlock(&filter->mutex);
	return frame;
}

static obs_properties_t *replay_filter_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *prop = obs_properties_add_int(
		props, SETTING_DURATION, obs_module_text("Duration"),
		SETTING_DURATION_MIN, SETTING_DURATION_MAX, 1000);
	obs_property_int_set_suffix(prop, "ms");
	obs_properties_add_bool(props, SETTING_INTERNAL_FRAMES,
				obs_module_text("CaptureInternalFrames"));
	obs_properties_add_float_slider(props, SETTING_AUDIO_THRESHOLD,
					obs_module_text("ThresholdDb"),
					SETTING_AUDIO_THRESHOLD_MIN,
					SETTING_AUDIO_THRESHOLD_MAX, 0.1);

	return props;
}

static void replay_filter_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	replay_filter_check(data);
}

struct obs_source_info replay_filter_async_info = {
	.id = REPLAY_FILTER_ASYNC_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_ASYNC,
	.create = replay_filter_create,
	.destroy = replay_filter_destroy,
	.update = replay_filter_update,
	.load = replay_filter_update,
	.video_tick = replay_filter_tick,
	.get_name = replay_filter_get_name,
	.get_properties = replay_filter_properties,
	.filter_video = replay_filter_video,
	.filter_audio = replay_filter_audio,
	.filter_remove = replay_filter_remove,
};

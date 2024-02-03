#include "version.h"
#include <math.h>
#include "replay.h"

void free_audio_data(struct replay_filter *filter)
{
	while (filter->audio_frames.size) {
		struct obs_audio_data audio;

		circlebuf_pop_front(&filter->audio_frames, &audio,
				    sizeof(struct obs_audio_data));
		free_audio_packet(&audio);
	}
}

void free_video_data(struct replay_filter *filter)
{
	while (filter->video_frames.size) {
		struct obs_source_frame *frame;

		circlebuf_pop_front(&filter->video_frames, &frame,
				    sizeof(struct obs_source_frame *));

		if (os_atomic_dec_long(&frame->refs) <= 0) {
			obs_source_frame_destroy(frame);
			frame = NULL;
		}
	}
}
static inline uint64_t uint64_diff(uint64_t ts1, uint64_t ts2)
{
	return (ts1 < ts2) ? (ts2 - ts1) : (ts1 - ts2);
}

EXPORT uint64_t os_gettime_ns(void);

struct obs_audio_data *replay_filter_audio(void *data,
					   struct obs_audio_data *audio)
{
	struct replay_filter *filter = data;
	struct obs_audio_data cached = *audio;
	bool threshold_trigger = !filter->trigger_threshold;
	if (filter->oai.samples_per_sec == 0 ||
	    filter->oai.format != AUDIO_FORMAT_FLOAT_PLANAR) {
		struct obs_audio_info oai;
		obs_get_audio_info(&oai);
		filter->oai.format = AUDIO_FORMAT_FLOAT_PLANAR;
		filter->oai.speakers = oai.speakers;
		filter->oai.samples_per_sec = oai.samples_per_sec;
	}

	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		if (!audio->data[i])
			break;

		cached.data[i] =
			bmemdup(audio->data[i], audio->frames * sizeof(float));

		for (size_t j = 0; !threshold_trigger && j < audio->frames;
		     j++) {
			if (fabsf(((float *)audio->data[i])[j]) >
			    filter->threshold)
				threshold_trigger = true;
		}
	}
	if (filter->trigger_threshold && threshold_trigger) {
		filter->trigger_threshold(filter->threshold_data);
	}
	const uint64_t timestamp = cached.timestamp;
	uint64_t adjusted_time = timestamp + filter->timing_adjust;
	const uint64_t os_time = os_gettime_ns();
	if (filter->timing_adjust &&
	    uint64_diff(os_time, timestamp) < MAX_TS_VAR) {
		adjusted_time = timestamp;
		filter->timing_adjust = 0;
	} else if (uint64_diff(os_time, adjusted_time) > MAX_TS_VAR) {
		filter->timing_adjust = os_time - timestamp;
		adjusted_time = os_time;
	}
	cached.timestamp = adjusted_time;

	pthread_mutex_lock(&filter->mutex);

	circlebuf_push_back(&filter->audio_frames, &cached, sizeof(cached));

	circlebuf_peek_front(&filter->audio_frames, &cached, sizeof(cached));

	uint64_t cur_duration = adjusted_time - cached.timestamp;
	while (filter->audio_frames.size > sizeof(cached) &&
	       cur_duration >= filter->duration + MAX_TS_VAR) {

		circlebuf_pop_front(&filter->audio_frames, NULL,
				    sizeof(cached));

		free_audio_packet(&cached);
		circlebuf_peek_front(&filter->audio_frames, &cached,
				     sizeof(cached));
		cur_duration = adjusted_time - cached.timestamp;
	}
	pthread_mutex_unlock(&filter->mutex);
	return audio;
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("replay-source", "en-US")

extern struct obs_source_info replay_filter_info;
extern struct obs_source_info replay_filter_audio_info;
extern struct obs_source_info replay_filter_async_info;
extern struct obs_source_info replay_source_info;

#if defined(_WIN32)
extern void RegisterDShowReplaySource();
#endif

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Replay Source] loaded version %s", PROJECT_VERSION);
	obs_register_source(&replay_source_info);
	obs_register_source(&replay_filter_info);
	obs_register_source(&replay_filter_audio_info);
	obs_register_source(&replay_filter_async_info);
#if defined(_WIN32)
	RegisterDShowReplaySource();
#endif
	return true;
}

void free_audio_packet(struct obs_audio_data *audio)
{
	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		bfree(audio->data[i]);
	memset(audio, 0, sizeof(*audio));
}

const char *obs_module_name(void)
{
	return obs_module_text("ReplaySource");
}

const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

const char *obs_module_author(void)
{
	return "Exeldro";
}

void replay_filter_check(void *data)
{
	struct replay_filter *filter = data;
	if (filter->last_check &&
	    filter->last_check + 3 * SEC_TO_NSEC > obs_get_video_frame_time())
		return;
	filter->last_check = obs_get_video_frame_time();
	obs_source_t *s =
		obs_get_source_by_name(obs_source_get_name(filter->src));
	if (s) {
		if (!filter->trigger_threshold) {
			obs_data_t *settings = obs_source_get_settings(s);
			if (obs_data_get_bool(settings,
					      SETTING_SOUND_TRIGGER)) {
				filter->threshold_data = obs_obj_get_data(s);
				filter->trigger_threshold =
					replay_trigger_threshold;
			}
			obs_data_release(settings);
		}
		obs_source_release(s);
	} else {
		obs_source_filter_remove(obs_filter_get_parent(filter->src),
					 filter->src);
	}
}

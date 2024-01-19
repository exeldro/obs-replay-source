#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>
#include <media-io/video-scaler.h>
#include <obs-frontend-api.h>
#include <obs-scene.h>
#include "replay.h"
#include <inttypes.h>
#include <string.h>

#define VISIBILITY_ACTION_RESTART 0
#define VISIBILITY_ACTION_PAUSE 1
#define VISIBILITY_ACTION_CONTINUE 2
#define VISIBILITY_ACTION_NONE 3

#define END_ACTION_HIDE 0
#define END_ACTION_PAUSE 1
#define END_ACTION_LOOP 2
#define END_ACTION_REVERSE 3
#define END_ACTION_HIDE_ALL 4
#define END_ACTION_PAUSE_ALL 5
#define END_ACTION_LOOP_ALL 6
#define END_ACTION_REVERSE_ALL 7

enum saving_status {
	SAVING_STATUS_NONE = 0,
	SAVING_STATUS_STARTING = 1,
	SAVING_STATUS_STARTING2 = 2,
	SAVING_STATUS_SAVING = 3,
	SAVING_STATUS_STOPPING = 4
};

struct replay {
	struct obs_source_frame **video_frames;
	uint64_t video_frame_count;
	struct obs_audio_data *audio_frames;
	struct audio_convert_info oai;
	uint64_t audio_frame_count;
	uint64_t first_frame_timestamp;
	uint64_t last_frame_timestamp;
	uint64_t duration;
	int64_t trim_front;
	int64_t trim_end;
};

struct replay_source {
	obs_source_t *source;
	obs_source_t *source_filter;
	obs_source_t *source_audio_filter;
	char *source_name;
	char *source_audio_name;
	float speed_percent;
	bool backward;
	bool backward_start;
	int visibility_action;
	int end_action;
	char *next_scene_name;
	bool next_scene_disabled;
	char *load_switch_scene_name;
	obs_hotkey_id replay_hotkey;
	obs_hotkey_id next_hotkey;
	obs_hotkey_id previous_hotkey;
	obs_hotkey_id first_hotkey;
	obs_hotkey_id last_hotkey;
	obs_hotkey_id remove_hotkey;
	obs_hotkey_id clear_hotkey;
	obs_hotkey_id restart_hotkey;
	obs_hotkey_id pause_hotkey;
	obs_hotkey_id faster_hotkey;
	obs_hotkey_id slower_hotkey;
	obs_hotkey_id faster_by_5_hotkey;
	obs_hotkey_id slower_by_5_hotkey;
	obs_hotkey_id normal_or_faster_hotkey;
	obs_hotkey_id normal_or_slower_hotkey;
	obs_hotkey_id normal_speed_hotkey;
	obs_hotkey_id half_speed_hotkey;
	obs_hotkey_id double_speed_hotkey;
	obs_hotkey_id trim_front_hotkey;
	obs_hotkey_id trim_end_hotkey;
	obs_hotkey_id trim_reset_hotkey;
	obs_hotkey_id reverse_hotkey;
	obs_hotkey_id forward_hotkey;
	obs_hotkey_id backward_hotkey;
	obs_hotkey_id forward_or_faster_hotkey;
	obs_hotkey_id backward_or_faster_hotkey;
	obs_hotkey_id save_hotkey;
	obs_hotkey_id enable_hotkey;
	obs_hotkey_id disable_hotkey;
	obs_hotkey_id enable_next_scene_hotkey;
	obs_hotkey_id disable_next_scene_hotkey;
	obs_hotkey_id next_scene_current_hotkey;
	obs_hotkey_id next_scene_hotkey;
	obs_hotkey_id next_frame_hotkey;
	obs_hotkey_id prev_frame_hotkey;
	uint64_t start_timestamp;
	uint64_t previous_frame_timestamp;
	uint64_t pause_timestamp;
	int64_t start_delay;
	int64_t retrieve_delay;
	uint64_t retrieve_timestamp;
	uint64_t threshold_timestamp;
	struct obs_source_audio audio;

	bool disabled;
	bool play;
	bool restart;
	bool active;
	bool end;
	bool stepped;
	enum saving_status saving_status;

	int replay_position;
	int replay_max;
	struct circlebuf replays;
	struct replay current_replay;
	struct replay saving_replay;

	uint64_t video_frame_position;
	uint64_t video_save_position;

	/* stores the audio data */
	uint64_t audio_frame_position;
	struct obs_audio_data audio_output;

	pthread_mutex_t video_mutex;
	pthread_mutex_t audio_mutex;
	pthread_mutex_t replay_mutex;

	uint32_t known_width;
	uint32_t known_height;

	video_t *video_output;
	obs_output_t *fileOutput;
	obs_encoder_t *h264Recording;
	audio_t *audio_t;
	video_scaler_t *scaler;
	bool lossless;
	char *file_format;
	char *directory;
	uint64_t start_save_timestamp;
	obs_encoder_t *aac;
	char *progress_source_name;
	char *text_source_name;
	char *text_format;
	bool sound_trigger;
	bool filter_loaded;
	bool free_after_save;
};

static void replace_text(struct dstr *str, size_t pos, size_t len,
			 const char *new_text)
{
	struct dstr front = {0};
	struct dstr back = {0};

	dstr_left(&front, str, pos);
	dstr_right(&back, str, pos + len);
	dstr_copy_dstr(str, &front);
	dstr_cat(str, new_text);
	dstr_cat_dstr(str, &back);
	dstr_free(&front);
	dstr_free(&back);
}

static void replay_update_text(struct replay_source *c)
{
	if (!c->text_source_name || !c->text_format)
		return;
	obs_source_t *s = obs_get_source_by_name(c->text_source_name);
	if (!s)
		return;

	struct dstr sf;
	size_t pos = 0;
	struct dstr buffer;
	dstr_init(&buffer);
	dstr_init_copy(&sf, c->text_format);
	while (pos < sf.len) {
		//duration, speed, index, count
		const char *cmp = sf.array + pos;
		if (astrcmp_n(cmp, "%SPEED%", 7) == 0) {
			dstr_printf(&buffer, "%.1f",
				    c->speed_percent *
					    (c->backward ? -1.0f : 1.0f));
			dstr_cat_ch(&buffer, '%');
			replace_text(&sf, pos, 7, buffer.array);
			pos += buffer.len;
		} else if (astrcmp_n(cmp, "%PROGRESS%", 10) == 0) {
			if (c->current_replay.video_frame_count &&
			    c->video_frame_position <
				    c->current_replay.video_frame_count) {
				dstr_printf(&buffer, "%.1f",
					    c->video_frame_position * 100.0 /
						    c->current_replay
							    .video_frame_count);
				dstr_cat_ch(&buffer, '%');
			} else {
				dstr_copy(&buffer, "");
			}
			replace_text(&sf, pos, 10, buffer.array);
			pos += buffer.len;
		} else if (astrcmp_n(cmp, "%COUNT%", 7) == 0) {
			dstr_printf(&buffer, "%d",
				    (int)(c->replays.size /
					  sizeof c->current_replay));
			replace_text(&sf, pos, 7, buffer.array);
			pos += buffer.len;
		} else if (astrcmp_n(cmp, "%INDEX%", 7) == 0) {
			if (c->replays.size) {
				dstr_printf(&buffer, "%d",
					    c->replay_position + 1);
			} else {
				dstr_copy(&buffer, "0");
			}
			replace_text(&sf, pos, 7, buffer.array);
			pos += buffer.len;
		} else if (astrcmp_n(cmp, "%DURATION%", 10) == 0) {
			if (c->replays.size) {
				dstr_printf(&buffer, "%.2f",
					    (double)c->current_replay.duration /
						    (double)1000000000.0);
			} else {
				dstr_copy(&buffer, "");
			}
			replace_text(&sf, pos, 10, buffer.array);
			pos += buffer.len;
		} else if (astrcmp_n(cmp, "%TIME%", 6) == 0) {
			if (c->replays.size && c->start_timestamp) {
				int64_t time = 0;
				if (c->pause_timestamp > c->start_timestamp) {
					time = c->pause_timestamp -
					       c->start_timestamp;
				} else {
					time = obs_get_video_frame_time() -
					       c->start_timestamp;
				}
				if (c->speed_percent != 100.0f) {
					time = (int64_t)((float)time *
							 c->speed_percent /
							 100.0f);
				}
				dstr_printf(&buffer, "%.2f",
					    (double)time /
						    (double)1000000000.0);
			} else {
				dstr_copy(&buffer, "");
			}
			replace_text(&sf, pos, 6, buffer.array);
			pos += buffer.len;
		} else if (astrcmp_n(cmp, "%FPS%", 5) == 0) {
			if (c->current_replay.video_frame_count &&
			    c->current_replay.duration) {
				dstr_printf(&buffer, "%d",
					    (int)(c->current_replay
							  .video_frame_count *
						  1000000000U /
						  c->current_replay.duration));
			} else {
				dstr_copy(&buffer, "0");
			}
			replace_text(&sf, pos, 5, buffer.array);
			pos += buffer.len;
		} else {
			pos++;
		}
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", sf.array);
	obs_source_update(s, settings);
	obs_data_release(settings);
	dstr_free(&sf);
	dstr_free(&buffer);
	obs_source_release(s);
}

struct siu {
	uint32_t crop_width;
	obs_source_t *source;
};
static bool EnumSceneItem(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	struct siu *siu = data;
	if (item->source == siu->source) {
		struct obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(item, &crop);
		crop.left = 0;
		crop.right = siu->crop_width;
		obs_sceneitem_set_crop(item, &crop);
	} else if (obs_sceneitem_is_group(item)) {
		obs_scene_enum_items(obs_sceneitem_group_get_scene(item),
				     EnumSceneItem, data);
	}
	return true;
}

static bool EnumScenesItems(void *data, obs_source_t *source)
{
	obs_scene_t *scene = obs_scene_from_source(source);
	obs_scene_enum_items(scene, EnumSceneItem, data);
	return true;
}

static void replay_update_progress_crop(struct replay_source *context,
					uint64_t t)
{
	if (!context->progress_source_name)
		return;
	obs_source_t *s = obs_get_source_by_name(context->progress_source_name);
	if (!s)
		return;
	const uint32_t width = obs_source_get_base_width(s);
	if (width) {
		struct siu siu;
		siu.source = s;
		if (t && context->current_replay.last_frame_timestamp &&
		    context->current_replay.duration) {
			siu.crop_width =
				(uint32_t)((context->current_replay
						    .last_frame_timestamp -
					    t) *
					   width /
					   context->current_replay.duration);
		} else {
			siu.crop_width = width;
		}
		obs_enum_scenes(EnumScenesItems, &siu);
	}
	obs_source_release(s);
}

static const char *replay_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ReplaySource");
}

static void EnumFilter(obs_source_t *source, obs_source_t *filter, void *data)
{
	UNUSED_PARAMETER(source);
	struct replay_source *c = data;
	const char *filterName = obs_source_get_name(filter);
	const char *sourceName = obs_source_get_name(c->source);
	const char *id = obs_source_get_unversioned_id(filter);
	if ((strcmp(REPLAY_FILTER_ASYNC_ID, id) == 0 ||
	     strcmp(REPLAY_FILTER_ID, id) == 0) &&
	    strcmp(filterName, sourceName) == 0)
		c->source_filter = filter;
}
static void EnumAudioFilter(obs_source_t *source, obs_source_t *filter,
			    void *data)
{
	UNUSED_PARAMETER(source);
	struct replay_source *c = data;
	const char *filterName = obs_source_get_name(filter);
	const char *sourceName = obs_source_get_name(c->source);
	const char *id = obs_source_get_unversioned_id(filter);
	if (strcmp(REPLAY_FILTER_AUDIO_ID, id) == 0 &&
	    strcmp(filterName, sourceName) == 0)
		c->source_audio_filter = filter;
}
static void EnumAudioVideoFilter(obs_source_t *source, obs_source_t *filter,
				 void *data)
{
	UNUSED_PARAMETER(source);
	struct replay_source *c = data;
	const char *filterName = obs_source_get_name(filter);
	const char *sourceName = obs_source_get_name(c->source);
	const char *id = obs_source_get_unversioned_id(filter);
	if ((strcmp(REPLAY_FILTER_AUDIO_ID, id) == 0 ||
	     strcmp(REPLAY_FILTER_ASYNC_ID, id) == 0 ||
	     strcmp(REPLAY_FILTER_ID, id) == 0) &&
	    strcmp(filterName, sourceName) == 0)
		c->source_audio_filter = filter;
}

static inline void obs_source_signal(struct obs_source *source,
				     const char *signal_source)
{
	struct calldata data;
	uint8_t stack[128];

	calldata_init_fixed(&data, stack, sizeof(stack));
	calldata_set_ptr(&data, "source", source);
	if (signal_source)
		signal_handler_signal(obs_source_get_signal_handler(source),
				      signal_source, &data);
}

static void replay_reverse_hotkey(void *data, obs_hotkey_id id,
				  obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (pressed) {
		const int64_t time = obs_get_video_frame_time();
		if (c->pause_timestamp) {
			c->start_timestamp += time - c->pause_timestamp;
			c->pause_timestamp = 0;
		}
		c->backward = !c->backward;
		if (!c->play) {
			c->play = true;
			obs_source_signal(c->source, "media_play");
		}
		if (c->end) {
			c->end = false;
			if (c->backward &&
			    c->current_replay.video_frame_count) {
				c->video_frame_position =
					c->current_replay.video_frame_count - 1;
			} else {
				c->video_frame_position = 0;
			}
		}
		const int64_t duration =
			(int64_t)(((int64_t)c->current_replay
					   .last_frame_timestamp -
				   (int64_t)c->current_replay
					   .first_frame_timestamp) *
				  100.0 / c->speed_percent);
		int64_t play_duration = time - c->start_timestamp;
		if (play_duration > duration) {
			play_duration = duration;
		}
		c->start_timestamp = time - duration + play_duration;
	}
}

static void replay_forward_hotkey(void *data, obs_hotkey_id id,
				  obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;
	const int64_t time = obs_get_video_frame_time();
	if (c->pause_timestamp) {
		c->start_timestamp += time - c->pause_timestamp;
		c->pause_timestamp = 0;
	}
	if (!c->play) {
		c->play = true;
		obs_source_signal(c->source, "media_play");
	}
	if (c->end) {
		c->end = false;
		c->video_frame_position = 0;
		c->start_timestamp = obs_get_video_frame_time();
		c->backward = false;
	} else if (c->backward) {
		c->backward = false;

		const int64_t duration =
			(int64_t)(((int64_t)c->current_replay
					   .last_frame_timestamp -
				   (int64_t)c->current_replay
					   .first_frame_timestamp) *
				  100.0 / c->speed_percent);
		int64_t play_duration = time - c->start_timestamp;
		if (play_duration > duration) {
			play_duration = duration;
		}
		c->start_timestamp = time - duration + play_duration;
	}
}

static void replay_backward_hotkey(void *data, obs_hotkey_id id,
				   obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;
	const int64_t time = obs_get_video_frame_time();
	if (c->pause_timestamp) {
		c->start_timestamp += time - c->pause_timestamp;
		c->pause_timestamp = 0;
	}
	if (!c->play) {
		c->play = true;
		obs_source_signal(c->source, "media_play");
	}
	if (c->end || c->video_frame_position == 0) {
		c->end = false;
		if (c->current_replay.video_frame_count)
			c->video_frame_position =
				c->current_replay.video_frame_count - 1;
		c->start_timestamp = obs_get_video_frame_time();
		c->backward = true;
	} else if (!c->backward) {
		c->backward = true;

		const int64_t duration =
			(int64_t)(((int64_t)c->current_replay
					   .last_frame_timestamp -
				   (int64_t)c->current_replay
					   .first_frame_timestamp) *
				  100.0 / c->speed_percent);
		int64_t play_duration = time - c->start_timestamp;
		if (play_duration > duration) {
			play_duration = duration;
		}
		c->start_timestamp = time - duration + play_duration;
	}
}

static void replay_update_position(struct replay_source *context, bool lock)
{
	if (lock)
		pthread_mutex_lock(&context->video_mutex);
	pthread_mutex_lock(&context->audio_mutex);
	const int replay_count =
		(int)(context->replays.size / sizeof context->current_replay);
	if (replay_count == 0) {
		context->current_replay.video_frame_count = 0;
		context->current_replay.video_frames = NULL;
		context->current_replay.audio_frame_count = 0;
		context->current_replay.audio_frames = NULL;
		context->replay_position = 0;
		struct obs_source_frame *f =
			obs_source_frame_create(VIDEO_FORMAT_NONE, 0, 0);
		obs_source_output_video(context->source, f);
		obs_source_frame_destroy(f);
		pthread_mutex_unlock(&context->audio_mutex);
		if (lock)
			pthread_mutex_unlock(&context->video_mutex);
		blog(LOG_INFO, "[replay_source: '%s'] No replay active",
		     obs_source_get_name(context->source));
		return;
	}
	if (context->replay_position >= replay_count) {
		context->replay_position = replay_count - 1;
	} else if (context->replay_position < 0) {
		context->replay_position = 0;
	}
	memcpy(&context->current_replay,
	       circlebuf_data(&context->replays,
			      context->replay_position *
				      sizeof context->current_replay),
	       sizeof context->current_replay);
	context->video_frame_position = 0;
	context->audio_frame_position = 0;
	context->start_timestamp = obs_get_video_frame_time();
	context->backward = context->backward_start;
	if (!context->backward && context->current_replay.trim_front != 0) {
		context->start_timestamp -=
			(uint64_t)(context->current_replay.trim_front *
					100.0 / context->speed_percent);
	} else if (context->backward && context->current_replay.trim_end != 0) {
		context->start_timestamp -=
			(uint64_t)(context->current_replay.trim_end *
					100.0 / context->speed_percent);
	}
	context->pause_timestamp = 0;
	if (context->backward && context->current_replay.video_frame_count) {
		context->video_frame_position =
			context->current_replay.video_frame_count - 1;
	}
	if (context->active ||
	    context->visibility_action == VISIBILITY_ACTION_CONTINUE) {
		if (!context->play) {
			context->play = true;
			obs_source_signal(context->source, "media_play");
		}
	} else {
		if (context->play) {
			context->play = false;
			obs_source_signal(context->source, "media_pause");
		}
		context->pause_timestamp = obs_get_video_frame_time();
	}

	pthread_mutex_unlock(&context->audio_mutex);
	if (lock)
		pthread_mutex_unlock(&context->video_mutex);

	replay_update_text(context);
}

static void replay_free_replay(struct replay *replay,
			       struct replay_source *context)
{
	if (replay == &context->saving_replay) {
		context->free_after_save = false;
	} else if (replay->video_frames ==
			   context->saving_replay.video_frames &&
		   context->saving_status != SAVING_STATUS_NONE) {
		context->free_after_save = true;
		return;
	}
	for (uint64_t i = 0; i < replay->video_frame_count; i++) {
		struct obs_source_frame *frame = replay->video_frames[i];
		if (frame && os_atomic_dec_long(&frame->refs) <= 0) {
			obs_source_frame_destroy(frame);
			replay->video_frames[i] = NULL;
		}
	}
	replay->video_frame_count = 0;
	if (replay->video_frames) {
		bfree(replay->video_frames);
		replay->video_frames = NULL;
	}

	for (uint64_t i = 0; i < replay->audio_frame_count; i++) {
		free_audio_packet(&replay->audio_frames[i]);
	}
	replay->audio_frame_count = 0;
	if (replay->audio_frames) {
		bfree(replay->audio_frames);
		replay->audio_frames = NULL;
	}
}
static void replay_purge_replays(struct replay_source *context)
{
	if ((int)(context->replays.size / sizeof context->current_replay) >
	    context->replay_max) {
		pthread_mutex_lock(&context->replay_mutex);
		const int replays_to_delete =
			(int)(context->replays.size /
				      sizeof context->current_replay -
			      context->replay_max);
		if (replays_to_delete > context->replay_position) {
			context->replay_position = replays_to_delete;
			replay_update_position(context, true);
		}
		while ((int)(context->replays.size /
			     sizeof context->current_replay) >
		       context->replay_max) {
			struct replay old_replay;
			circlebuf_pop_front(&context->replays, &old_replay,
					    sizeof context->current_replay);
			replay_free_replay(&old_replay, context);
			context->replay_position--;
		}
		if (context->replay_max > 1)
			blog(LOG_INFO,
			     "[replay_source: '%s'] switched to replay %i/%i",
			     obs_source_get_name(context->source),
			     context->replay_position + 1,
			     (int)(context->replays.size /
				   sizeof context->current_replay));
		pthread_mutex_unlock(&context->replay_mutex);
	}
}
static void replay_retrieve(struct replay_source *context);

void replay_trigger_threshold(void *data)
{
	struct replay_source *context = data;
	const uint64_t os_time = obs_get_video_frame_time();
	uint64_t duration = context->current_replay.duration;
	if (context->speed_percent < 100.f)
		duration =
			(uint64_t)(duration * 100.0 / context->speed_percent);
	if (context->threshold_timestamp &&
	    context->threshold_timestamp + context->retrieve_delay + duration >
		    os_time)
		return;

	context->threshold_timestamp = os_time;
	blog(LOG_INFO, "[replay_source: '%s'] audio triggered",
	     obs_source_get_name(context->source));
	if (context->retrieve_delay > 0) {
		context->retrieve_timestamp =
			obs_get_video_frame_time() + context->retrieve_delay;
	} else {
		replay_retrieve(context);
	}
}

static void replay_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_DURATION, 5000);
	obs_data_set_default_int(settings, SETTING_REPLAYS, 1);
	obs_data_set_default_int(settings, SETTING_SPEED, 100);
	obs_data_set_default_int(settings, SETTING_VISIBILITY_ACTION,
				 VISIBILITY_ACTION_CONTINUE);
	obs_data_set_default_int(settings, SETTING_START_DELAY, 0);
	obs_data_set_default_int(settings, SETTING_END_ACTION, END_ACTION_LOOP);
	obs_data_set_default_bool(settings, SETTING_BACKWARD, false);
	obs_data_set_default_string(settings, SETTING_FILE_FORMAT,
				    "%CCYY-%MM-%DD %hh.%mm.%ss");
	obs_data_set_default_bool(settings, SETTING_LOSSLESS, false);
}

static void replay_source_show(void *data)
{
	struct replay_source *context = data;
	UNUSED_PARAMETER(context);
}

static void replay_source_hide(void *data)
{
	struct replay_source *context = data;
	UNUSED_PARAMETER(context);
}

static void replay_source_active(void *data)
{
	struct replay_source *context = data;
	if (context->visibility_action == VISIBILITY_ACTION_PAUSE ||
	    context->visibility_action == VISIBILITY_ACTION_CONTINUE) {
		if (!context->play && !context->end) {
			context->play = true;
			if (context->pause_timestamp) {
				context->start_timestamp +=
					obs_get_video_frame_time() -
					context->pause_timestamp;
				context->pause_timestamp = 0;
			}
			obs_source_signal(context->source, "media_play");
		}
	} else if (context->visibility_action == VISIBILITY_ACTION_RESTART) {
		context->play = true;
		context->restart = true;
		obs_source_signal(context->source, "media_restart");
	}
	context->active = true;
}

static void replay_source_deactive(void *data)
{
	struct replay_source *context = data;
	if (context->visibility_action == VISIBILITY_ACTION_PAUSE) {
		if (context->play) {
			context->play = false;
			context->pause_timestamp = obs_get_video_frame_time();
			obs_source_signal(context->source, "media_pause");
		}
	} else if (context->visibility_action == VISIBILITY_ACTION_RESTART) {
		if (context->play) {
			context->play = false;
			obs_source_signal(context->source, "media_pause");
		}
		context->restart = true;
	}
	context->active = false;
}

void replay_restart(void *data)
{
	struct replay_source *c = data;
	c->restart = true;
	c->play = true;
	obs_source_signal(c->source, "media_restart");
}

void replay_stop(void *data)
{
	struct replay_source *c = data;
	c->play = false;
	c->restart = true;
	obs_source_media_ended(c->source);
}

void replay_next(void *data)
{
	struct replay_source *context = data;
	const int replay_count =
		(int)(context->replays.size / sizeof context->current_replay);
	if (replay_count == 0)
		return;

	if (context->replay_position + 1 >= replay_count) {
		context->replay_position = replay_count - 1;
	} else {
		context->replay_position++;
	}
	replay_update_position(context, true);
	blog(LOG_INFO, "[replay_source: '%s'] next switched to replay %i/%i",
	     obs_source_get_name(context->source), context->replay_position + 1,
	     replay_count);
}

void replay_previous(void *data)
{
	struct replay_source *context = data;
	if (context->replay_position <= 0) {
		context->replay_position = 0;
	} else {
		context->replay_position--;
	}
	replay_update_position(context, true);
	blog(LOG_INFO,
	     "[replay_source: '%s'] previous hotkey switched to replay %i/%i",
	     obs_source_get_name(context->source), context->replay_position + 1,
	     (int)(context->replays.size / sizeof context->current_replay));
}

static void replay_play_pause(void *data, bool pause)
{
	struct replay_source *c = data;
	if (pause) {
		if (c->play) {
			c->play = false;
			c->pause_timestamp = obs_get_video_frame_time();
			obs_source_signal(c->source, "media_pause");
		} else {
			c->play = true;
			if (c->pause_timestamp) {
				c->start_timestamp +=
					obs_get_video_frame_time() -
					c->pause_timestamp;
				c->pause_timestamp = 0;
			}
			obs_source_signal(c->source, "media_play");
		}
	} else {
		const int64_t time = obs_get_video_frame_time();
		if (c->pause_timestamp) {
			c->start_timestamp += time - c->pause_timestamp;
			c->pause_timestamp = 0;
		}
		c->play = true;
		if (c->end || (c->video_frame_position == 0 && c->backward)) {
			c->end = false;
			if (c->backward) {
				if (c->current_replay.video_frame_count)
					c->video_frame_position =
						c->current_replay
							.video_frame_count -
						1;
			} else {
				c->video_frame_position = 0;
			}
			c->start_timestamp = obs_get_video_frame_time();
		}
		obs_source_signal(c->source, "media_play");
	}
}

static void replay_restart_hotkey(void *data, obs_hotkey_id id,
				  obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed)
		replay_restart(data);
}

static void replay_pause_hotkey(void *data, obs_hotkey_id id,
				obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed)
		replay_play_pause(data, true);
}

static void InitFileOutputLossless(struct replay_source *context)
{
	context->fileOutput = obs_output_create(
		"ffmpeg_output", "replay_ffmpeg_output", NULL, NULL);
	obs_data_t *foSettings = obs_data_create();
	obs_data_set_string(foSettings, "format_name", "avi");
	obs_data_set_string(foSettings, "video_encoder", "utvideo");
	obs_data_set_string(foSettings, "audio_encoder", "pcm_s16le");
	obs_output_update(context->fileOutput, foSettings);
	obs_data_release(foSettings);
	obs_output_set_media(context->fileOutput, context->video_output,
			     context->audio_t);
}

static void InitFileOutput(struct replay_source *context)
{
	context->fileOutput = obs_output_create(
		"ffmpeg_muxer", "replay_ffmpeg_output", NULL, NULL);
	context->h264Recording = obs_video_encoder_create(
		"obs_x264", "replay_h264_recording", NULL, NULL);
	obs_data_t *xsettings = obs_data_create();
	obs_data_set_int(xsettings, "crf", 23);
	obs_data_set_bool(xsettings, "use_bufsize", true);
	obs_data_set_string(xsettings, "rate_control", "CRF");
	obs_data_set_string(xsettings, "profile", "high");
	obs_data_set_string(xsettings, "preset", "veryfast");
	obs_encoder_update(context->h264Recording, xsettings);
	obs_data_release(xsettings);
	obs_encoder_set_video(context->h264Recording, context->video_output);
	obs_output_set_video_encoder(context->fileOutput,
				     context->h264Recording);
	context->aac =
		obs_audio_encoder_create("ffmpeg_aac", "aac", NULL, 0, NULL);
	obs_encoder_set_audio(context->aac, context->audio_t);
	obs_output_set_audio_encoder(context->fileOutput, context->aac, 0);
}

static inline size_t convert_time_to_frames(size_t sample_rate, uint64_t t)
{
	return (size_t)(t * (uint64_t)sample_rate / 1000000000ULL);
}

bool audio_input_callback(void *param, uint64_t start_ts_in, uint64_t end_ts_in,
			  uint64_t *out_ts, uint32_t mixers,
			  struct audio_output_data *mixes)
{
	UNUSED_PARAMETER(mixers);
	struct replay_source *context = param;

	*out_ts = start_ts_in;

	if (!context->audio_t)
		return true;

	if (!context->start_save_timestamp ||
	    start_ts_in < context->start_save_timestamp)
		return true;

	uint64_t end_timestamp = context->start_save_timestamp +
				 context->saving_replay.last_frame_timestamp -
				 context->saving_replay.first_frame_timestamp;
	if (start_ts_in > end_timestamp)
		return true;

	size_t channels = audio_output_get_channels(context->audio_t);
	size_t sample_rate = audio_output_get_sample_rate(context->audio_t);

	pthread_mutex_lock(&context->audio_mutex);

	uint64_t i = 0;
	uint64_t duration_start = start_ts_in - context->start_save_timestamp;
	uint64_t duration_end = end_ts_in - context->start_save_timestamp;
	while (i < context->saving_replay.audio_frame_count &&
	       context->saving_replay.audio_frames[i].timestamp <
		       context->saving_replay.first_frame_timestamp) {
		i++;
	}
	if (i == context->saving_replay.audio_frame_count) {
		pthread_mutex_unlock(&context->audio_mutex);
		return true;
	}

	while (i < context->saving_replay.audio_frame_count &&
	       context->saving_replay.audio_frames[i].timestamp -
			       context->saving_replay.first_frame_timestamp <
		       duration_start) {
		i++;
	}
	if (i == context->saving_replay.audio_frame_count) {
		pthread_mutex_unlock(&context->audio_mutex);
		return true;
	}
	if (i)
		i--;

	while (i < context->saving_replay.audio_frame_count &&
	       duration_end >=
		       context->saving_replay.audio_frames[i].timestamp -
			       context->saving_replay.first_frame_timestamp) {
		size_t total_floats = AUDIO_OUTPUT_FRAMES;
		size_t start_point = 0;
		size_t start_point2 = 0;
		if (context->saving_replay.audio_frames[i].timestamp -
			    context->saving_replay.first_frame_timestamp >
		    duration_start) {
			start_point = convert_time_to_frames(
				sample_rate,
				context->saving_replay.audio_frames[i].timestamp -
					context->saving_replay
						.first_frame_timestamp -
					duration_start);
			if (start_point >= AUDIO_OUTPUT_FRAMES) {
				pthread_mutex_unlock(&context->audio_mutex);
				return true;
			}

			total_floats -= start_point;
		} else if (context->saving_replay.audio_frames[i].timestamp -
				   context->saving_replay.first_frame_timestamp <
			   duration_start) {
			start_point2 = convert_time_to_frames(
				sample_rate,
				duration_start -
					(context->saving_replay.audio_frames[i]
						 .timestamp -
					 context->saving_replay
						 .first_frame_timestamp));
			if (start_point2 >=
			    context->saving_replay.audio_frames[i].frames) {
				i++;
				continue;
			}
		}
		if (context->saving_replay.audio_frames[i].frames -
			    start_point2 <
		    total_floats) {
			total_floats =
				context->saving_replay.audio_frames[i].frames -
				start_point2;
		}

		for (size_t mix_idx = 0; mix_idx < MAX_AUDIO_MIXES; mix_idx++) {
			for (size_t ch = 0; ch < channels; ch++) {
				register float *mix = mixes[mix_idx].data[ch];
				register float *aud =
					(float *)context->saving_replay
						.audio_frames[i]
						.data[ch];
				register float *end;

				if (!aud)
					continue;

				aud += start_point2;
				mix += start_point;
				end = aud + total_floats;

				while (aud < end)
					*(mix++) += *(aud++);
			}
		}
		i++;
	}
	pthread_mutex_unlock(&context->audio_mutex);

	return true;
}

void replay_save(struct replay_source *context)
{
	if (context->current_replay.video_frame_count == 0) {
		context->saving_status = SAVING_STATUS_NONE;
		return;
	}
	if (context->saving_status != SAVING_STATUS_NONE &&
	    context->saving_status != SAVING_STATUS_STARTING)
		return;

	pthread_mutex_lock(&context->video_mutex);

	memcpy(&context->saving_replay, &context->current_replay,
	       sizeof context->current_replay);

	const uint32_t width = context->saving_replay.video_frames[0]->width;
	const uint32_t height = context->saving_replay.video_frames[0]->height;
	if (context->known_width != width || context->known_height != height) {
		video_t *t = obs_get_video();
		const struct video_output_info *ovi = video_output_get_info(t);

		struct video_output_info vi = {0};
		vi.format = ovi->format;
		vi.width = width;
		vi.height = height;
		vi.fps_den = ovi->fps_den;
		vi.fps_num = ovi->fps_num;
		vi.cache_size = 16;
		vi.colorspace = ovi->colorspace;
		vi.range = ovi->range;
		vi.name = "ReplayOutput";

		video_output_close(context->video_output);
		const int r = video_output_open(&context->video_output, &vi);
		if (r != VIDEO_OUTPUT_SUCCESS) {
			context->saving_status = SAVING_STATUS_NONE;
			pthread_mutex_unlock(&context->video_mutex);
			if (context->free_after_save) {
				replay_free_replay(&context->saving_replay,
						   context);
			}
			return;
		}

		context->known_width = width;
		context->known_height = height;

		if (context->scaler) {
			video_scaler_destroy(context->scaler);
			context->scaler = NULL;
		}

		struct video_scale_info ssi;
		ssi.format = context->saving_replay.video_frames[0]->format;
		ssi.colorspace = ovi->colorspace;
		ssi.range = ovi->range;
		ssi.width = context->saving_replay.video_frames[0]->width;
		ssi.height = context->saving_replay.video_frames[0]->height;
		struct video_scale_info dsi;
		dsi.format = vi.format;
		dsi.colorspace = ovi->colorspace;
		dsi.range = ovi->range;
		dsi.height = height;
		dsi.width = width;
		video_scaler_create(&context->scaler, &dsi, &ssi,
				    VIDEO_SCALE_DEFAULT);
	}
	if (!context->audio_t) {
		struct audio_output_info oi;
		oi.name = "ReplayAudio";
		oi.speakers = context->saving_replay.oai.speakers;
		oi.samples_per_sec = context->saving_replay.oai.samples_per_sec;
		oi.format = context->saving_replay.oai.format;
		oi.input_param = context;
		oi.input_callback = audio_input_callback;
		const int r = audio_output_open(&context->audio_t, &oi);
		if (r != AUDIO_OUTPUT_SUCCESS) {
			context->saving_status = SAVING_STATUS_NONE;
			pthread_mutex_unlock(&context->video_mutex);
			if (context->free_after_save) {
				replay_free_replay(&context->saving_replay,
						   context);
			}
			return;
		}
	}
	if (!context->fileOutput) {
		if (context->lossless) {
			InitFileOutputLossless(context);
		} else {
			InitFileOutput(context);
		}
	}

	char *filename = os_generate_formatted_filename(
		context->lossless ? "avi" : "flv", true, context->file_format);
	struct dstr path = {NULL, 0, 0};
	dstr_copy(&path, context->directory);
	dstr_replace(&path, "\\", "/");
	if (dstr_end(&path) != '/')
		dstr_cat_ch(&path, '/');
	dstr_cat(&path, filename);
	blog(LOG_INFO, "[replay_source: '%s'] start saving '%s'",
	     obs_source_get_name(context->source), filename);
	bfree(filename);
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", path.array);
	obs_data_set_string(settings, "url", path.array);

	obs_output_update(context->fileOutput, settings);
	dstr_free(&path);
	obs_data_release(settings);

	context->video_save_position = 0;
	context->start_save_timestamp = obs_get_video_frame_time();

	struct obs_source_frame *frame = context->saving_replay.video_frames[0];
	if (context->saving_replay.trim_front) {
		while (frame->timestamp <
		       context->saving_replay.first_frame_timestamp +
			       context->saving_replay.trim_front) {
			context->video_save_position++;
			if (context->video_save_position >=
			    context->saving_replay.video_frame_count) {
				context->saving_status = SAVING_STATUS_STOPPING;
				pthread_mutex_unlock(&context->video_mutex);
				return;
			}
			frame = context->saving_replay.video_frames
					[context->video_save_position];
		}
	}
	struct video_frame output_frame;
	if (video_output_lock_frame(context->video_output, &output_frame, 1,
				    context->start_save_timestamp)) {
		video_scaler_scale(context->scaler, output_frame.data,
				   output_frame.linesize,
				   (const uint8_t *const *)frame->data,
				   frame->linesize);
		video_output_unlock_frame(context->video_output);
	}
	pthread_mutex_unlock(&context->video_mutex);
	if (!obs_output_start(context->fileOutput)) {
		const char *error =
			obs_output_get_last_error(context->fileOutput);
		if (error)
			blog(LOG_WARNING,
			     "[replay_source: '%s'] error output start: %s",
			     obs_source_get_name(context->source), error);
		else
			blog(LOG_WARNING,
			     "[replay_source: '%s'] error output start",
			     obs_source_get_name(context->source));

		context->saving_status = SAVING_STATUS_NONE;
		if (context->free_after_save) {
			replay_free_replay(&context->saving_replay, context);
		}
		return;
	}
	context->saving_status = SAVING_STATUS_STARTING2;
}

static void *update_scene_thread(void *data)
{
	obs_source_t *scene = data;
	obs_frontend_set_current_scene(scene);
	return NULL;
}

static void replay_retrieve(struct replay_source *context)
{
	obs_source_t *s = obs_get_source_by_name(context->source_name);
	context->source_filter = NULL;
	bool dswow_video = false;
	bool dswow_audio = false;
	if (s) {
		if (strcmp(obs_source_get_unversioned_id(s),
			   "dshow_input_replay") == 0) {
			dswow_video = true;
		} else {
			obs_source_enum_filters(s, EnumFilter, context);
		}
	}
	obs_source_t *as = obs_get_source_by_name(context->source_audio_name);
	context->source_audio_filter = NULL;
	if (as) {
		if (strcmp(obs_source_get_unversioned_id(as),
			   "dshow_input_replay") == 0) {
			dswow_audio = true;
		} else {
			obs_source_enum_filters(as, EnumAudioVideoFilter,
						context);
		}
	}

	struct replay_filter *vf =
		context->source_filter
			? obs_obj_get_data(context->source_filter)
			: NULL;
	if (dswow_video)
		vf = obs_obj_get_data(s);
	struct replay_filter *af =
		context->source_audio_filter
			? obs_obj_get_data(context->source_audio_filter)
			: vf;
	if (dswow_audio)
		af = obs_obj_get_data(as);
	if (vf && vf->video_frames.size == 0)
		vf = NULL;
	if (af && af->audio_frames.size == 0)
		af = NULL;

	if (!vf && !af) {
		if (s)
			obs_source_release(s);
		if (as)
			obs_source_release(as);
		return;
	}

	struct replay new_replay;
	new_replay.last_frame_timestamp = 0;
	new_replay.first_frame_timestamp = 0;
	new_replay.trim_end = 0;
	new_replay.trim_front = 0;
	if (vf) {
		struct obs_source_frame *frame;
		pthread_mutex_lock(&vf->mutex);
		if (vf->video_frames.size) {
			circlebuf_peek_front(&vf->video_frames, &frame,
					     sizeof(struct obs_source_frame *));
			new_replay.first_frame_timestamp = frame->timestamp;
			new_replay.last_frame_timestamp = frame->timestamp;
		}
		new_replay.video_frame_count =
			vf->video_frames.size /
			sizeof(struct obs_source_frame *);
		new_replay.video_frames =
			bzalloc((size_t)new_replay.video_frame_count *
				sizeof(struct obs_source_frame *));
		for (uint64_t i = 0; i < new_replay.video_frame_count; i++) {
			circlebuf_pop_front(&vf->video_frames, &frame,
					    sizeof(struct obs_source_frame *));
			new_replay.last_frame_timestamp = frame->timestamp;
			*(new_replay.video_frames + i) = frame;
		}
		pthread_mutex_unlock(&vf->mutex);
	} else {
		new_replay.video_frames = NULL;
		new_replay.video_frame_count = 0;
	}
	if (af) {
		pthread_mutex_lock(&af->mutex);
		struct obs_audio_data audio;
		if (!vf && af->audio_frames.size) {
			circlebuf_peek_front(&af->audio_frames, &audio,
					     sizeof(struct obs_audio_data));
			new_replay.first_frame_timestamp = audio.timestamp;
			new_replay.last_frame_timestamp = audio.timestamp;
		}
		new_replay.oai = af->oai;
		new_replay.audio_frame_count =
			af->audio_frames.size / sizeof(struct obs_audio_data);
		new_replay.audio_frames =
			bzalloc((size_t)new_replay.audio_frame_count *
				sizeof(struct obs_audio_data));
		for (uint64_t i = 0; i < new_replay.audio_frame_count; i++) {
			circlebuf_pop_front(&af->audio_frames, &audio,
					    sizeof(struct obs_audio_data));
			if (!vf) {
				new_replay.last_frame_timestamp =
					audio.timestamp;
			}
			memcpy(&new_replay.audio_frames[i], &audio,
			       sizeof(struct obs_audio_data));
			for (size_t j = 0; j < MAX_AV_PLANES; j++) {
				if (!audio.data[j])
					break;

				new_replay.audio_frames[i].data[j] =
					audio.data[j];
			}
		}
		pthread_mutex_unlock(&af->mutex);
	} else {
		new_replay.audio_frames = NULL;
		new_replay.audio_frame_count = 0;
		new_replay.oai.speakers = SPEAKERS_STEREO;
		new_replay.oai.samples_per_sec = 48000;
		new_replay.oai.format = AUDIO_FORMAT_FLOAT_PLANAR;
	}
	if (s)
		obs_source_release(s);
	if (as)
		obs_source_release(as);
	new_replay.duration = new_replay.last_frame_timestamp -
			      new_replay.first_frame_timestamp;

	if (context->start_delay > 0) {
		if (context->backward_start) {
			if (context->speed_percent == 100.0f) {
				new_replay.trim_end = context->start_delay * -1;
			} else {
				new_replay.trim_end =
					(int64_t)(context->start_delay *
						  context->speed_percent /
						  -100.0);
			}
			new_replay.trim_front = 0;
		} else {
			if (context->speed_percent == 100.0f) {
				new_replay.trim_front =
					context->start_delay * -1;
			} else {
				new_replay.trim_front =
					(int64_t)(context->start_delay *
						  context->speed_percent /
						  -100.0);
			}
			new_replay.trim_end = 0;
		}
	} else if (context->start_delay < 0 &&
		   context->start_delay * -1 < (int64_t)new_replay.duration) {
		new_replay.trim_front = context->start_delay * -1;
	}

	pthread_mutex_lock(&context->replay_mutex);
	circlebuf_push_back(&context->replays, &new_replay, sizeof new_replay);
	pthread_mutex_unlock(&context->replay_mutex);

	blog(LOG_INFO, "[replay_source: '%s'] replay added of %.2f seconds",
	     obs_source_get_name(context->source),
	     (double)new_replay.duration / (double)1000000000.0);

	if (context->replays.size == sizeof new_replay) {
		replay_update_position(context, true);
	}
	replay_purge_replays(context);
	if (context->load_switch_scene_name) {
		s = obs_get_source_by_name(context->load_switch_scene_name);
		if (s) {
			pthread_t thread;
			pthread_create(&thread, NULL, update_scene_thread, s);
			obs_source_release(s);
		}
	}
}

// Finds closest frame in the current replay given a desired timestamp.
uint64_t find_closest_frame(void *data, uint64_t ts, bool le) {
	struct replay_source *c = data;
	int64_t count = c->current_replay.video_frame_count;
	struct obs_source_frame **frames = c->current_replay.video_frames;
	if (ts < c->current_replay.first_frame_timestamp) {
		return 0;
	} else if (ts > c->current_replay.last_frame_timestamp) {
		return count - 1;
	}

	int64_t i = 0, mid = 0, j = count;
	while (i < j) {
		mid = (i + j) / 2;
		if (frames[mid]->timestamp == ts) {
			return mid;
		} else  if (frames[mid]->timestamp > ts) {
			if (mid > 0 && frames[mid-1]->timestamp < ts) {
				return le ? mid - 1 : mid;
			}
			j = mid;
		} else if (frames[mid]->timestamp < ts) {
			if (mid < count - 1 && frames[mid+1]->timestamp > ts) {
				return le ? mid : mid + 1;
			}
			i = mid + 1;
		}
	}
	if (i == mid + 1) {
		return le ? mid : mid + 1;
	}
	return le ? mid - 1 : mid;
}

static void replay_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			  bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *context = data;
	if (!pressed || !context->source_name)
		return;
	blog(LOG_INFO, "[replay_source: '%s'] Load replay pressed",
	     obs_source_get_name(context->source));
	if (context->retrieve_delay > 0) {
		context->retrieve_timestamp =
			obs_get_video_frame_time() + context->retrieve_delay;
	} else {
		replay_retrieve(context);
	}
}

static void replay_save_hotkey(void *data, obs_hotkey_id id,
			       obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *context = data;
	if (!pressed)
		return;
	blog(LOG_INFO, "[replay_source: '%s'] Save replay pressed",
	     obs_source_get_name(context->source));
	if (context->saving_status == SAVING_STATUS_NONE)
		context->saving_status = SAVING_STATUS_STARTING;
}

static void replay_disable_hotkey(void *data, obs_hotkey_id id,
				  obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if (!pressed || c->disabled)
		return;

	c->disabled = true;
	obs_source_update(c->source, NULL);
}

static void replay_enable_hotkey(void *data, obs_hotkey_id id,
				 obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if (!pressed || !c->disabled)
		return;

	c->disabled = false;
	obs_source_update(c->source, NULL);
}

static void replay_disable_next_scene_hotkey(void *data, obs_hotkey_id id,
					     obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if (!pressed || c->next_scene_disabled)
		return;

	c->next_scene_disabled = true;
}

static void replay_enable_next_scene_hotkey(void *data, obs_hotkey_id id,
					    obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if (!pressed || !c->next_scene_disabled)
		return;

	c->next_scene_disabled = false;
}

static void replay_next_scene_current_hotkey(void *data, obs_hotkey_id id,
					     obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if (!pressed)
		return;

	obs_source_t *current_scene = obs_frontend_get_current_scene();
	if (current_scene == NULL)
		return;

	const char *next_scene_name = obs_source_get_name(current_scene);
	obs_source_release(current_scene);
	if (c->next_scene_name) {
		if (strcmp(c->next_scene_name, next_scene_name) != 0) {
			bfree(c->next_scene_name);
			c->next_scene_name = bstrdup(next_scene_name);
		}
	} else {
		c->next_scene_name = bstrdup(next_scene_name);
	}
}

static void replay_next_scene_hotkey(void *data, obs_hotkey_id id,
				     obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if (!pressed || !c->next_scene_name)
		return;

	obs_source_t *current = obs_frontend_get_current_scene();
	if (current) {
		const char *current_name = obs_source_get_name(current);
		if (strcmp(current_name, c->next_scene_name) != 0) {
			obs_source_t *s =
				obs_get_source_by_name(c->next_scene_name);
			if (s) {
				pthread_t thread;
				pthread_create(&thread, NULL,
					       update_scene_thread, s);
				obs_source_release(s);
			}
		}
		obs_source_release(current);
	} else {
		obs_source_t *s = obs_get_source_by_name(c->next_scene_name);
		if (s) {
			pthread_t thread;
			pthread_create(&thread, NULL, update_scene_thread, s);
			obs_source_release(s);
		}
	}
}

void replay_step_frames(void *data, bool pressed, bool forward, uint64_t num_frames) {
	struct replay_source *c = data;
	if (c->current_replay.video_frame_count && pressed) {
		uint64_t next_pos = c->video_frame_position;
		if (forward) {
			// Check for wrap at end
			if (next_pos + num_frames >= c->current_replay.video_frame_count ||
				c->current_replay.video_frames[next_pos + num_frames]->timestamp >
				c->current_replay.last_frame_timestamp - c->current_replay.trim_end) {
				next_pos = find_closest_frame(c, c->current_replay.first_frame_timestamp + c->current_replay.trim_front, false);
			} else {
				next_pos += num_frames;
			}

		} else {
			// Check for wrap at beginning
			if (c->video_frame_position < num_frames ||
			    c->current_replay.video_frames[c->video_frame_position - num_frames]->timestamp <
				c->current_replay.first_frame_timestamp + c->current_replay.trim_front) {
				next_pos = find_closest_frame(c, c->current_replay.last_frame_timestamp - c->current_replay.trim_end, true);
			} else {
				next_pos -= num_frames;
			}
		}
		if (c->play) {
			c->play = false;
			c->pause_timestamp = obs_get_video_frame_time();
			obs_source_signal(c->source, "media_pause");

		}
		c->stepped = true;
		int64_t prev_time = c->current_replay.video_frames[next_pos]->timestamp;
		int64_t next_time =  c->current_replay.video_frames[c->video_frame_position]->timestamp;
		int64_t time_diff = (int64_t)((next_time - prev_time) * 100 / c->speed_percent);
		if (c->backward) {
			time_diff *= -1;
		}
		c->start_timestamp += time_diff;
		c->video_frame_position = next_pos;
	}
}

static void replay_next_frame_hotkey(void *data, obs_hotkey_id id,
			       obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	replay_step_frames(data, pressed, true, 1);
}

static void replay_prev_frame_hotkey(void *data, obs_hotkey_id id,
			       obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	replay_step_frames(data, pressed, false, 1);
}

static void replay_next_hotkey(void *data, obs_hotkey_id id,
			       obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed)
		replay_next(data);
}

static void replay_previous_hotkey(void *data, obs_hotkey_id id,
				   obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed)
		replay_previous(data);

}

static void replay_first_hotkey(void *data, obs_hotkey_id id,
				obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *context = data;
	if (!pressed)
		return;

	context->replay_position = 0;
	replay_update_position(context, true);
	blog(LOG_INFO,
	     "[replay_source: '%s'] first hotkey switched to replay %i/%i",
	     obs_source_get_name(context->source), context->replay_position + 1,
	     (int)(context->replays.size / sizeof context->current_replay));
}

static void replay_last_hotkey(void *data, obs_hotkey_id id,
			       obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *context = data;
	const int replay_count =
		(int)(context->replays.size / sizeof context->current_replay);
	if (!pressed || replay_count == 0)
		return;

	context->replay_position = replay_count - 1;
	replay_update_position(context, true);
	blog(LOG_INFO,
	     "[replay_source: '%s'] last hotkey switched to replay %i/%i",
	     obs_source_get_name(context->source), context->replay_position + 1,
	     replay_count);
}

static void replay_remove_hotkey(void *data, obs_hotkey_id id,
				 obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *context = data;
	if (!pressed)
		return;
	const int replay_count =
		(int)(context->replays.size / sizeof context->current_replay);

	if (context->replay_position >= replay_count)
		return;

	pthread_mutex_lock(&context->replay_mutex);
	struct replay removed_replay;
	for (int i = 0; i < replay_count; i++) {
		if (i == context->replay_position) {
			circlebuf_pop_front(&context->replays, &removed_replay,
					    sizeof removed_replay);
		} else {
			struct replay replay;
			circlebuf_pop_front(&context->replays, &replay,
					    sizeof replay);
			circlebuf_push_back(&context->replays, &replay,
					    sizeof replay);
		}
	}
	blog(LOG_INFO, "[replay_source: '%s'] remove hotkey removed %i/%i",
	     obs_source_get_name(context->source), context->replay_position + 1,
	     replay_count);
	pthread_mutex_unlock(&context->replay_mutex);
	replay_update_position(context, true);
	replay_free_replay(&removed_replay, context);
}

static void replay_clear_hotkey(void *data, obs_hotkey_id id,
				obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *context = data;
	if (!pressed || context->replays.size == 0)
		return;

	pthread_mutex_lock(&context->video_mutex);
	pthread_mutex_lock(&context->audio_mutex);
	context->current_replay.video_frame_count = 0;
	context->current_replay.audio_frame_count = 0;
	context->replay_position = 0;
	context->end = true;
	context->play = false;
	obs_source_media_ended(context->source);
	pthread_mutex_unlock(&context->audio_mutex);
	pthread_mutex_unlock(&context->video_mutex);
	struct obs_source_frame *f =
		obs_source_frame_create(VIDEO_FORMAT_NONE, 0, 0);
	obs_source_output_video(context->source, f);
	obs_source_frame_destroy(f);
	pthread_mutex_lock(&context->replay_mutex);
	while (context->replays.size) {
		struct replay replay;
		circlebuf_pop_front(&context->replays, &replay, sizeof replay);
		replay_free_replay(&replay, context);
	}
	pthread_mutex_unlock(&context->replay_mutex);
	replay_update_text(context);
	replay_update_progress_crop(context, 0);
	blog(LOG_INFO, "[replay_source: '%s'] clear hotkey",
	     obs_source_get_name(context->source));

	obs_source_media_ended(context->source);
}
void update_speed(struct replay_source *context, float new_speed)
{
	if (new_speed < SETTING_SPEED_MIN)
		new_speed = SETTING_SPEED_MIN;
	if (new_speed > SETTING_SPEED_MAX)
		new_speed = SETTING_SPEED_MAX;

	if (new_speed == context->speed_percent)
		return;
	//info("update speed from %.2f to %.2f", context->speed_percent, new_speed);
	if (context->current_replay.video_frame_count) {
		struct obs_source_frame *peek_frame =
			context->current_replay
				.video_frames[context->video_frame_position];
		uint64_t duration =
			peek_frame->timestamp -
			context->current_replay.first_frame_timestamp;
		if (context->backward) {
			duration =
				context->current_replay.last_frame_timestamp -
				peek_frame->timestamp;
		}
		const uint64_t old_duration =
			(uint64_t)(duration * 100.0 / context->speed_percent);
		const uint64_t new_duration =
			(uint64_t)(duration * 100.0 / new_speed);
		context->start_timestamp += old_duration - new_duration;
	}
	context->speed_percent = new_speed;
	replay_update_text(context);
}

static void replay_faster_hotkey(void *data, obs_hotkey_id id,
				 obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	update_speed(c, c->speed_percent * 3.0f / 2.0f);
}

static void replay_slower_hotkey(void *data, obs_hotkey_id id,
				 obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	update_speed(c, c->speed_percent * 2.0f / 3.0f);
}

static void replay_faster_by_5_hotkey(void *data, obs_hotkey_id id,
				      obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	update_speed(c, c->speed_percent + 5.0f);
}

static void replay_slower_by_5_hotkey(void *data, obs_hotkey_id id,
				      obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	if (c->speed_percent > 5.0f)
		update_speed(c, c->speed_percent - 5.0f);
}

static void replay_normal_or_faster_hotkey(void *data, obs_hotkey_id id,
					   obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;
	if (c->speed_percent < 100.0f) {
		update_speed(c, 100.0f);
	} else {
		update_speed(c, c->speed_percent * 3.0f / 2.0f);
	}
}

static void replay_normal_or_slower_hotkey(void *data, obs_hotkey_id id,
					   obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;
	if (c->speed_percent > 100.0f) {
		update_speed(c, 100.0f);
	} else {
		update_speed(c, c->speed_percent * 2.0f / 3.0f);
	}
}

static void replay_normal_speed_hotkey(void *data, obs_hotkey_id id,
				       obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	update_speed(c, 100.0f);
}

static void replay_half_speed_hotkey(void *data, obs_hotkey_id id,
				     obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	update_speed(c, 50.0f);
}

static void replay_double_speed_hotkey(void *data, obs_hotkey_id id,
				       obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	update_speed(c, 200.0f);
}

static void replay_forward_or_faster_hotkey(void *data, obs_hotkey_id id,
					    obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	if (c->backward) {
		replay_forward_hotkey(data, id, hotkey, pressed);
		return;
	}
	replay_normal_or_faster_hotkey(data, id, hotkey, pressed);
}

static void replay_backward_or_faster_hotkey(void *data, obs_hotkey_id id,
					     obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;
	if (!c->backward) {
		replay_backward_hotkey(data, id, hotkey, pressed);
		return;
	}
	replay_normal_or_faster_hotkey(data, id, hotkey, pressed);
}

static void replay_trim_front_hotkey(void *data, obs_hotkey_id id,
				     obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	const uint64_t timestamp = c->pause_timestamp == 0
					   ? obs_get_video_frame_time()
					   : c->pause_timestamp;
	int64_t duration = timestamp - c->start_timestamp;
	if (c->speed_percent != 100.0f) {
		duration = (int64_t)(duration * c->speed_percent / 100.0);
	}
	if (c->backward) {
		duration = (c->current_replay.last_frame_timestamp -
			    c->current_replay.first_frame_timestamp) -
			   duration;
	}
	if (duration + c->current_replay.first_frame_timestamp <
	    c->current_replay.last_frame_timestamp -
		    c->current_replay.trim_end) {
		c->current_replay.trim_front = duration;
		struct replay *r = circlebuf_data(
			&c->replays,
			c->replay_position * sizeof c->current_replay);
		if (r)
			r->trim_front = duration;
	}
}

static void replay_trim_end_hotkey(void *data, obs_hotkey_id id,
				   obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	const uint64_t timestamp = c->pause_timestamp == 0
					   ? obs_get_video_frame_time()
					   : c->pause_timestamp;
	if (timestamp > c->start_timestamp) {
		int64_t duration = timestamp - c->start_timestamp;
		if (c->speed_percent != 100.0f) {
			duration =
				(int64_t)(duration * c->speed_percent / 100.0);
		}
		if (!c->backward) {
			duration = (c->current_replay.last_frame_timestamp -
				    c->current_replay.first_frame_timestamp) -
				   duration;
		}
		if (c->current_replay.first_frame_timestamp +
			    c->current_replay.trim_front <
		    c->current_replay.last_frame_timestamp - duration) {
			c->current_replay.trim_end = duration;
			struct replay *r = circlebuf_data(
				&c->replays,
				c->replay_position * sizeof c->current_replay);
			if (r)
				r->trim_end = duration;
		}
	}
}

static void replay_trim_reset_hotkey(void *data, obs_hotkey_id id,
				     obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	c->current_replay.trim_end = 0;

	if (c->start_delay > 0) {
		if (c->speed_percent == 100.0f) {
			c->current_replay.trim_front = c->start_delay * -1;
		} else {
			c->current_replay.trim_front =
				(int64_t)(c->start_delay * c->speed_percent /
					  -100.0);
		}
	} else {
		c->current_replay.trim_front = 0;
	}
	struct replay *r = circlebuf_data(
		&c->replays, c->replay_position * sizeof c->current_replay);
	if (r) {
		r->trim_end = c->current_replay.trim_end;
		r->trim_front = c->current_replay.trim_front;
	}
}

void update_filter_settings(obs_source_t *filter, obs_data_t *settings)
{
	obs_data_t *filter_settings = obs_source_get_settings(filter);
	if (!filter_settings)
		return;
	bool changed = false;
	if (obs_data_get_int(filter_settings, SETTING_DURATION) !=
	    obs_data_get_int(settings, SETTING_DURATION)) {
		obs_data_set_int(filter_settings, SETTING_DURATION,
				 obs_data_get_int(settings, SETTING_DURATION));
		changed = true;
	}
	if (obs_data_get_bool(filter_settings, SETTING_SOUND_TRIGGER) !=
	    obs_data_get_bool(settings, SETTING_SOUND_TRIGGER)) {
		obs_data_set_bool(filter_settings, SETTING_SOUND_TRIGGER,
				  obs_data_get_bool(settings,
						    SETTING_SOUND_TRIGGER));
		changed = true;
	}
	if (obs_data_get_double(filter_settings, SETTING_AUDIO_THRESHOLD) !=
	    obs_data_get_double(settings, SETTING_AUDIO_THRESHOLD)) {
		obs_data_set_double(
			filter_settings, SETTING_AUDIO_THRESHOLD,
			obs_data_get_double(settings, SETTING_AUDIO_THRESHOLD));
		changed = true;
	}
	obs_data_release(filter_settings);
	if (changed)
		obs_source_update(filter, NULL);
}

static void replay_source_update(void *data, obs_data_t *settings)
{
	struct replay_source *context = data;
	const char *execute_action =
		obs_data_get_string(settings, SETTING_EXECUTE_ACTION);
	if (execute_action && strlen(execute_action)) {
		if (strcmp(execute_action, "Load") == 0) {
			replay_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Next") == 0) {
			replay_next_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Previous") == 0) {
			replay_previous_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "First") == 0) {
			replay_first_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Last") == 0) {
			replay_last_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Remove") == 0) {
			replay_remove_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Clear") == 0) {
			replay_clear_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Save") == 0) {
			replay_save_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Restart") == 0) {
			replay_restart_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Pause") == 0) {
			replay_pause_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Faster") == 0) {
			replay_faster_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Slower") == 0) {
			replay_slower_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Faster5Percent") == 0) {
			replay_faster_by_5_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Slower5Percent") == 0) {
			replay_slower_by_5_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "NormalOrFaster") == 0) {
			replay_normal_or_faster_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "NormalOrSlower") == 0) {
			replay_normal_or_slower_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "NormalSpeed") == 0) {
			replay_normal_speed_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "HalfSpeed") == 0) {
			replay_half_speed_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "DoubleSpeed") == 0) {
			replay_double_speed_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "TrimFront") == 0) {
			replay_trim_front_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "TrimEnd") == 0) {
			replay_trim_end_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "TrimReset") == 0) {
			replay_trim_reset_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Reverse") == 0) {
			replay_reverse_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Backward") == 0) {
			replay_backward_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "Forward") == 0) {
			replay_forward_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "ForwardOrFaster") == 0) {
			replay_forward_or_faster_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "BackwardOrFaster") == 0) {
			replay_backward_or_faster_hotkey(context, 0, NULL,
							 true);
		} else if (strcmp(execute_action, "DisableNextScene") == 0) {
			context->next_scene_disabled = true;
		} else if (strcmp(execute_action, "EnableNextScene") == 0) {
			context->next_scene_disabled = false;
		} else if (strcmp(execute_action, "Disable") == 0) {
			context->disabled = true;
		} else if (strcmp(execute_action, "Enable") == 0) {
			context->disabled = false;
		} else if (strcmp(execute_action, "SetNextSceneToCurrent") ==
			   0) {
			replay_next_scene_current_hotkey(context, 0, NULL,
							 true);
		} else if (strcmp(execute_action, "SwitchToNextScene") == 0) {
			replay_next_scene_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "NextFrame") == 0) {
			replay_next_frame_hotkey(context, 0, NULL, true);
		} else if (strcmp(execute_action, "PrevFrame") == 0) {
			replay_prev_frame_hotkey(context, 0, NULL, true);
		}
		obs_data_erase(settings, SETTING_EXECUTE_ACTION);
	}
	const char *source_name = obs_data_get_string(settings, SETTING_SOURCE);
	if (context->source_name) {
		if (strcmp(context->source_name, source_name) != 0 ||
		    context->disabled) {
			obs_source_t *s =
				obs_get_source_by_name(context->source_name);
			if (s) {
				do {
					context->source_filter = NULL;
					obs_source_enum_filters(s, EnumFilter,
								data);
					if (context->source_filter) {
						obs_source_filter_remove(
							s,
							context->source_filter);
					}
				} while (context->source_filter);
				obs_source_release(s);
			}
			if (strcmp(context->source_name, source_name) != 0) {
				bfree(context->source_name);
				context->source_name = bstrdup(source_name);
			}
		}
	} else {
		context->source_name = bstrdup(source_name);
	}
	const char *source_audio_name =
		obs_data_get_string(settings, SETTING_SOURCE_AUDIO);
	if (context->source_audio_name) {
		if (strcmp(context->source_audio_name, source_audio_name) !=
			    0 ||
		    context->disabled) {
			obs_source_t *s = obs_get_source_by_name(
				context->source_audio_name);
			if (s) {
				do {
					context->source_audio_filter = NULL;
					obs_source_enum_filters(
						s, EnumAudioFilter, data);
					if (context->source_audio_filter) {
						obs_source_filter_remove(
							s,
							context->source_audio_filter);
					}
				} while (context->source_audio_filter);
				obs_source_release(s);
			}
			if (strcmp(context->source_audio_name,
				   source_audio_name) != 0) {
				bfree(context->source_audio_name);
				context->source_audio_name =
					bstrdup(source_audio_name);
			}
		}
	} else {
		context->source_audio_name = bstrdup(source_audio_name);
	}
	const char *next_scene_name =
		obs_data_get_string(settings, SETTING_NEXT_SCENE);
	if (context->next_scene_name) {
		if (strcmp(context->next_scene_name, next_scene_name) != 0) {
			bfree(context->next_scene_name);
			context->next_scene_name = bstrdup(next_scene_name);
		}
	} else {
		context->next_scene_name = bstrdup(next_scene_name);
	}

	const char *load_switch_scene_name =
		obs_data_get_string(settings, SETTING_LOAD_SWITCH_SCENE);
	if (context->load_switch_scene_name) {
		if (strcmp(context->load_switch_scene_name,
			   load_switch_scene_name) != 0) {
			bfree(context->load_switch_scene_name);
			context->load_switch_scene_name =
				bstrdup(load_switch_scene_name);
		}
	} else {
		context->load_switch_scene_name =
			bstrdup(load_switch_scene_name);
	}

	context->visibility_action =
		(int)obs_data_get_int(settings, SETTING_VISIBILITY_ACTION);
	context->end_action =
		(int)obs_data_get_int(settings, SETTING_END_ACTION);
	context->start_delay =
		obs_data_get_int(settings, SETTING_START_DELAY) * 1000000;
	context->retrieve_delay =
		obs_data_get_int(settings, SETTING_RETRIEVE_DELAY) * 1000000;

	context->replay_max = (int)obs_data_get_int(settings, SETTING_REPLAYS);
	replay_purge_replays(context);

	context->speed_percent =
		(float)obs_data_get_double(settings, SETTING_SPEED);
	if (context->speed_percent < SETTING_SPEED_MIN ||
	    context->speed_percent > SETTING_SPEED_MAX)
		context->speed_percent = 100.0f;

	context->backward_start = obs_data_get_bool(settings, SETTING_BACKWARD);
	if (context->backward != context->backward_start) {
		replay_reverse_hotkey(context, 0, NULL, true);
	}
	context->sound_trigger =
		obs_data_get_bool(settings, SETTING_SOUND_TRIGGER);
	if (!context->disabled) {

		obs_source_t *s = NULL;
		if (strcmp(context->source_name,
			   obs_source_get_name(context->source)) != 0) {
			s = obs_get_source_by_name(context->source_name);
		}
		if (s) {
			if (strcmp(obs_source_get_unversioned_id(s),
				   "dshow_input_replay") == 0) {
				if (obs_obj_get_data(s)) {
					update_filter_settings(s, settings);
					((struct replay_filter *)
						 obs_obj_get_data(s))
						->threshold_data = data;
					((struct replay_filter *)
						 obs_obj_get_data(s))
						->trigger_threshold =
						context->sound_trigger
							? replay_trigger_threshold
							: NULL;
					context->filter_loaded = true;
					blog(LOG_INFO,
					     "[replay_source: '%s'] connected to dshow '%s'",
					     obs_source_get_name(
						     context->source),
					     context->source_name);
				}
			} else {
				context->source_filter = NULL;
				obs_source_enum_filters(s, EnumFilter, data);
				if (!context->source_filter) {
					if ((obs_source_get_output_flags(s) &
					     OBS_SOURCE_ASYNC) ==
					    OBS_SOURCE_ASYNC) {
						context->source_filter = obs_source_create_private(
							REPLAY_FILTER_ASYNC_ID,
							obs_source_get_name(
								context->source),
							settings);
						blog(LOG_INFO,
						     "[replay_source: '%s'] created async filter for '%s'",
						     obs_source_get_name(
							     context->source),
						     context->source_name);
					} else {
						context->source_filter =
							obs_source_create_private(
								REPLAY_FILTER_ID,
								obs_source_get_name(
									context->source),
								settings);
						blog(LOG_INFO,
						     "[replay_source: '%s'] created filter for '%s'",
						     obs_source_get_name(
							     context->source),
						     context->source_name);
					}
					if (context->source_filter) {
						obs_source_filter_add(
							s,
							context->source_filter);
					}
				} else if (obs_obj_get_data(
						   context->source_filter)) {
					update_filter_settings(
						context->source_filter,
						settings);
					blog(LOG_INFO,
					     "[replay_source: '%s'] updated filter for '%s'",
					     obs_source_get_name(
						     context->source),
					     context->source_name);
				}
				if (obs_obj_get_data(context->source_filter)) {
					((struct replay_filter *)
						 obs_obj_get_data(
							 context->source_filter))
						->threshold_data = data;
					((struct replay_filter *)
						 obs_obj_get_data(
							 context->source_filter))
						->trigger_threshold =
						context->sound_trigger
							? replay_trigger_threshold
							: NULL;
					context->filter_loaded = true;
					blog(LOG_INFO,
					     "[replay_source: '%s'] connected to '%s'",
					     obs_source_get_name(
						     context->source),
					     context->source_name);
				}
			}
			obs_source_release(s);
		}
		s = NULL;
		if (strcmp(context->source_audio_name,
			   obs_source_get_name(context->source)) != 0) {
			s = obs_get_source_by_name(context->source_audio_name);
		}
		if (s) {
			if (strcmp(obs_source_get_unversioned_id(s),
				   "dshow_input_replay") == 0) {
				if (obs_obj_get_data(s)) {
					update_filter_settings(s, settings);
					((struct replay_filter *)
						 obs_obj_get_data(s))
						->threshold_data = data;
					((struct replay_filter *)
						 obs_obj_get_data(s))
						->trigger_threshold =
						context->sound_trigger
							? replay_trigger_threshold
							: NULL;
					context->filter_loaded = true;
					blog(LOG_INFO,
					     "[replay_source: '%s'] connected to dshow '%s'",
					     obs_source_get_name(
						     context->source),
					     context->source_audio_name);
				}
			} else {
				context->source_audio_filter = NULL;
				obs_source_enum_filters(s, EnumAudioVideoFilter,
							data);
				if (!context->source_audio_filter) {
					if ((obs_source_get_output_flags(s) &
					     OBS_SOURCE_AUDIO) != 0) {
						context->source_audio_filter =
							obs_source_create_private(
								REPLAY_FILTER_AUDIO_ID,
								obs_source_get_name(
									context->source),
								settings);
						blog(LOG_INFO,
						     "[replay_source: '%s'] created audio filter for '%s'",
						     obs_source_get_name(
							     context->source),
						     context->source_audio_name);
					}
					if (context->source_audio_filter) {
						obs_source_filter_add(
							s,
							context->source_audio_filter);
					}
				} else if (obs_obj_get_data(
						   context->source_audio_filter)) {
					update_filter_settings(
						context->source_audio_filter,
						settings);
					blog(LOG_INFO,
					     "[replay_source: '%s'] updated audio filter for '%s'",
					     obs_source_get_name(
						     context->source),
					     context->source_audio_name);
				}
				if (obs_obj_get_data(
					    context->source_audio_filter)) {
					((struct replay_filter *)obs_obj_get_data(
						 context->source_audio_filter))
						->threshold_data = data;
					((struct replay_filter *)obs_obj_get_data(
						 context->source_audio_filter))
						->trigger_threshold =
						context->sound_trigger
							? replay_trigger_threshold
							: NULL;
					context->filter_loaded = true;
					blog(LOG_INFO,
					     "[replay_source: '%s'] connected to '%s'",
					     obs_source_get_name(
						     context->source),
					     context->source_audio_name);
				}
			}
			obs_source_release(s);
		}
	}
	const char *file_format =
		obs_data_get_string(settings, SETTING_FILE_FORMAT);
	if (context->file_format) {
		if (strcmp(context->file_format, file_format) != 0) {
			bfree(context->file_format);
			context->file_format = bstrdup(file_format);
		}
	} else {
		context->file_format = bstrdup(file_format);
	}
	const char *progress_source =
		obs_data_get_string(settings, SETTING_PROGRESS_SOURCE);
	if (context->progress_source_name) {
		if (strcmp(context->progress_source_name, progress_source) !=
		    0) {
			bfree(context->progress_source_name);
			context->progress_source_name =
				bstrdup(progress_source);
		}
	} else {
		context->progress_source_name = bstrdup(progress_source);
	}

	const char *text_source =
		obs_data_get_string(settings, SETTING_TEXT_SOURCE);
	if (context->text_source_name) {
		if (strcmp(context->text_source_name, text_source) != 0) {
			bfree(context->text_source_name);
			context->text_source_name = bstrdup(text_source);
		}
	} else {
		context->text_source_name = bstrdup(text_source);
	}

	const char *text = obs_data_get_string(settings, SETTING_TEXT);
	if (context->text_format) {
		if (strcmp(context->text_format, text) != 0) {
			bfree(context->text_format);
			context->text_format = bstrdup(text);
		}
	} else {
		context->text_format = bstrdup(text);
	}

	context->lossless = obs_data_get_bool(settings, SETTING_LOSSLESS);
	const char *directory =
		obs_data_get_string(settings, SETTING_DIRECTORY);
	if (context->directory) {
		if (strcmp(context->directory, directory) != 0) {
			bfree(context->directory);
			context->directory = bstrdup(directory);
		}
	} else {
		context->directory = bstrdup(directory);
	}
	replay_update_text(context);
}

static void *replay_source_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct replay_source *context = bzalloc(sizeof(struct replay_source));
	context->source = source;
	obs_source_set_async_unbuffered(source, true);
	pthread_mutex_init(&context->video_mutex, NULL);
	pthread_mutex_init(&context->audio_mutex, NULL);
	pthread_mutex_init(&context->replay_mutex, NULL);

	circlebuf_init(&context->replays);

	context->replay_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Replay", obs_module_text("LoadReplay"),
		replay_hotkey, context);

	context->next_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Next", obs_module_text("Next"),
		replay_next_hotkey, context);

	context->previous_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Previous", obs_module_text("Previous"),
		replay_previous_hotkey, context);

	context->first_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.First", obs_module_text("First"),
		replay_first_hotkey, context);

	context->last_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Last", obs_module_text("Last"),
		replay_last_hotkey, context);

	context->remove_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Remove", obs_module_text("Remove"),
		replay_remove_hotkey, context);

	context->clear_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Clear", obs_module_text("Clear"),
		replay_clear_hotkey, context);

	context->save_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Save", obs_module_text("SaveReplay"),
		replay_save_hotkey, context);

	context->restart_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Restart", obs_module_text("Restart"),
		replay_restart_hotkey, context);

	context->pause_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Pause", obs_module_text("Pause"),
		replay_pause_hotkey, context);

	context->faster_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Faster", obs_module_text("Faster"),
		replay_faster_hotkey, context);

	context->slower_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Slower", obs_module_text("Slower"),
		replay_slower_hotkey, context);

	context->faster_by_5_hotkey =
		obs_hotkey_register_source(source, "ReplaySource.FasterBy5",
					   obs_module_text("Faster5Percent"),
					   replay_faster_by_5_hotkey, context);

	context->slower_by_5_hotkey =
		obs_hotkey_register_source(source, "ReplaySource.SlowerBy5",
					   obs_module_text("Slower5Percent"),
					   replay_slower_by_5_hotkey, context);

	context->normal_or_faster_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.NormalOrFaster",
		obs_module_text("NormalOrFaster"),
		replay_normal_or_faster_hotkey, context);

	context->normal_or_slower_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.NormalOrSlower",
		obs_module_text("NormalOrSlower"),
		replay_normal_or_slower_hotkey, context);

	context->normal_speed_hotkey =
		obs_hotkey_register_source(source, "ReplaySource.NormalSpeed",
					   obs_module_text("NormalSpeed"),
					   replay_normal_speed_hotkey, context);

	context->half_speed_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.HalfSpeed", obs_module_text("HalfSpeed"),
		replay_half_speed_hotkey, context);

	context->double_speed_hotkey =
		obs_hotkey_register_source(source, "ReplaySource.DoubleSpeed",
					   obs_module_text("DoubleSpeed"),
					   replay_double_speed_hotkey, context);

	context->trim_front_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.TrimFront", obs_module_text("TrimFront"),
		replay_trim_front_hotkey, context);

	context->trim_end_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.TrimEnd", obs_module_text("TrimEnd"),
		replay_trim_end_hotkey, context);

	context->trim_reset_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.TrimReset", obs_module_text("TrimReset"),
		replay_trim_reset_hotkey, context);

	context->reverse_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Reverse", obs_module_text("Reverse"),
		replay_reverse_hotkey, context);

	context->forward_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Forward", obs_module_text("Forward"),
		replay_forward_hotkey, context);

	context->backward_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Backward", obs_module_text("Backward"),
		replay_backward_hotkey, context);

	context->forward_or_faster_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.ForwardOrFaster",
		obs_module_text("ForwardOrFaster"),
		replay_forward_or_faster_hotkey, context);

	context->backward_or_faster_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.BackwardOrFaster",
		obs_module_text("BackwardOrFaster"),
		replay_backward_or_faster_hotkey, context);

	context->disable_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Disable", obs_module_text("Disable"),
		replay_disable_hotkey, context);

	context->enable_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.Enable", obs_module_text("Enable"),
		replay_enable_hotkey, context);

	context->disable_next_scene_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.DisableNextScene",
		obs_module_text("DisableNextScene"),
		replay_disable_next_scene_hotkey, context);

	context->enable_next_scene_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.EnableNextScene",
		obs_module_text("EnableNextScene"),
		replay_enable_next_scene_hotkey, context);

	context->next_scene_current_hotkey = obs_hotkey_register_source(
		source, "ReplaySource.NextSceneCurrent",
		obs_module_text("SetNextSceneToCurrent"),
		replay_next_scene_current_hotkey, context);

	context->next_scene_hotkey =
		obs_hotkey_register_source(source, "ReplaySource.NextScene",
					   obs_module_text("SwitchToNextScene"),
					   replay_next_scene_hotkey, context);
	
	context->prev_frame_hotkey =
		obs_hotkey_register_source(source, "ReplaySource.PrevFrame",
		obs_module_text("PrevFrame"),
		replay_prev_frame_hotkey, context);

	context->next_frame_hotkey =
		obs_hotkey_register_source(source, "ReplaySource.NextFrame",
		obs_module_text("NextFrame"),
		replay_next_frame_hotkey, context);

	return context;
}

static void replay_source_destroy(void *data)
{
	struct replay_source *context = data;

	pthread_mutex_lock(&context->video_mutex);
	pthread_mutex_lock(&context->audio_mutex);
	context->current_replay.video_frame_count = 0;
	context->current_replay.video_frames = NULL;
	context->current_replay.audio_frame_count = 0;
	context->current_replay.audio_frames = NULL;
	pthread_mutex_unlock(&context->video_mutex);
	pthread_mutex_unlock(&context->audio_mutex);

	if (context->source_name)
		bfree(context->source_name);

	if (context->source_audio_name)
		bfree(context->source_audio_name);

	if (context->next_scene_name)
		bfree(context->next_scene_name);

	if (context->load_switch_scene_name)
		bfree(context->load_switch_scene_name);

	if (context->directory)
		bfree(context->directory);

	if (context->file_format)
		bfree(context->file_format);

	if (context->progress_source_name)
		bfree(context->progress_source_name);

	if (context->text_source_name)
		bfree(context->text_source_name);

	if (context->text_format)
		bfree(context->text_format);

	if (context->h264Recording) {
		obs_encoder_release(context->h264Recording);
		context->h264Recording = NULL;
	}

	if (context->aac) {
		obs_encoder_release(context->aac);
		context->aac = NULL;
	}

	if (context->fileOutput) {
		obs_output_release(context->fileOutput);
		context->fileOutput = NULL;
	}

	if (context->video_output) {
		video_output_close(context->video_output);
		context->video_output = NULL;
	}

	if (context->audio_t) {
		audio_output_close(context->audio_t);
		context->audio_t = NULL;
	}
	pthread_mutex_lock(&context->replay_mutex);
	while (context->replays.size) {
		circlebuf_pop_front(&context->replays, &context->current_replay,
				    sizeof context->current_replay);
		replay_free_replay(&context->current_replay, context);
	}
	circlebuf_free(&context->replays);
	pthread_mutex_unlock(&context->replay_mutex);

	if (context->scaler) {
		video_scaler_destroy(context->scaler);
		context->scaler = NULL;
	}

	pthread_mutex_destroy(&context->video_mutex);
	pthread_mutex_destroy(&context->audio_mutex);
	pthread_mutex_destroy(&context->replay_mutex);
	bfree(context);
}

static void replay_output_frame(struct replay_source *context,
				struct obs_source_frame *frame)
{
	uint64_t t = frame->timestamp;
	if (t < context->current_replay.first_frame_timestamp ||
	    t > context->current_replay.last_frame_timestamp)
		return;
	if (context->backward) {
		frame->timestamp =
			context->current_replay.last_frame_timestamp -
			frame->timestamp;
	} else {
		frame->timestamp -=
			context->current_replay.first_frame_timestamp;
	}
	if (context->speed_percent != 100.0f) {
		frame->timestamp = (uint64_t)(frame->timestamp * 100.0 /
					      context->speed_percent);
	}
	frame->timestamp += context->start_timestamp;
	if (context->previous_frame_timestamp <= frame->timestamp) {
		context->previous_frame_timestamp = frame->timestamp;
		obs_source_output_video(context->source, frame);
	}
	frame->timestamp = t;
	replay_update_progress_crop(context, t);
}

void replay_source_end_action(struct replay_source *context)
{
	const int replay_count =
		(int)(context->replays.size / sizeof context->current_replay);
	if (replay_count == 0) {
		context->play = false;
		context->end = true;
		obs_source_media_ended(context->source);
		return;
	}
	bool finish = false;
	if (context->end_action == END_ACTION_HIDE ||
	    context->end_action == END_ACTION_PAUSE ||
	    (context->end_action == END_ACTION_HIDE_ALL && replay_count == 1) ||
	    (context->end_action == END_ACTION_PAUSE_ALL &&
	     replay_count == 1)) {
		context->play = false;
		context->end = true;
		finish = true;
		obs_source_media_ended(context->source);
	} else if (context->end_action == END_ACTION_REVERSE ||
		   (context->end_action == END_ACTION_REVERSE_ALL &&
		    replay_count == 1)) {
		replay_reverse_hotkey(context, 0, NULL, true);
	} else if (context->end_action == END_ACTION_LOOP ||
		   (context->end_action == END_ACTION_LOOP_ALL &&
		    replay_count == 1)) {
		context->restart = true;
	} else {
		if (context->backward) {
			if (context->replay_position <= 0) {
				context->replay_position = replay_count - 1;
				finish = true;
			} else {
				context->replay_position--;
				replay_update_position(context, false);
			}
		} else {
			if (context->replay_position + 1 >= replay_count) {
				context->replay_position = 0;
				finish = true;
			} else {
				context->replay_position++;
				replay_update_position(context, false);
			}
		}
		if (finish) {
			if (context->end_action == END_ACTION_REVERSE_ALL) {
				replay_reverse_hotkey(context, 0, NULL, true);
				finish = false;
			} else if (context->end_action == END_ACTION_LOOP_ALL) {
				replay_update_position(context, false);
				finish = false;
			} else {
				context->play = false;
				context->end = true;
				obs_source_media_ended(context->source);
			}
		}
	}
	if (finish && context->next_scene_name && context->active &&
	    !context->next_scene_disabled) {
		obs_source_t *current = obs_frontend_get_current_scene();
		if (current) {
			const char *current_name = obs_source_get_name(current);
			if (strcmp(current_name, context->next_scene_name) !=
			    0) {
				obs_source_t *s = obs_get_source_by_name(
					context->next_scene_name);
				if (s) {
					obs_source_release(s);
					pthread_t thread;
					pthread_create(&thread, NULL,
						       update_scene_thread, s);
				}
			}
			obs_source_release(current);
		} else {
			obs_source_t *s = obs_get_source_by_name(
				context->next_scene_name);
			if (s) {
				obs_source_release(s);
				pthread_t thread;
				pthread_create(&thread, NULL,
					       update_scene_thread, s);
			}
		}
	}
}

static void replay_source_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct replay_source *context = data;

	const uint64_t os_timestamp = obs_get_video_frame_time();

	if (context->retrieve_timestamp &&
	    context->retrieve_timestamp < os_timestamp) {
		context->retrieve_timestamp = 0;
		replay_retrieve(context);
	}
	if (!context->filter_loaded) {
		if (context->source_name) {
			obs_source_t *s =
				obs_get_source_by_name(context->source_name);
			if (s) {
				obs_data_t *settings = obs_source_get_settings(
					context->source);
				replay_source_update(data, settings);
				obs_data_release(settings);
				obs_source_release(s);
				return;
			}
		}
		if (context->source_audio_name) {
			obs_source_t *s = obs_get_source_by_name(
				context->source_audio_name);
			if (s) {
				obs_data_t *settings = obs_source_get_settings(
					context->source);
				replay_source_update(data, settings);
				obs_data_release(settings);
				obs_source_release(s);
				return;
			}
		}
		return;
	}

	if (context->saving_status == SAVING_STATUS_STARTING) {
		replay_save(context);
	} else if (context->saving_status == SAVING_STATUS_STARTING2) {
		if (obs_output_active(context->fileOutput) ||
		    os_timestamp - context->start_save_timestamp >
			    1000000000UL) {
			context->saving_status = SAVING_STATUS_SAVING;
		}
	} else if (context->saving_status == SAVING_STATUS_SAVING) {
		if (!obs_output_active(context->fileOutput)) {
			const char *error =
				obs_output_get_last_error(context->fileOutput);
			blog(LOG_ERROR,
			     "[replay_source: '%s'] error output not active: %s",
			     obs_source_get_name(context->source), error);
			context->saving_status = SAVING_STATUS_NONE;
			if (context->free_after_save) {
				replay_free_replay(&context->saving_replay,
						   context);
			}
		} else if (context->video_save_position >=
			   context->saving_replay.video_frame_count) {
			context->saving_status = SAVING_STATUS_STOPPING;
			obs_output_stop(context->fileOutput);
		} else {
			pthread_mutex_lock(&context->video_mutex);
			struct obs_source_frame *frame =
				context->saving_replay.video_frames
					[context->video_save_position];
			while (frame->timestamp <
			       context->saving_replay.first_frame_timestamp +
				       context->saving_replay.trim_front) {
				context->video_save_position++;
				if (context->video_save_position >=
				    context->saving_replay.video_frame_count) {
					context->saving_status =
						SAVING_STATUS_STOPPING;
					break;
				}
				frame = context->saving_replay.video_frames
						[context->video_save_position];
			}
			uint64_t timestamp = frame->timestamp;
			if (context->start_save_timestamp >
			    context->saving_replay.first_frame_timestamp) {
				timestamp += context->start_save_timestamp -
					     context->saving_replay
						     .first_frame_timestamp;
			}
			timestamp -= context->saving_replay.trim_front;

			struct video_frame output_frame;
			if (video_output_lock_frame(context->video_output,
						    &output_frame, 1,
						    timestamp)) {
				video_scaler_scale(
					context->scaler, output_frame.data,
					output_frame.linesize,
					(const uint8_t *const *)frame->data,
					frame->linesize);
				video_output_unlock_frame(
					context->video_output);
			}
			if (timestamp <= os_timestamp) {
				context->video_save_position++;
				if (context->video_save_position >=
				    context->saving_replay.video_frame_count) {
					context->saving_status =
						SAVING_STATUS_STOPPING;
					obs_output_stop(context->fileOutput);
				} else {
					frame = context->saving_replay.video_frames
							[context->video_save_position];
					if (frame->timestamp >=
					    context->saving_replay
							    .last_frame_timestamp -
						    context->saving_replay
							    .trim_end) {
						context->saving_status =
							SAVING_STATUS_STOPPING;
						obs_output_stop(
							context->fileOutput);
					}
				}
			}
			pthread_mutex_unlock(&context->video_mutex);
		}
	} else if (context->saving_status == SAVING_STATUS_STOPPING) {
		if (!context->fileOutput ||
		    !obs_output_active(context->fileOutput)) {
			context->saving_status = SAVING_STATUS_NONE;
			if (context->free_after_save) {
				replay_free_replay(&context->saving_replay,
						   context);
			}
		} else {

			if (os_timestamp - context->start_save_timestamp >
			    context->saving_replay.last_frame_timestamp -
				    context->saving_replay
					    .first_frame_timestamp) {
				obs_output_stop(context->fileOutput);
				pthread_mutex_lock(&context->video_mutex);
				if (context->video_save_position >=
				    context->saving_replay.video_frame_count) {
					context->video_save_position =
						context->saving_replay
							.video_frame_count -
						1;
				}
				struct obs_source_frame *frame =
					context->saving_replay.video_frames
						[context->video_save_position];
				struct video_frame output_frame;
				if (video_output_lock_frame(
					    context->video_output,
					    &output_frame, 1, os_timestamp)) {
					video_scaler_scale(
						context->scaler,
						output_frame.data,
						output_frame.linesize,
						(const uint8_t *const *)
							frame->data,
						frame->linesize);
					video_output_unlock_frame(
						context->video_output);
				}
				pthread_mutex_unlock(&context->video_mutex);
			}
		}
	} else if (context->fileOutput) {
		obs_output_release(context->fileOutput);
		context->fileOutput = NULL;
		if (context->h264Recording) {
			obs_encoder_release(context->h264Recording);
			context->h264Recording = NULL;
		}
		if (context->aac) {
			obs_encoder_release(context->aac);
			context->aac = NULL;
		}
		blog(LOG_INFO, "[replay_source: '%s'] stopped saving",
		     obs_source_get_name(context->source));
	}

	pthread_mutex_lock(&context->video_mutex);
	if (!context->current_replay.video_frame_count &&
	    !context->current_replay.audio_frame_count) {
		if (context->play) {
			context->play = false;
			obs_source_signal(context->source, "media_pause");
		}
	} else if (context->disabled) {
		context->play = false;
		context->end = true;
		obs_source_media_ended(context->source);
	}
	if (!context->play) {
		if (context->end &&
		    (context->end_action == END_ACTION_HIDE ||
		     context->end_action == END_ACTION_HIDE_ALL)) {
			struct obs_source_frame *f = obs_source_frame_create(
				VIDEO_FORMAT_NONE, 0, 0);
			obs_source_output_video(context->source, f);
			obs_source_frame_destroy(f);
		}
		if (context->stepped) {
			context->stepped = false;
			struct obs_source_frame *frame =
				context->current_replay
					.video_frames[context->video_frame_position];
			uint64_t t = frame->timestamp;
			frame->timestamp = os_timestamp;
			obs_source_output_video(context->source, frame);
			frame->timestamp = t;
			replay_update_text(context);
		}
		pthread_mutex_unlock(&context->video_mutex);
		return;
	}
	if (context->end) {
		context->end = false;
		obs_source_media_started(context->source);
	}
	replay_update_text(context);

	if (context->current_replay.video_frame_count) {
		if (context->video_frame_position >=
		    context->current_replay.video_frame_count) {
			context->video_frame_position =
				context->current_replay.video_frame_count - 1;
		}
		struct obs_source_frame *frame =
			context->current_replay
				.video_frames[context->video_frame_position];
		if (context->backward) {
			if (context->restart) {

				context->video_frame_position =
					context->current_replay
						.video_frame_count -
					1;
				frame = context->current_replay.video_frames
						[context->video_frame_position];
				context->start_timestamp = os_timestamp;
				context->restart = false;
				if (context->current_replay.trim_end != 0) {
					context->start_timestamp -=
						(uint64_t)(context->current_replay
									.trim_end *
								100.0 /
								context->speed_percent);

					if (context->current_replay.trim_end <
					    0) {
						uint64_t t = frame->timestamp;
						frame->timestamp = os_timestamp;
						context->previous_frame_timestamp =
							frame->timestamp;
						obs_source_output_video(
							context->source, frame);
						frame->timestamp = t;
						pthread_mutex_unlock(
							&context->video_mutex);
						return;
					}
					uint64_t desired_ts = context->current_replay.last_frame_timestamp - context->current_replay.trim_end;
					int64_t desired_frame_num = find_closest_frame(context, desired_ts, true);
					frame = context->current_replay.video_frames
							[desired_frame_num];
				}
			}

			const int64_t video_duration =
				os_timestamp -
				(int64_t)context->start_timestamp;
			//TODO audio backwards
			int64_t source_duration =
				(int64_t)((context->current_replay
						   .last_frame_timestamp -
					   frame->timestamp) *
					  100.0 / context->speed_percent);

			struct obs_source_frame *output_frame = frame;
			while (context->play &&
			       video_duration >= source_duration) {
				output_frame = frame;
				if (frame->timestamp <=
				    context->current_replay
						    .first_frame_timestamp +
					    context->current_replay.trim_front) {
					replay_source_end_action(context);
					break;
				}
				if (context->video_frame_position == 0) {
					replay_source_end_action(context);
					break;
				}
				context->video_frame_position--;

				frame = context->current_replay.video_frames
						[context->video_frame_position];

				source_duration =
					(int64_t)((context->current_replay
							   .last_frame_timestamp -
						   frame->timestamp) *
						  100.0 /
						  context->speed_percent);
			}
			if (context->video_frame_position == 0) {
				replay_source_end_action(context);
			} else  {
				replay_output_frame(context, output_frame);
			}
		} else {
			if (context->restart) {
				context->video_frame_position = 0;
				context->restart = false;
				context->start_timestamp = os_timestamp;
				context->audio_frame_position = 0;
				frame = context->current_replay.video_frames
						[context->video_frame_position];
				if (context->current_replay.trim_front != 0) {
					context->start_timestamp -=
						(uint64_t)(context->current_replay
									.trim_front *
								100.0 /
								context->speed_percent);
					if (context->current_replay.trim_front <
					    0) {
						uint64_t t = frame->timestamp;
						frame->timestamp = os_timestamp;
						context->previous_frame_timestamp =
							frame->timestamp;
						obs_source_output_video(
							context->source, frame);
						frame->timestamp = t;
						pthread_mutex_unlock(
							&context->video_mutex);
						return;
					}
					uint64_t desired_ts = context->current_replay
							       .first_frame_timestamp +
						       context->current_replay
							       .trim_front;
					int64_t desired_frame_num = find_closest_frame(context, desired_ts, false);
					frame = context->current_replay.video_frames
							[desired_frame_num];
				}
			}
			if (context->start_timestamp > os_timestamp) {
				pthread_mutex_unlock(&context->video_mutex);
				return;
			}
			const int64_t video_duration =
				(int64_t)os_timestamp -
				(int64_t)context->start_timestamp;

			if (context->current_replay.audio_frame_count > 1) {
				pthread_mutex_lock(&context->audio_mutex);
				struct obs_audio_data peek_audio =
					context->current_replay.audio_frames
						[context->audio_frame_position];
				const int64_t frame_duration =
					(context->current_replay
						 .last_frame_timestamp -
					 context->current_replay
						 .first_frame_timestamp) /
					context->current_replay
						.video_frame_count;
				//const uint64_t duration = audio_frames_to_ns(info.samples_per_sec, peek_audio.frames);
				int64_t audio_duration =
					(int64_t)(((int64_t)peek_audio.timestamp -
						   (int64_t)context
							   ->current_replay
							   .first_frame_timestamp) *
						  100.0 /
						  context->speed_percent);
				while (context->play &&
				       video_duration + frame_duration >
					       audio_duration) {
					if (peek_audio.timestamp >
						    context->current_replay
								    .first_frame_timestamp -
							    frame_duration &&
					    peek_audio.timestamp <
						    context->current_replay
								    .last_frame_timestamp +
							    frame_duration) {
						context->audio.frames =
							peek_audio.frames;

						if (context->speed_percent !=
						    100.0f) {
							context->audio
								.timestamp =
								(uint64_t)(context->start_timestamp +
									   (((int64_t)peek_audio
										     .timestamp -
									     (int64_t)context
										     ->current_replay
										     .first_frame_timestamp) *
									    100.0 /
									    context->speed_percent));
							context->audio
								.samples_per_sec =
								(uint32_t)(context->current_replay
										   .oai
										   .samples_per_sec *
									   context->speed_percent /
									   100.0);
						} else {
							context->audio
								.timestamp =
								peek_audio
									.timestamp +
								context->start_timestamp -
								context->current_replay
									.first_frame_timestamp;
							context->audio
								.samples_per_sec =
								context->current_replay
									.oai
									.samples_per_sec;
						}
						for (size_t i = 0;
						     i < MAX_AV_PLANES; i++) {
							context->audio.data[i] =
								peek_audio
									.data[i];
						}

						context->audio.speakers =
							context->current_replay
								.oai.speakers;
						context->audio.format =
							context->current_replay
								.oai.format;

						obs_source_output_audio(
							context->source,
							&context->audio);
					}
					context->audio_frame_position++;
					if (context->audio_frame_position >=
					    context->current_replay
						    .audio_frame_count) {
						context->audio_frame_position =
							0;
						break;
					}
					peek_audio =
						context->current_replay.audio_frames
							[context->audio_frame_position];
					audio_duration =
						(int64_t)(((int64_t)peek_audio
								   .timestamp -
							   (int64_t)context
								   ->current_replay
								   .first_frame_timestamp) *
							  100.0 /
							  context->speed_percent);
				}
				pthread_mutex_unlock(&context->audio_mutex);
			}
			int64_t source_duration =
				(int64_t)((frame->timestamp -
					   context->current_replay
						   .first_frame_timestamp) *
					  100.0 / context->speed_percent);
			struct obs_source_frame *output_frame = frame;
			while (context->play &&
			       video_duration >= source_duration) {
				output_frame = frame;
				if (frame->timestamp >=
				    context->current_replay.last_frame_timestamp -
					    context->current_replay.trim_end) {
					replay_source_end_action(context);
					break;
				}
				context->video_frame_position++;
				if (context->video_frame_position >=
				    context->current_replay.video_frame_count) {
					context->video_frame_position =
						context->current_replay
							.video_frame_count -
						1;
					replay_source_end_action(context);
					break;
				}
				frame = context->current_replay.video_frames
						[context->video_frame_position];
				source_duration =
					(int64_t)((frame->timestamp -
						   context->current_replay
							   .first_frame_timestamp) *
						  100.0 /
						  context->speed_percent);
			}
			if (context->video_frame_position >=
				   context->current_replay.video_frame_count -
					   1) {
				context->video_frame_position =
					context->current_replay
						.video_frame_count -
					1;
				replay_source_end_action(context);
			} else {
				replay_output_frame(context, output_frame);
			}
		}
	} else if (context->current_replay.audio_frame_count) {
		//no video, only audio
		struct obs_audio_data peek_audio =
			context->current_replay
				.audio_frames[context->audio_frame_position];

		if (context->current_replay.first_frame_timestamp ==
		    peek_audio.timestamp) {
			context->start_timestamp = os_timestamp;
			context->pause_timestamp = 0;
			context->restart = false;
		} else if (context->restart) {
			context->audio_frame_position = 0;
			peek_audio = context->current_replay.audio_frames
					     [context->audio_frame_position];
			context->restart = false;
			context->start_timestamp = os_timestamp;
			context->pause_timestamp = 0;
		}
		if (context->start_timestamp == os_timestamp &&
		    context->current_replay.trim_front != 0) {
			if (context->speed_percent == 100.0f) {
				context->start_timestamp -=
					context->current_replay.trim_front;
			} else {
				context->start_timestamp -=
					(uint64_t)(context->current_replay
							   .trim_front *
						   100.0 /
						   context->speed_percent);
			}
			if (context->current_replay.trim_front < 0) {
				pthread_mutex_unlock(&context->video_mutex);
				return;
			}
			while (peek_audio.timestamp <
			       context->current_replay.first_frame_timestamp +
				       context->current_replay.trim_front) {
				context->audio_frame_position++;
				if (context->audio_frame_position >=
				    context->current_replay.audio_frame_count) {
					context->audio_frame_position = 0;
					break;
				}
				peek_audio =
					context->current_replay.audio_frames
						[context->audio_frame_position];
			}
		}
		if (context->start_timestamp > os_timestamp) {
			pthread_mutex_unlock(&context->video_mutex);
			return;
		}

		const int64_t video_duration =
			os_timestamp - context->start_timestamp;

		int64_t audio_duration =
			(int64_t)(((int64_t)peek_audio.timestamp -
				   (int64_t)context->current_replay
					   .first_frame_timestamp) *
				  100.0 / context->speed_percent);

		while (context->play &&
		       context->current_replay.audio_frame_count > 1 &&
		       video_duration >= audio_duration) {
			if (peek_audio.timestamp >=
			    context->current_replay.last_frame_timestamp -
				    context->current_replay.trim_end) {
				if (context->end_action != END_ACTION_LOOP) {
					context->play = false;
					context->end = true;
					obs_source_media_ended(context->source);
				} else if (context->current_replay.trim_end !=
					   0) {
					context->restart = true;
				}
				if (context->next_scene_name &&
				    context->active) {
					obs_source_t *s = obs_get_source_by_name(
						context->next_scene_name);
					if (s) {
						obs_frontend_set_current_scene(
							s);
						obs_source_release(s);
					}
				}
			}

			context->audio.frames = peek_audio.frames;

			if (context->speed_percent != 100.0f) {
				context->audio.timestamp =
					(uint64_t)(context->start_timestamp +
						   (peek_audio.timestamp -
						    context->current_replay
							    .first_frame_timestamp) *
							   100.0 /
							   context->speed_percent);
				context->audio.samples_per_sec =
					(uint32_t)(context->current_replay.oai
							   .samples_per_sec *
						   context->speed_percent /
						   100.0);
			} else {
				context->audio.timestamp =
					peek_audio.timestamp +
					context->start_timestamp -
					context->current_replay
						.first_frame_timestamp;
				context->audio.samples_per_sec =
					context->current_replay.oai
						.samples_per_sec;
			}
			for (size_t i = 0; i < MAX_AV_PLANES; i++) {
				context->audio.data[i] = peek_audio.data[i];
			}
			context->audio.speakers =
				context->current_replay.oai.speakers;
			context->audio.format =
				context->current_replay.oai.format;

			obs_source_output_audio(context->source,
						&context->audio);
			context->audio_frame_position++;
			if (context->audio_frame_position >=
			    context->current_replay.audio_frame_count) {
				context->audio_frame_position = 0;
				break;
			}
			peek_audio = context->current_replay.audio_frames
					     [context->audio_frame_position];
			audio_duration =
				(int64_t)(((int64_t)peek_audio.timestamp -
					   (int64_t)context->current_replay
						   .first_frame_timestamp) *
					  100.0 / context->speed_percent);
		}
	}
	pthread_mutex_unlock(&context->video_mutex);
}
static bool EnumVideoSources(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) != 0)
		obs_property_list_add_string(prop, obs_source_get_name(source),
					     obs_source_get_name(source));
	return true;
}
static bool EnumAudioSources(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0)
		obs_property_list_add_string(prop, obs_source_get_name(source),
					     obs_source_get_name(source));
	return true;
}
static bool EnumScenes(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE)
		obs_property_list_add_string(prop, obs_source_get_name(source),
					     obs_source_get_name(source));
	return true;
}
static bool replay_button(obs_properties_t *props, obs_property_t *property,
			  void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct replay_source *s = data;
	replay_retrieve(s);
	return false; // no properties changed
}

static bool EnumTextSources(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	const char *source_id = obs_source_get_unversioned_id(source);
	if (strcmp(source_id, "text_gdiplus") == 0 ||
	    strcmp(source_id, "text_ft2_source") == 0)
		obs_property_list_add_string(prop, obs_source_get_name(source),
					     obs_source_get_name(source));
	return true;
}

static bool replay_video_source_modified(obs_properties_t *props,
					 obs_property_t *property,
					 obs_data_t *data)
{
	UNUSED_PARAMETER(property);
	const char *source_name = obs_data_get_string(data, SETTING_SOURCE);
	bool async_source = false;
	if (source_name) {
		obs_source_t *s = obs_get_source_by_name(source_name);
		if (s && (obs_source_get_output_flags(s) & OBS_SOURCE_ASYNC) ==
				 OBS_SOURCE_ASYNC) {
			async_source = true;
		}
		obs_source_release(s);
	}
	obs_property_t *prop =
		obs_properties_get(props, SETTING_INTERNAL_FRAMES);
	obs_property_set_visible(prop, async_source);
	return true;
}

static bool replay_text_source_modified(obs_properties_t *props,
					obs_property_t *property,
					obs_data_t *data)
{
	UNUSED_PARAMETER(property);
	const char *source_name =
		obs_data_get_string(data, SETTING_TEXT_SOURCE);
	bool text_source = false;
	if (source_name) {
		obs_source_t *s = obs_get_source_by_name(source_name);
		if (s) {
			text_source = true;
		}
		obs_source_release(s);
	}
	obs_property_t *prop = obs_properties_get(props, SETTING_TEXT);
	obs_property_set_visible(prop, text_source);
	return true;
}

static bool replay_sound_trigger_modified(obs_properties_t *props,
					  obs_property_t *property,
					  obs_data_t *data)
{
	UNUSED_PARAMETER(property);
	const bool sound_trigger =
		obs_data_get_bool(data, SETTING_SOUND_TRIGGER);
	obs_property_t *prop =
		obs_properties_get(props, SETTING_AUDIO_THRESHOLD);
	obs_property_set_visible(prop, sound_trigger);
	return true;
}

static obs_properties_t *replay_source_properties(void *data)
{
	struct replay_source *s = data;
	UNUSED_PARAMETER(s);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop = obs_properties_add_list(
		props, SETTING_SOURCE, obs_module_text("VideoSource"),
		OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, "", "");
	obs_enum_sources(EnumVideoSources, prop);
	obs_enum_scenes(EnumVideoSources, prop);
	obs_property_set_modified_callback(prop, replay_video_source_modified);
	obs_properties_add_bool(props, SETTING_INTERNAL_FRAMES,
				obs_module_text("CaptureInternalFrames"));

	prop = obs_properties_add_list(props, SETTING_SOURCE_AUDIO,
				       obs_module_text("AudioSource"),
				       OBS_COMBO_TYPE_EDITABLE,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, "", "");
	obs_enum_sources(EnumAudioSources, prop);

	prop = obs_properties_add_int(props, SETTING_DURATION,
				      obs_module_text("Duration"),
				      SETTING_DURATION_MIN,
				      SETTING_DURATION_MAX, 1000);
	obs_property_int_set_suffix(prop, "ms");
	prop = obs_properties_add_int(props, SETTING_RETRIEVE_DELAY,
				      obs_module_text("LoadDelay"), 0, 100000,
				      1000);
	obs_property_int_set_suffix(prop, "ms");
	obs_properties_add_int(props, SETTING_REPLAYS,
			       obs_module_text("MaxReplays"), 1, 10, 1);

	prop = obs_properties_add_list(props, SETTING_VISIBILITY_ACTION,
				       obs_module_text("VisibilityAction"),
				       OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Restart"),
				  VISIBILITY_ACTION_RESTART);
	obs_property_list_add_int(prop, obs_module_text("Pause"),
				  VISIBILITY_ACTION_PAUSE);
	obs_property_list_add_int(prop, obs_module_text("Continue"),
				  VISIBILITY_ACTION_CONTINUE);
	obs_property_list_add_int(prop, obs_module_text("None"),
				  VISIBILITY_ACTION_NONE);

	prop = obs_properties_add_int(props, SETTING_START_DELAY,
				      obs_module_text("StartDelay"), -100000,
				      100000, 1000);
	obs_property_int_set_suffix(prop, "ms");

	prop = obs_properties_add_list(props, SETTING_END_ACTION,
				       obs_module_text("EndAction"),
				       OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("HideAfterSingle"),
				  END_ACTION_HIDE);
	obs_property_list_add_int(prop, obs_module_text("HideAfterAll"),
				  END_ACTION_HIDE_ALL);
	obs_property_list_add_int(prop, obs_module_text("PauseAfterSingle"),
				  END_ACTION_PAUSE);
	obs_property_list_add_int(prop, obs_module_text("PauseAfterAll"),
				  END_ACTION_PAUSE_ALL);
	obs_property_list_add_int(prop, obs_module_text("LoopSingle"),
				  END_ACTION_LOOP);
	obs_property_list_add_int(prop, obs_module_text("LoopAll"),
				  END_ACTION_LOOP_ALL);
	obs_property_list_add_int(prop, obs_module_text("ReverseAfterSingle"),
				  END_ACTION_REVERSE);
	obs_property_list_add_int(prop, obs_module_text("ReverseAfterAll"),
				  END_ACTION_REVERSE_ALL);

	prop = obs_properties_add_list(props, SETTING_NEXT_SCENE,
				       obs_module_text("NextScene"),
				       OBS_COMBO_TYPE_EDITABLE,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, "", "");
	obs_enum_scenes(EnumScenes, prop);

	obs_properties_add_float_slider(props, SETTING_SPEED,
					obs_module_text("SpeedPercentage"),
					SETTING_SPEED_MIN, SETTING_SPEED_MAX,
					1.0);
	obs_properties_add_bool(props, SETTING_BACKWARD,
				obs_module_text("Backwards"));

	obs_properties_add_path(props, SETTING_DIRECTORY,
				obs_module_text("Directory"),
				OBS_PATH_DIRECTORY, NULL, NULL);
	obs_properties_add_text(props, SETTING_FILE_FORMAT,
				obs_module_text("FilenameFormatting"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props, SETTING_LOSSLESS,
				obs_module_text("Lossless"));

	prop = obs_properties_add_list(props, SETTING_PROGRESS_SOURCE,
				       obs_module_text("ProgressCropSource"),
				       OBS_COMBO_TYPE_EDITABLE,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, "", "");
	obs_enum_sources(EnumVideoSources, prop);

	prop = obs_properties_add_list(props, SETTING_TEXT_SOURCE,
				       obs_module_text("TextSource"),
				       OBS_COMBO_TYPE_EDITABLE,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, "", "");
	obs_enum_sources(EnumTextSources, prop);
	obs_property_set_modified_callback(prop, replay_text_source_modified);

	obs_properties_add_text(props, SETTING_TEXT,
				obs_module_text("TextFormat"),
				OBS_TEXT_MULTILINE);

	prop = obs_properties_add_bool(
		props, SETTING_SOUND_TRIGGER,
		obs_module_text("SoundTriggerLoadReplay"));
	obs_property_set_modified_callback(prop, replay_sound_trigger_modified);

	obs_properties_add_float_slider(props, SETTING_AUDIO_THRESHOLD,
					obs_module_text("ThresholdDb"),
					SETTING_AUDIO_THRESHOLD_MIN,
					SETTING_AUDIO_THRESHOLD_MAX, 0.1);

	prop = obs_properties_add_list(props, SETTING_LOAD_SWITCH_SCENE,
				       obs_module_text("LoadReplaySwitchScene"),
				       OBS_COMBO_TYPE_EDITABLE,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, "", "");
	obs_enum_scenes(EnumScenes, prop);

	obs_properties_add_button(props, "replay_button",
				  obs_module_text("LoadReplay"), replay_button);

	return props;
}

int64_t replay_get_duration(void *data)
{
	struct replay_source *c = data;
	return (int64_t)((c->current_replay.duration / c->speed_percent *
			  100.0) /
			 1000000UL);
}

int64_t replay_get_time(void *data)
{
	struct replay_source *c = data;
	if (c->replays.size && c->start_timestamp) {
		uint64_t time = 0;
		if (c->pause_timestamp > c->start_timestamp) {
			time = c->pause_timestamp - c->start_timestamp;
		} else {
			time = obs_get_video_frame_time() - c->start_timestamp;
		}
		return time / 1000000UL;
	}
	return 0;
}

void replay_set_time(void *data, int64_t seconds)
{
	struct replay_source *c = data;

	if (c->pause_timestamp > c->start_timestamp) {
		c->start_timestamp = c->pause_timestamp - seconds * 1000000UL;
	} else {
		c->start_timestamp =
			obs_get_video_frame_time() - seconds * 1000000UL;
	}
}

enum obs_media_state replay_get_state(void *data)
{
	struct replay_source *c = data;
	if (c->play)
		return OBS_MEDIA_STATE_PLAYING;
	if (c->end)
		return OBS_MEDIA_STATE_ENDED;
	return OBS_MEDIA_STATE_PAUSED;
}

struct obs_source_info replay_source_info = {
	.id = REPLAY_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
			OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = replay_source_get_name,
	.create = replay_source_create,
	.destroy = replay_source_destroy,
	.load = replay_source_update,
	.update = replay_source_update,
	.get_defaults = replay_source_defaults,
	.show = replay_source_show,
	.hide = replay_source_hide,
	.activate = replay_source_active,
	.deactivate = replay_source_deactive,
	.video_tick = replay_source_tick,
	.get_properties = replay_source_properties,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.media_get_duration = replay_get_duration,
	.media_get_state = replay_get_state,
	.media_get_time = replay_get_time,
	.media_next = replay_next,
	.media_play_pause = replay_play_pause,
	.media_previous = replay_previous,
	.media_restart = replay_restart,
	.media_stop = replay_stop,
	.media_set_time = replay_set_time,
};

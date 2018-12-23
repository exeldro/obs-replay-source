#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <../UI/obs-frontend-api/obs-frontend-api.h>
#include <obs-scene.h>
#include "replay.h"

#define blog(log_level, format, ...) \
	blog(log_level, "[replay_source: '%s'] " format, \
			obs_source_get_name(context->source), ##__VA_ARGS__)

#define debug(format, ...) \
	blog(LOG_DEBUG, format, ##__VA_ARGS__)
#define info(format, ...) \
	blog(LOG_INFO, format, ##__VA_ARGS__)
#define warn(format, ...) \
	blog(LOG_WARNING, format, ##__VA_ARGS__)

#define VISIBILITY_ACTION_RESTART 0
#define VISIBILITY_ACTION_PAUSE 1
#define VISIBILITY_ACTION_CONTINUE 2
#define VISIBILITY_ACTION_NONE 3

#define END_ACTION_HIDE 0
#define END_ACTION_PAUSE 1
#define END_ACTION_LOOP 2

struct replay_source {
	obs_source_t  *source;
	obs_source_t  *source_filter;
	obs_source_t  *source_audio_filter;
	char          *source_name;
	char          *source_audio_name;
	long          duration;
	int           speed_percent;
	int           visibility_action;
	int           end_action;
	char          *next_scene_name;
	obs_hotkey_id replay_hotkey;
	obs_hotkey_id restart_hotkey;
	obs_hotkey_id pause_hotkey;
	obs_hotkey_id faster_hotkey;
	obs_hotkey_id slower_hotkey;
	obs_hotkey_id normal_speed_hotkey;
	obs_hotkey_id half_speed_hotkey;
	obs_hotkey_id trim_front_hotkey;
	obs_hotkey_id trim_end_hotkey;
	obs_hotkey_id trim_reset_hotkey;
	uint64_t      first_frame_timestamp;
	uint64_t      start_timestamp;
	uint64_t      last_frame_timestamp;
	uint64_t      previous_frame_timestamp;
	uint64_t      pause_timestamp;
	int64_t       trim_front;
	int64_t       trim_end;
	int64_t       start_delay;
	struct obs_source_audio audio;

	bool          play;
	bool          restart;
	bool          active;
	bool          end;
	
	/* contains struct obs_source_frame* */
	struct circlebuf               video_frames;

	/* stores the audio data */
	struct circlebuf               audio_frames;
	struct obs_audio_data          audio_output;

	pthread_mutex_t    mutex;
};


static const char *replay_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ReplayInput");
}

static void EnumFilter(obs_source_t *source, obs_source_t *filter, void *data)
{
	struct replay_source *c = data;
	const char *filterName = obs_source_get_name(filter);
	const char *sourceName = obs_source_get_name(c->source);
	const char *id = obs_source_get_id(filter);
	if ((strcmp(REPLAY_FILTER_ASYNC_ID, id) == 0 || strcmp(REPLAY_FILTER_ID, id) == 0) && strcmp(filterName, sourceName) == 0)
		c->source_filter = filter;

}
static void EnumAudioFilter(obs_source_t *source, obs_source_t *filter, void *data)
{
	struct replay_source *c = data;
	const char *filterName = obs_source_get_name(filter);
	const char *sourceName = obs_source_get_name(c->source);
	const char *id = obs_source_get_id(filter);
	if (strcmp(REPLAY_FILTER_AUDIO_ID, id) == 0 && strcmp(filterName, sourceName) == 0)
		c->source_audio_filter = filter;

}


static void replay_source_update(void *data, obs_data_t *settings)
{
	struct replay_source *context = data;
	const char *source_name = obs_data_get_string(settings, SETTING_SOURCE);
	if (context->source_name){
		if(strcmp(context->source_name, source_name) != 0){
			obs_source_t *s = obs_get_source_by_name(context->source_name);
			if(s){
				do{
					context->source_filter = NULL;
					obs_source_enum_filters(s, EnumFilter, data);
					if(context->source_filter)
					{
						obs_source_filter_remove(s,context->source_filter);
					}
				}while(context->source_filter);
				obs_source_release(s);
			}
			bfree(context->source_name);
			context->source_name = bstrdup(source_name);
		}
	}else{
		context->source_name = bstrdup(source_name);
	}
	const char *source_audio_name = obs_data_get_string(settings, SETTING_SOURCE_AUDIO);
	if (context->source_audio_name){
		if(strcmp(context->source_audio_name, source_audio_name) != 0){
			obs_source_t *s = obs_get_source_by_name(context->source_audio_name);
			if(s){
				do{
					context->source_audio_filter = NULL;
					obs_source_enum_filters(s, EnumAudioFilter, data);
					if(context->source_audio_filter)
					{
						obs_source_filter_remove(s,context->source_audio_filter);
					}
				}while(context->source_audio_filter);
				obs_source_release(s);
			}
			bfree(context->source_audio_name);
			context->source_audio_name = bstrdup(source_audio_name);
		}
	}else{
		context->source_audio_name = bstrdup(source_audio_name);
	}
	const char *next_scene_name = obs_data_get_string(settings, "next_scene");
	if (context->next_scene_name){
		if(strcmp(context->next_scene_name, next_scene_name) != 0){
			bfree(context->next_scene_name);
			context->next_scene_name = bstrdup(next_scene_name);
		}
	}else{
		context->next_scene_name = bstrdup(next_scene_name);
	}

	context->duration = (long)obs_data_get_int(settings, SETTING_DURATION);
	context->visibility_action = (int)obs_data_get_int(settings, SETTING_VISIBILITY_ACTION);
	context->end_action = (int)obs_data_get_int(settings, SETTING_END_ACTION);
	context->start_delay = obs_data_get_int(settings,SETTING_START_DELAY)*1000000;

	context->speed_percent = (int)obs_data_get_int(settings, SETTING_SPEED);
	if (context->speed_percent < 1 || context->speed_percent > 200)
		context->speed_percent = 100;

	obs_source_t *s = obs_get_source_by_name(context->source_name);
	if(s){
		context->source_filter = NULL;
		obs_source_enum_filters(s, EnumFilter, data);
		if(!context->source_filter)
		{
			if((obs_source_get_output_flags(s) & OBS_SOURCE_ASYNC) == OBS_SOURCE_ASYNC)
			{
				context->source_filter = obs_source_create_private(REPLAY_FILTER_ASYNC_ID,obs_source_get_name(context->source), settings);
			}
			else
			{
				context->source_filter = obs_source_create_private(REPLAY_FILTER_ID,obs_source_get_name(context->source), settings);
			}
			if(context->source_filter){
				obs_source_filter_add(s,context->source_filter);
			}
		}else{
			obs_source_update(context->source_filter,settings);
		}

		obs_source_release(s);
	}
	s = obs_get_source_by_name(context->source_audio_name);
	if(s){
		context->source_audio_filter = NULL;
		obs_source_enum_filters(s, EnumAudioFilter, data);
		if(!context->source_audio_filter)
		{
			if((obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO) != 0)
			{
				context->source_audio_filter = obs_source_create_private(REPLAY_FILTER_AUDIO_ID,obs_source_get_name(context->source), settings);
			}
			if(context->source_audio_filter){
				obs_source_filter_add(s,context->source_audio_filter);
			}
		}else{
			obs_source_update(context->source_audio_filter,settings);
		}
		obs_source_release(s);
	}
}

static void replay_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_DURATION, 5);
	obs_data_set_default_int(settings, SETTING_SPEED, 100);
	obs_data_set_default_int(settings, SETTING_VISIBILITY_ACTION, VISIBILITY_ACTION_CONTINUE);
	obs_data_set_default_int(settings, SETTING_START_DELAY, 0);
	obs_data_set_default_int(settings, SETTING_END_ACTION, END_ACTION_LOOP);
}

static void replay_source_show(void *data)
{
	struct replay_source *context = data;
}

static void replay_source_hide(void *data)
{
	struct replay_source *context = data;

}

static void replay_source_active(void *data)
{
	struct replay_source *context = data;
	if(context->visibility_action == VISIBILITY_ACTION_PAUSE || context->visibility_action == VISIBILITY_ACTION_CONTINUE)
	{
		if(!context->play){
			context->play = true;
			if(context->pause_timestamp)
			{
				context->start_timestamp += obs_get_video_frame_time() - context->pause_timestamp;
			}
		}
	}
	else if(context->visibility_action == VISIBILITY_ACTION_RESTART)
	{
		context->play = true;
		context->restart = true;
	}
	context->active = true;
}

static void replay_source_deactive(void *data)
{
	struct replay_source *context = data;
	if(context->visibility_action == VISIBILITY_ACTION_PAUSE)
	{
		if(context->play){
			context->play = false;
			context->pause_timestamp = obs_get_video_frame_time();
		}
	}
	else if(context->visibility_action == VISIBILITY_ACTION_RESTART)
	{
		context->play = false;
		context->restart = true;
	}
	context->active = false;
}

static void replay_restart_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(pressed){
		c->restart = true;
		c->play = true;
	}
}

static void replay_pause_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(pressed){
		if(c->play)
		{
			c->play = false;
			c->pause_timestamp = obs_get_video_frame_time();
		}else
		{
			c->play = true;
			if(c->pause_timestamp)
			{
				c->start_timestamp += obs_get_video_frame_time() - c->pause_timestamp;
			}
		}
	}
}

static void replay_retrieve(struct replay_source *c)
{

	obs_source_t *s = obs_get_source_by_name(c->source_name);
	c->source_filter = NULL;
	if(s){
		obs_source_enum_filters(s, EnumFilter, c);
	}
	obs_source_t *as = obs_get_source_by_name(c->source_audio_name);
	c->source_audio_filter = NULL;
	if(as){
		obs_source_enum_filters(as, EnumAudioFilter, c);
	}

	struct replay_filter* vf = c->source_filter?c->source_filter->context.data:NULL;
	struct replay_filter* af = c->source_audio_filter?c->source_audio_filter->context.data:vf;
	if(vf && vf->video_frames.size == 0)
		vf = NULL;
	if(af && af->audio_frames.size == 0)
		af = NULL;

	if(!vf && !af){
		if(s)
			obs_source_release(s);
		if(as)
			obs_source_release(as);
		return;
	}
	pthread_mutex_lock(&c->mutex);
	struct obs_source_frame *frame;
	while (c->video_frames.size) {
		circlebuf_pop_front(&c->video_frames, &frame, sizeof(struct obs_source_frame*));
		if (os_atomic_dec_long(&frame->refs) <= 0) {
			obs_source_frame_destroy(frame);
			frame = NULL;
		}
	}
	while (c->audio_frames.size) {
		struct obs_audio_data audio;
		circlebuf_pop_front(&c->audio_frames, &audio,
		sizeof(struct obs_audio_data));
		free_audio_packet(&audio);
	}
	c->start_timestamp = obs_get_video_frame_time();
	c->pause_timestamp = 0;
	if(c->speed_percent == 100){
		c->trim_front = c->start_delay * -1;
	}else{
		c->trim_front = c->start_delay * c->speed_percent / -100;
	}
	c->trim_end = 0;
	
	if(vf){
		struct obs_source_frame *frame;
		pthread_mutex_lock(&vf->mutex);
		if(vf->video_frames.size){
			circlebuf_peek_front(&vf->video_frames, &frame, sizeof(struct obs_source_frame*));
			c->first_frame_timestamp = frame->timestamp;
			c->last_frame_timestamp = frame->timestamp;
		}
		while (vf->video_frames.size) {
			circlebuf_pop_front(&vf->video_frames, &frame, sizeof(struct obs_source_frame*));
			c->last_frame_timestamp = frame->timestamp;
			circlebuf_push_back(&c->video_frames, &frame, sizeof(struct obs_source_frame*));
		}
		pthread_mutex_unlock(&vf->mutex);
	}
	if(af){
		struct obs_audio_info info;
		obs_get_audio_info(&info);
		pthread_mutex_lock(&af->mutex);
		struct obs_audio_data audio;
		if (!vf && af->audio_frames.size)
		{
			circlebuf_peek_front(&af->audio_frames, &audio, sizeof(struct obs_audio_data));
			c->first_frame_timestamp = audio.timestamp;
			c->last_frame_timestamp = audio.timestamp;
		}
		while (af->audio_frames.size) {
			struct obs_audio_data cached;
			circlebuf_pop_front(&af->audio_frames, &audio, sizeof(struct obs_audio_data));
			const uint64_t duration = audio_frames_to_ns(info.samples_per_sec, audio.frames);
			const uint64_t end_timestamp = audio.timestamp + duration;
			if(!vf){
				c->last_frame_timestamp = audio.timestamp;
			}
			memcpy(&cached, &audio, sizeof(cached));
			for (size_t i = 0; i < MAX_AV_PLANES; i++) {
				if (!audio.data[i])
					break;

				cached.data[i] = bmemdup(audio.data[i], cached.frames * sizeof(float));
			}
			circlebuf_push_back(&c->audio_frames, &cached, sizeof(struct obs_audio_data));
		}
		pthread_mutex_unlock(&af->mutex);
	}
	pthread_mutex_unlock(&c->mutex);
	if(c->active || c->visibility_action == VISIBILITY_ACTION_CONTINUE || c->visibility_action == VISIBILITY_ACTION_NONE)
	{
		c->play = true;
	}
	if(s)
		obs_source_release(s);
	if(as)
		obs_source_release(as);
}

static void replay_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if(!pressed || !c->source_name)
		return;

	replay_retrieve(c);
}

void update_speed(struct replay_source *c, int new_speed)
{
	if(new_speed < 1)
		new_speed = 1;

	if(new_speed == c->speed_percent)
		return;
	if(c->video_frames.size)
	{
		struct obs_source_frame *peek_frame = NULL;
		circlebuf_peek_front(&c->video_frames, &peek_frame, sizeof(struct obs_source_frame*));
		const uint64_t duration = peek_frame->timestamp - c->first_frame_timestamp;
		const uint64_t old_duration = duration * 100 / c->speed_percent;
		const uint64_t new_duration = duration * 100 / new_speed;
		c->start_timestamp += old_duration - new_duration;
	}
	c->speed_percent = new_speed;
}

static void replay_faster_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed)
		return;

	update_speed(c, c->speed_percent*3/2);
}

static void replay_slower_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed)
		return;

	update_speed(c, c->speed_percent*2/3);
}

static void replay_normal_speed_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed)
		return;

	update_speed(c, 100);
}

static void replay_half_speed_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed)
		return;

	update_speed(c, 50);
}

static void replay_trim_front_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed)
		return;

	const uint64_t timestamp = obs_get_video_frame_time();
	int64_t duration = timestamp - c->start_timestamp;
	if(c->speed_percent != 100)
	{
		duration = duration * c->speed_percent / 100;
	}
	if(duration + c->first_frame_timestamp < c->last_frame_timestamp - c->trim_end){
		c->trim_front = duration;
	}
}

static void replay_trim_end_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed)
		return;
	const uint64_t timestamp = obs_get_video_frame_time();
	if(timestamp > c->start_timestamp)
	{
		uint64_t duration = timestamp - c->start_timestamp;
		if(c->speed_percent != 100)
		{
			duration = duration * c->speed_percent / 100;
		}
		if(duration < (c->first_frame_timestamp + c->trim_front) - c->last_frame_timestamp)
		{			
			c->trim_end = (c->last_frame_timestamp - c->first_frame_timestamp) - duration;
		}
	}

}

static void replay_trim_reset_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed)
		return;
	c->trim_end = 0;
	if(c->speed_percent == 100){
		c->trim_front = c->start_delay * -1;
	}else{
		c->trim_front = c->start_delay * c->speed_percent / -100;
	}
}

static void *replay_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct replay_source *context = bzalloc(sizeof(struct replay_source));
	context->source = source;
	pthread_mutex_init(&context->mutex, NULL);

	replay_source_update(context, settings);

	context->replay_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.Replay",
			obs_module_text("Replay"),
			replay_hotkey, context);
	
	context->restart_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.Restart",
			obs_module_text("Restart"),
			replay_restart_hotkey, context);
	
	context->pause_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.Pause",
			obs_module_text("Pause"),
			replay_pause_hotkey, context);

	context->faster_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.Faster",
			obs_module_text("Faster"),
			replay_faster_hotkey, context);

	context->slower_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.Slower",
			obs_module_text("Slower"),
			replay_slower_hotkey, context);

	context->normal_speed_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.NormalSpeed",
			obs_module_text("Normal speed"),
			replay_normal_speed_hotkey, context);

	context->half_speed_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.HalfSpeed",
			obs_module_text("Half speed"),
			replay_half_speed_hotkey, context);

	context->trim_front_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.TrimFront",
			obs_module_text("Trim front"),
			replay_trim_front_hotkey, context);

	context->trim_end_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.TrimEnd",
			obs_module_text("Trim end"),
			replay_trim_end_hotkey, context);

	context->trim_reset_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.TrimReset",
			obs_module_text("Trim reset"),
			replay_trim_reset_hotkey, context);

	return context;
}

static void replay_source_destroy(void *data)
{
	struct replay_source *context = data;

	if (context->source_name)
		bfree(context->source_name);

	if (context->source_audio_name)
		bfree(context->source_audio_name);

	if (context->next_scene_name)
		bfree(context->next_scene_name);

	pthread_mutex_lock(&context->mutex);
	while (context->video_frames.size) {
		struct obs_source_frame *frame;

		circlebuf_pop_front(&context->video_frames, &frame,
				sizeof(struct obs_source_frame*));

		if (os_atomic_dec_long(&frame->refs) <= 0) {
			obs_source_frame_destroy(frame);
			frame = NULL;
		}
	}
	pthread_mutex_unlock(&context->mutex);
	circlebuf_free(&context->video_frames);
	circlebuf_free(&context->audio_frames);
	pthread_mutex_destroy(&context->mutex);
	bfree(context);
}

static void replay_source_tick(void *data, float seconds)
{
	struct replay_source *context = data;

	if(!context->video_frames.size && !context->audio_frames.size){
		context->play = false;
	}
	if(!context->play)
	{
		if(context->end && context->end_action == END_ACTION_HIDE)
		{
			obs_source_output_video(context->source, NULL);
		}
		return;
	}
	context->end = false;
	const uint64_t timestamp = obs_get_video_frame_time();

	pthread_mutex_lock(&context->mutex);
	if(context->video_frames.size){
		struct obs_source_frame *frame = NULL;
		struct obs_source_frame *peek_frame = NULL;
		circlebuf_peek_front(&context->video_frames, &peek_frame, sizeof(struct obs_source_frame*));
		
		if(context->first_frame_timestamp == peek_frame->timestamp)
		{
			context->start_timestamp = timestamp;
			context->pause_timestamp = 0;
			context->restart = false;
			context->audio.timestamp = os_gettime_ns();
			context->audio.frames = 0;
			obs_source_output_audio(context->source, &context->audio);
		}
		else if(context->restart)
		{
			while(peek_frame->timestamp != context->first_frame_timestamp)
			{
				circlebuf_pop_front(&context->video_frames, &frame, sizeof(struct obs_source_frame*));
				circlebuf_push_back(&context->video_frames, &frame, sizeof(struct obs_source_frame*));
				circlebuf_peek_front(&context->video_frames, &peek_frame, sizeof(struct obs_source_frame*));
			}
			context->restart = false;
			context->start_timestamp = timestamp;
			context->pause_timestamp = 0;
			context->audio.timestamp = os_gettime_ns();
			context->audio.frames = 0;
			obs_source_output_audio(context->source, &context->audio);
			if(context->audio_frames.size > sizeof(struct obs_audio_data))
			{
				struct obs_audio_data peek_audio;
				circlebuf_peek_front(&context->audio_frames, &peek_audio, sizeof(peek_audio));
				uint64_t prev_time = 0;
				while (prev_time <= peek_audio.timestamp)
				{
					circlebuf_pop_front(&context->audio_frames, NULL, sizeof(peek_audio));
					circlebuf_push_back(&context->audio_frames, &peek_audio, sizeof(peek_audio));
					prev_time = peek_audio.timestamp;
					circlebuf_peek_front(&context->audio_frames, &peek_audio, sizeof(peek_audio));
				}
			}
		}
		if(context->start_timestamp == timestamp && context->trim_front != 0){
			if(context->speed_percent == 100){
				context->start_timestamp -= context->trim_front;
			}else{
				context->start_timestamp -= context->trim_front * 100 / context->speed_percent;
			}
			if(context->trim_front < 0){
				circlebuf_pop_front(&context->video_frames, &frame, sizeof(struct obs_source_frame*));
				circlebuf_push_back(&context->video_frames, &frame, sizeof(struct obs_source_frame*));
				const uint64_t t = frame->timestamp;
				frame->timestamp = context->start_timestamp;
				context->previous_frame_timestamp = frame->timestamp;
				obs_source_output_video(context->source, frame);
				frame->timestamp = t;
				pthread_mutex_unlock(&context->mutex);
				return;
			}
			while(peek_frame->timestamp < context->first_frame_timestamp + context->trim_front)
			{
				circlebuf_pop_front(&context->video_frames, &frame, sizeof(struct obs_source_frame*));
				circlebuf_push_back(&context->video_frames, &frame, sizeof(struct obs_source_frame*));
				circlebuf_peek_front(&context->video_frames, &peek_frame, sizeof(struct obs_source_frame*));
			}
		}
		if(context->start_timestamp > timestamp){
			pthread_mutex_unlock(&context->mutex);
			return;
		}

		const int64_t video_duration = timestamp - context->start_timestamp;

		if(context->audio_frames.size > sizeof(struct obs_audio_data)){
			struct obs_audio_data peek_audio;
			
			circlebuf_peek_front(&context->audio_frames, &peek_audio, sizeof(peek_audio));
			uint64_t previous = 0;
			struct obs_audio_info info;
			obs_get_audio_info(&info);
			const int64_t frames = context->video_frames.size / sizeof(struct obs_source_frame*);
			const int64_t frame_duration = (context->last_frame_timestamp - context->first_frame_timestamp)/frames;
			//const uint64_t duration = audio_frames_to_ns(info.samples_per_sec, peek_audio.frames);
			int64_t audio_duration = ((int64_t)peek_audio.timestamp - (int64_t)context->first_frame_timestamp) * 100 / context->speed_percent;
			while(context->play && video_duration + frame_duration > audio_duration && peek_audio.timestamp > previous)
			{
				previous = peek_audio.timestamp;
				circlebuf_pop_front(&context->audio_frames, NULL, sizeof(peek_audio));
				circlebuf_push_back(&context->audio_frames, &peek_audio, sizeof(peek_audio));
				if(peek_audio.timestamp > context->first_frame_timestamp - frame_duration && peek_audio.timestamp < context->last_frame_timestamp + frame_duration){
					context->audio.frames = peek_audio.frames;

					if(context->speed_percent != 100)
					{
						context->audio.timestamp = context->start_timestamp + (peek_audio.timestamp - context->first_frame_timestamp) * 100 / context->speed_percent;
						context->audio.samples_per_sec = info.samples_per_sec * context->speed_percent / 100;
					}else
					{
						context->audio.timestamp = peek_audio.timestamp + context->start_timestamp - context->first_frame_timestamp;
						context->audio.samples_per_sec = info.samples_per_sec;
					}
					for (size_t i = 0; i < MAX_AV_PLANES; i++) {
						context->audio.data[i] = peek_audio.data[i];
					}


					context->audio.speakers = info.speakers;
					context->audio.format = AUDIO_FORMAT_FLOAT_PLANAR;

					obs_source_output_audio(context->source, &context->audio);
				}
				circlebuf_peek_front(&context->audio_frames, &peek_audio, sizeof(peek_audio));
				audio_duration = ((int64_t)peek_audio.timestamp - (int64_t)context->first_frame_timestamp) * 100 / context->speed_percent;
			}
		}


		int64_t source_duration = (peek_frame->timestamp - context->first_frame_timestamp) * 100 / context->speed_percent;
		if(video_duration < source_duration){
			pthread_mutex_unlock(&context->mutex);
			return;
		}

		while(context->play && context->video_frames.size > sizeof(struct obs_source_frame*) && video_duration >= source_duration && (frame == NULL || frame->timestamp <= peek_frame->timestamp)){
			if(peek_frame->timestamp >= context->last_frame_timestamp - context->trim_end)
			{
				if(context->end_action != END_ACTION_LOOP)
				{
					context->play = false;
					context->end = true;
				}
				else if (context->trim_end != 0)
				{
					context->restart = true;
				}
				if(context->next_scene_name && context->active)
				{
					obs_source_t *s = obs_get_source_by_name(context->next_scene_name);
					if(s)
					{
						obs_frontend_set_current_scene(s);
						obs_source_release(s);
					}
				}
			}
			circlebuf_pop_front(&context->video_frames, &frame, sizeof(struct obs_source_frame*));

			circlebuf_push_back(&context->video_frames, &frame, sizeof(struct obs_source_frame*));

			circlebuf_peek_front(&context->video_frames, &peek_frame, sizeof(struct obs_source_frame*));
			source_duration = (peek_frame->timestamp - context->first_frame_timestamp) * 100 / context->speed_percent;
		}
		if(frame){
			const uint64_t t = frame->timestamp;
			frame->timestamp -= context->first_frame_timestamp;
			if(context->speed_percent != 100)
			{
				frame->timestamp = frame->timestamp * 100 / context->speed_percent;
			}
			frame->timestamp += context->start_timestamp;
			context->previous_frame_timestamp = frame->timestamp;
			obs_source_output_video(context->source, frame);
			frame->timestamp = t;
		}
	}else if(context->audio_frames.size)
	{
		//no video, only audio
		struct obs_audio_data peek_audio;
		
		circlebuf_peek_front(&context->audio_frames, &peek_audio, sizeof(peek_audio));
		if(context->first_frame_timestamp == peek_audio.timestamp)
		{
			context->start_timestamp = timestamp;
			context->pause_timestamp = 0;
			context->restart = false;
		}
		else if(context->restart)
		{
			while(peek_audio.timestamp != context->first_frame_timestamp)
			{
				circlebuf_pop_front(&context->audio_frames, NULL, sizeof(peek_audio));
				circlebuf_push_back(&context->audio_frames, &peek_audio, sizeof(peek_audio));
				circlebuf_peek_front(&context->audio_frames, &peek_audio, sizeof(peek_audio));
			}
			context->restart = false;
			context->start_timestamp = timestamp;
			context->pause_timestamp = 0;
		}
		if(context->start_timestamp == timestamp && context->trim_front != 0){
			if(context->speed_percent == 100){
				context->start_timestamp -= context->trim_front;
			}else{
				context->start_timestamp -= context->trim_front * 100 / context->speed_percent;
			}
			if(context->trim_front < 0){
				pthread_mutex_unlock(&context->mutex);
				return;
			}
			while(peek_audio.timestamp < context->first_frame_timestamp + context->trim_front)
			{
				circlebuf_pop_front(&context->audio_frames, NULL, sizeof(peek_audio));
				circlebuf_push_back(&context->audio_frames, &peek_audio, sizeof(peek_audio));
				circlebuf_peek_front(&context->audio_frames, &peek_audio, sizeof(peek_audio));
			}
		}
		if(context->start_timestamp > timestamp){
			pthread_mutex_unlock(&context->mutex);
			return;
		}

		const int64_t video_duration = timestamp - context->start_timestamp;
		uint64_t previous = 0;
		struct obs_audio_info info;
		obs_get_audio_info(&info);
		
		int64_t audio_duration = ((int64_t)peek_audio.timestamp - (int64_t)context->first_frame_timestamp) * 100 / context->speed_percent;
		while(context->play && context->audio_frames.size > sizeof(peek_audio) && video_duration >= audio_duration && peek_audio.timestamp > previous)
		{
			if(peek_audio.timestamp >= context->last_frame_timestamp - context->trim_end)
			{
				if(context->end_action != END_ACTION_LOOP)
				{
					context->play = false;
					context->end = true;
				}
				else if (context->trim_end != 0)
				{
					context->restart = true;
				}
				if(context->next_scene_name && context->active)
				{
					obs_source_t *s = obs_get_source_by_name(context->next_scene_name);
					if(s)
					{
						obs_frontend_set_current_scene(s);
						obs_source_release(s);
					}
				}
			}
			previous = peek_audio.timestamp;
			circlebuf_pop_front(&context->audio_frames, NULL, sizeof(peek_audio));
			circlebuf_push_back(&context->audio_frames, &peek_audio, sizeof(peek_audio));

			context->audio.frames = peek_audio.frames;

			if(context->speed_percent != 100)
			{
				context->audio.timestamp = context->start_timestamp + (peek_audio.timestamp - context->first_frame_timestamp) * 100 / context->speed_percent;
				context->audio.samples_per_sec = info.samples_per_sec * context->speed_percent / 100;
			}else
			{
				context->audio.timestamp = peek_audio.timestamp + context->start_timestamp - context->first_frame_timestamp;
				context->audio.samples_per_sec = info.samples_per_sec;
			}
			for (size_t i = 0; i < MAX_AV_PLANES; i++) {
				context->audio.data[i] = peek_audio.data[i];
			}


			context->audio.speakers = info.speakers;
			context->audio.format = AUDIO_FORMAT_FLOAT_PLANAR;

			obs_source_output_audio(context->source, &context->audio);
			circlebuf_peek_front(&context->audio_frames, &peek_audio, sizeof(peek_audio));
			audio_duration = ((int64_t)peek_audio.timestamp - (int64_t)context->first_frame_timestamp) * 100 / context->speed_percent;
		}
	}
	pthread_mutex_unlock(&context->mutex);
}
static bool EnumVideoSources(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	if((source->info.output_flags & OBS_SOURCE_VIDEO) != 0)
		obs_property_list_add_string(prop,obs_source_get_name(source),obs_source_get_name(source));
	return true;
}
static bool EnumAudioSources(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	if((source->info.output_flags & OBS_SOURCE_AUDIO) != 0)
		obs_property_list_add_string(prop,obs_source_get_name(source),obs_source_get_name(source));
	return true;
}
static bool EnumScenes(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	if(source->info.type == OBS_SOURCE_TYPE_SCENE)
		obs_property_list_add_string(prop,obs_source_get_name(source),obs_source_get_name(source));
	return true;
}
static bool replay_button(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct replay_source *s = data;
	replay_retrieve(s);
	return false; // no properties changed
}

static obs_properties_t *replay_source_properties(void *data)
{
	struct replay_source *s = data;

	obs_properties_t *props = obs_properties_create();
	obs_property_t* prop = obs_properties_add_list(props,SETTING_SOURCE,TEXT_SOURCE, OBS_COMBO_TYPE_EDITABLE,OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(EnumVideoSources, prop);
	prop = obs_properties_add_list(props,SETTING_SOURCE_AUDIO,TEXT_SOURCE_AUDIO, OBS_COMBO_TYPE_EDITABLE,OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(EnumAudioSources, prop);

	obs_properties_add_int(props,SETTING_DURATION,TEXT_DURATION,1,200,1);

	prop = obs_properties_add_list(props, SETTING_VISIBILITY_ACTION, "Visibility Action",
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, "Restart", VISIBILITY_ACTION_RESTART);
	obs_property_list_add_int(prop, "Pause", VISIBILITY_ACTION_PAUSE);
	obs_property_list_add_int(prop, "Continue", VISIBILITY_ACTION_CONTINUE);
	obs_property_list_add_int(prop, "None", VISIBILITY_ACTION_NONE);

	obs_properties_add_int(props, SETTING_START_DELAY,TEXT_START_DELAY,0,100000,1000);

	prop = obs_properties_add_list(props, SETTING_END_ACTION, "End Action",
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, "Hide", END_ACTION_HIDE);
	obs_property_list_add_int(prop, "Pause", END_ACTION_PAUSE);
	obs_property_list_add_int(prop, "Loop", END_ACTION_LOOP);

	prop = obs_properties_add_list(props,SETTING_NEXT_SCENE,TEXT_NEXT_SCENE, OBS_COMBO_TYPE_EDITABLE,OBS_COMBO_FORMAT_STRING);
	//obs_enum_scenes(EnumScenes, prop);

	obs_properties_add_int_slider(props, SETTING_SPEED,
			obs_module_text("SpeedPercentage"), 1, 200, 1);

	obs_properties_add_button(props,"replay_button","Get replay", replay_button);

	return props;
}

struct obs_source_info replay_source_info = {
	.id             = REPLAY_SOURCE_ID,
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO |
	                OBS_SOURCE_AUDIO |
	                OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name       = replay_source_get_name,
	.create         = replay_source_create,
	.destroy        = replay_source_destroy,
	.update         = replay_source_update,
	.get_defaults   = replay_source_defaults,
	.show           = replay_source_show,
	.hide           = replay_source_hide,
	.activate       = replay_source_active,
	.deactivate     = replay_source_deactive,
	.video_tick     = replay_source_tick,
	.get_properties = replay_source_properties
};



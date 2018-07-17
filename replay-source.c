#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <sys/stat.h>
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
	char          *source_name;
	long          duration;
	int           speed_percent;
	int           visibility_action;
	int           end_action;
	obs_hotkey_id replay_hotkey;
	obs_hotkey_id restart_hotkey;
	obs_hotkey_id pause_hotkey;
	obs_hotkey_id faster_hotkey;
	obs_hotkey_id slower_hotkey;
	obs_hotkey_id normal_speed_hotkey;
	obs_hotkey_id half_speed_hotkey;
	uint64_t      first_frame_timestamp;
	uint64_t      start_timestamp;
	uint64_t      last_frame_timestamp;
	uint64_t      previous_frame_timestamp;
	uint64_t      pause_timestamp;

	bool          play;
	bool          restart;
	bool          active;
	bool          end;
	
	/* contains struct obs_source_frame* */
	struct circlebuf               video_frames;

	/* stores the audio data */
	struct circlebuf               audio_frames;
	struct obs_audio_data          audio_output;
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
	if (strcmp("replay_filter", id) == 0 && strcmp(filterName, sourceName) == 0)
		c->source_filter = filter;

}


static void replay_source_update(void *data, obs_data_t *settings)
{
	struct replay_source *context = data;
	const char *source_name = obs_data_get_string(settings, "source");

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

	context->duration = (long)obs_data_get_int(settings, "duration");
	context->visibility_action = (int)obs_data_get_int(settings, "visibility_action");
	context->end_action = (int)obs_data_get_int(settings, "end_action");

	context->speed_percent = (int)obs_data_get_int(settings, "speed_percent");
	if (context->speed_percent < 1 || context->speed_percent > 200)
		context->speed_percent = 100;

	obs_source_t *s = obs_get_source_by_name(context->source_name);
	if(!s)
		return;

	context->source_filter = NULL;
	obs_source_enum_filters(s, EnumFilter, data);
	if(!context->source_filter)
	{
		context->source_filter = obs_source_create_private(REPLAY_FILTER_ID,obs_source_get_name(context->source), settings);
		if(context->source_filter){
			obs_source_filter_add(s,context->source_filter);
		}
	}else{
		obs_source_update(context->source_filter,settings);
	}

	obs_source_release(s);
}

static void replay_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings,SETTING_DURATION,5);
	obs_data_set_default_int(settings, "speed_percent", 100);
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

static void replay_retrive(struct replay_source *c)
{

	obs_source_t *s = obs_get_source_by_name(c->source_name);
	if(!s)
		return;
	
	c->source_filter = NULL;
	obs_source_enum_filters(s, EnumFilter, c);
	if(c->source_filter)
	{
		struct replay_filter* d = c->source_filter->context.data;
		if(d)
		{
			obs_source_t *parent = obs_filter_get_parent(d->src);
			pthread_mutex_lock(&parent->async_mutex);
			struct obs_source_frame *frame;
			while (c->video_frames.size) {
				circlebuf_pop_front(&c->video_frames, &frame, sizeof(struct obs_source_frame*));
				//obs_source_release_frame(c->source, frame);
				if (os_atomic_dec_long(&frame->refs) <= 0) {
					obs_source_frame_destroy(frame);
					frame = NULL;
				}
			}
			c->start_timestamp = obs_get_video_frame_time();
			c->pause_timestamp = 0;
			if(d->video_frames.size){
				circlebuf_peek_front(&d->video_frames, &frame, sizeof(struct obs_source_frame*));
				c->first_frame_timestamp = frame->timestamp;
				c->last_frame_timestamp = frame->timestamp;
			}
			while (d->video_frames.size) {
				circlebuf_pop_front(&d->video_frames, &frame, sizeof(struct obs_source_frame*));
				os_atomic_inc_long(&frame->refs);
				c->last_frame_timestamp = frame->timestamp;
				circlebuf_push_back(&c->video_frames, &frame, sizeof(struct obs_source_frame*));
			}
			pthread_mutex_unlock(&parent->async_mutex);
			if(c->active || c->visibility_action == VISIBILITY_ACTION_CONTINUE || c->visibility_action == VISIBILITY_ACTION_NONE)
			{
				c->play = true;
			}
		}
	}
	obs_source_release(s);
}

static void replay_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if(!pressed || !c->source_name)
		return;

	replay_retrive(c);
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

static void *replay_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct replay_source *context = bzalloc(sizeof(struct replay_source));
	context->source = source;

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

	return context;
}

static void replay_source_destroy(void *data)
{
	struct replay_source *context = data;

	if (context->source_name)
		bfree(context->source_name);

	while (context->video_frames.size) {
		struct obs_source_frame *frame;

		circlebuf_pop_front(&context->video_frames, &frame,
				sizeof(struct obs_source_frame*));

		if (os_atomic_dec_long(&frame->refs) <= 0) {
			obs_source_frame_destroy(frame);
			frame = NULL;
		}
	}
	//free_audio_packet(&context->audio_output);
	circlebuf_free(&context->video_frames);
	circlebuf_free(&context->audio_frames);
	bfree(context);
}

static void replay_source_tick(void *data, float seconds)
{
	struct replay_source *context = data;

	if(!context->video_frames.size){
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
	struct obs_source_frame *frame = NULL;
	struct obs_source_frame *peek_frame = NULL;
	circlebuf_peek_front(&context->video_frames, &peek_frame, sizeof(struct obs_source_frame*));
	const uint64_t timestamp = obs_get_video_frame_time();
	if(context->first_frame_timestamp == peek_frame->timestamp)
	{
		context->start_timestamp = timestamp;
		context->pause_timestamp = 0;
		context->restart = false;
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
	}
	uint64_t video_duration = timestamp - context->start_timestamp;
	uint64_t source_duration = (peek_frame->timestamp - context->first_frame_timestamp) * 100 / context->speed_percent ;
	if(video_duration < source_duration)
		return;

	while(context->play && context->video_frames.size && video_duration >= source_duration && (frame == NULL || frame->timestamp <= peek_frame->timestamp)){

		if(context->last_frame_timestamp == peek_frame->timestamp)
		{
			if(context->end_action != END_ACTION_LOOP)
			{
				context->play = false;
				context->end = true;
			}
		}
		circlebuf_pop_front(&context->video_frames, &frame, sizeof(struct obs_source_frame*));

		circlebuf_push_back(&context->video_frames, &frame, sizeof(struct obs_source_frame*));

		circlebuf_peek_front(&context->video_frames, &peek_frame, sizeof(struct obs_source_frame*));
		source_duration = (peek_frame->timestamp - context->first_frame_timestamp) * 100 / context->speed_percent;
	}
	if(frame){
		uint64_t t = frame->timestamp;
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
}
static bool EnumSources(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	if((source->info.output_flags | OBS_SOURCE_ASYNC) != 0)
		obs_property_list_add_string(prop,obs_source_get_name(source),obs_source_get_name(source));
	return true;
}

static bool replay_button(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct replay_source *s = data;
	replay_retrive(s);
	return false; // no properties changed
}

static obs_properties_t *replay_source_properties(void *data)
{
	struct replay_source *s = data;

	obs_properties_t *props = obs_properties_create();
	//obs_properties_add_text(props,"source","Source",OBS_TEXT_DEFAULT);
	obs_property_t* prop = obs_properties_add_list(props,SETTING_SOURCE,TEXT_SOURCE, OBS_COMBO_TYPE_EDITABLE,OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(EnumSources, prop);

	obs_properties_add_int(props,SETTING_DURATION,TEXT_DURATION,1,200,1);

	prop = obs_properties_add_list(props, "visibility_action", "Visibility Action",
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, "Restart", VISIBILITY_ACTION_RESTART);
	obs_property_list_add_int(prop, "Pause", VISIBILITY_ACTION_PAUSE);
	obs_property_list_add_int(prop, "Continue", VISIBILITY_ACTION_CONTINUE);
	obs_property_list_add_int(prop, "None", VISIBILITY_ACTION_NONE);

	prop = obs_properties_add_list(props, "end_action", "End Action",
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, "Hide", END_ACTION_HIDE);
	obs_property_list_add_int(prop, "Pause", END_ACTION_PAUSE);
	obs_property_list_add_int(prop, "Loop", END_ACTION_LOOP);

	obs_properties_add_int_slider(props, "speed_percent",
			obs_module_text("SpeedPercentage"), 1, 200, 1);

	obs_properties_add_button(props,"replay_button","Get replay", replay_button);

	return props;
}

struct obs_source_info replay_source_info = {
	.id             = REPLAY_SOURCE_ID,
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO |
//	                OBS_SOURCE_AUDIO |
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



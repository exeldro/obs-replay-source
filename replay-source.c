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

struct replay_source {
	obs_source_t  *source;
	obs_source_t  *source_filter;
	char          *source_name;
	long          duration;
	int           speed_percent;
	obs_hotkey_id replay_hotkey;
	uint64_t      first_frame_timestamp;
	uint64_t      start_timestamp;
	uint64_t      last_frame_timestamp;
	uint64_t      previous_frame_timestamp;
	bool          loop;
	
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
		obs_source_t *s = obs_get_source_by_name(context->source_name);
		if(s){
			context->source_filter = NULL;
			obs_source_enum_filters(s, EnumFilter, data);
			if(context->source_filter)
			{
				obs_source_filter_remove(s,context->source_filter);
			}
			obs_source_release(s);
		}
		bfree(context->source_name);
		context->source_name = bstrdup(source_name);
	}else{
		context->source_name = bstrdup(source_name);
	}

	context->duration = obs_data_get_int(settings, "duration");
	context->loop = obs_data_get_bool(settings, "loop");
	context->speed_percent = obs_data_get_int(settings, "speed_percent");
	if (context->speed_percent < 1 || context->speed_percent > 200)
		context->speed_percent = 100;

	obs_source_t *s = obs_get_source_by_name(context->source_name);
	if(!s)
		return;
	
	context->source_filter = obs_source_create_private(REPLAY_FILTER_ID,obs_source_get_name(context->source), settings);
	if(context->source_filter){
		obs_source_filter_add(s,context->source_filter);
	}

	obs_source_release(s);
}

static void replay_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings,SETTING_DURATION,5);
	obs_data_set_default_bool(settings,SETTING_LOOP,true);
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

static void replay_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(!pressed || !c->source_name)
		return;

	obs_source_t *s = obs_get_source_by_name(c->source_name);
	if(!s)
		return;
	
	c->source_filter = NULL;
	obs_source_enum_filters(s, EnumFilter, data);
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
				os_atomic_dec_long(&frame->refs);
			}
			c->start_timestamp = obs_get_video_frame_time();
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
		}
	}
	obs_source_release(s);
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

	if(context->video_frames.size){
		struct obs_source_frame *frame;
		struct obs_source_frame *peek_frame;
		circlebuf_peek_front(&context->video_frames, &peek_frame, sizeof(struct obs_source_frame*));
		const uint64_t timestamp = obs_get_video_frame_time();
		if(context->first_frame_timestamp == peek_frame->timestamp)
		{
			context->start_timestamp = timestamp;
		}
		uint64_t video_duration = timestamp - context->start_timestamp;
		uint64_t source_duration = (peek_frame->timestamp - context->first_frame_timestamp) * 100 / context->speed_percent ;
		if(video_duration < source_duration)
			return;

		while(context->video_frames.size && video_duration >= source_duration){

			circlebuf_pop_front(&context->video_frames, &frame, sizeof(struct obs_source_frame*));
			if(context->loop){
				struct obs_source_frame *new_frame = obs_source_frame_create(frame->format, frame->width, frame->height);
				new_frame->refs = 1;
				obs_source_frame_copy(new_frame, frame);
				circlebuf_push_back(&context->video_frames, &new_frame, sizeof(struct obs_source_frame*));
			}
			circlebuf_peek_front(&context->video_frames, &peek_frame, sizeof(struct obs_source_frame*));
			source_duration = (peek_frame->timestamp - context->first_frame_timestamp) * 100 / context->speed_percent;
			if(context->first_frame_timestamp == peek_frame->timestamp)
			{
				context->start_timestamp = timestamp;
				video_duration = timestamp - context->start_timestamp;
			}
		}
		if(context->speed_percent != 100)
		{
			frame->timestamp = frame->timestamp * 100 / context->speed_percent;
		}
		context->previous_frame_timestamp = frame->timestamp;
		obs_source_output_video(context->source, frame);
	}
	else
	{
		obs_source_output_video(context->source, NULL);
	}
}
static bool EnumSources(void *data, obs_source_t *source)
{
	obs_properties_t *prop = data;
	if((source->info.output_flags | OBS_SOURCE_ASYNC) != 0)
		obs_property_list_add_string(prop,obs_source_get_name(source),obs_source_get_name(source));
	return true;
}

static obs_properties_t *replay_source_properties(void *data)
{
	struct replay_source *s = data;

	obs_properties_t *props = obs_properties_create();
	//obs_properties_add_text(props,"source","Source",OBS_TEXT_DEFAULT);
	obs_properties_t* prop = obs_properties_add_list(props,SETTING_SOURCE,TEXT_SOURCE, OBS_COMBO_TYPE_EDITABLE,OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(EnumSources, prop);

	obs_properties_add_int(props,SETTING_DURATION,TEXT_DURATION,1,200,1);
	obs_properties_add_bool(props,SETTING_LOOP,TEXT_LOOP);
	obs_properties_add_int_slider(props, "speed_percent",
			obs_module_text("SpeedPercentage"), 1, 200, 1);

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
	.video_tick     = replay_source_tick,
	.get_properties = replay_source_properties
};



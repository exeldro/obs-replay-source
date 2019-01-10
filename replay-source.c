#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>
#include <media-io/video-scaler.h>
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
#define END_ACTION_REVERSE 3

enum saving_status
{
	SAVING_STATUS_NONE = 0,
	SAVING_STATUS_STARTING = 1,
	SAVING_STATUS_SAVING = 2,
	SAVING_STATUS_STOPPING = 3
};

struct replay_source {
	obs_source_t  *source;
	obs_source_t  *source_filter;
	obs_source_t  *source_audio_filter;
	char          *source_name;
	char          *source_audio_name;
	long          duration;
	int           speed_percent;
	bool          backward;
	bool          backward_start;
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
	obs_hotkey_id backward_hotkey;
	obs_hotkey_id save_hotkey;
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
	enum saving_status saving_status;
	
	/* contains struct obs_source_frame* */
	struct obs_source_frame**        video_frames;
	uint64_t                         video_frame_count;
	uint64_t                         video_frame_position;
	uint64_t                         video_save_position;

	/* stores the audio data */
	struct obs_audio_data*         audio_frames;
	uint64_t                       audio_frame_count;
	uint64_t                       audio_frame_position;
	struct obs_audio_data          audio_output;

	pthread_mutex_t    video_mutex;
	pthread_mutex_t    audio_mutex;

	uint32_t known_width;
	uint32_t known_height;

	video_t* video_output;
	obs_output_t* fileOutput;
	obs_encoder_t* h264Recording;
	audio_t* audio_t;
	video_scaler_t *scaler;
	bool lossless;
	char *file_format;
	char *directory;
	uint64_t start_save_timestamp;
	obs_encoder_t* aac;
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
static void EnumAudioVideoFilter(obs_source_t *source, obs_source_t *filter, void *data)
{
	struct replay_source *c = data;
	const char *filterName = obs_source_get_name(filter);
	const char *sourceName = obs_source_get_name(c->source);
	const char *id = obs_source_get_id(filter);
	if ((strcmp(REPLAY_FILTER_AUDIO_ID, id) == 0 || strcmp(REPLAY_FILTER_ASYNC_ID, id) == 0 || strcmp(REPLAY_FILTER_ID, id) == 0) && strcmp(filterName, sourceName) == 0)
		c->source_audio_filter = filter;
}

static void replay_backward_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if(pressed){
		const int64_t time = obs_get_video_frame_time();
		if(c->pause_timestamp)
		{
			c->start_timestamp += time - c->pause_timestamp;
			c->pause_timestamp = 0;
		}
		c->backward = !c->backward;
		c->play = true;
		if(c->end){
			c->end = false;
			if(c->backward && c->video_frame_count)
			{
				c->video_frame_position = c->video_frame_count - 1;
			}else
			{
				c->video_frame_position = 0;
			}
		}
		const int64_t duration = ((int64_t)c->last_frame_timestamp - (int64_t)c->first_frame_timestamp) * (int64_t)100 / (int64_t)c->speed_percent;
		int64_t play_duration = time - c->start_timestamp;
		if(play_duration > duration)
		{
			play_duration = duration;
		}
		c->start_timestamp = time - duration + play_duration;
	}
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

	context->backward_start = obs_data_get_bool(settings, SETTING_BACKWARD);
	if(context->backward != context->backward_start)
	{
		replay_backward_hotkey(context, 0, NULL, true);
	}
	

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
		obs_source_enum_filters(s, EnumAudioVideoFilter, data);
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
	const char *file_format = obs_data_get_string(settings, SETTING_FILE_FORMAT);
	if(context->file_format)
	{
		if(strcmp(context->file_format, file_format) != 0)
		{
			bfree(context->file_format);
			context->file_format = bstrdup(file_format);
		}
	}else{
		context->file_format = bstrdup(file_format);
	}
	context->lossless = obs_data_get_bool(settings, SETTING_LOSSLESS);
	const char *directory = obs_data_get_string(settings, SETTING_DIRECTORY);
	if(context->directory)
	{
		if(strcmp(context->directory, directory) != 0)
		{
			bfree(context->directory);
			context->directory = bstrdup(directory);
		}
	}else{
		context->directory = bstrdup(directory);
	}
}

static void replay_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_DURATION, 5);
	obs_data_set_default_int(settings, SETTING_SPEED, 100);
	obs_data_set_default_int(settings, SETTING_VISIBILITY_ACTION, VISIBILITY_ACTION_CONTINUE);
	obs_data_set_default_int(settings, SETTING_START_DELAY, 0);
	obs_data_set_default_int(settings, SETTING_END_ACTION, END_ACTION_LOOP);
	obs_data_set_default_bool(settings, SETTING_BACKWARD, false);
	obs_data_set_default_string(settings, SETTING_FILE_FORMAT, "%CCYY-%MM-%DD %hh.%mm.%ss");
	obs_data_set_default_bool(settings, SETTING_LOSSLESS, false);
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
				c->pause_timestamp = 0;
			}
		}
	}
}

static void InitFileOutputLossless(struct replay_source* context)
{
	context->fileOutput = obs_output_create("ffmpeg_output","replay_ffmpeg_output", NULL,NULL);
	obs_data_t *foSettings = obs_data_create();
	obs_data_set_string(foSettings, "format_name", "avi");
	obs_data_set_string(foSettings, "video_encoder", "utvideo");
	obs_data_set_string(foSettings, "audio_encoder", "pcm_s16le");
	obs_output_update(context->fileOutput, foSettings);
	obs_data_release(foSettings);
	obs_output_set_media(context->fileOutput, context->video_output, context->audio_t);
}

static void InitFileOutput(struct replay_source* context)
{
	context->fileOutput = obs_output_create("ffmpeg_muxer","replay_ffmpeg_output", NULL,NULL);
	context->h264Recording = obs_video_encoder_create("obs_x264","replay_h264_recording", NULL, NULL);
	obs_data_t *xsettings = obs_data_create();
	obs_data_set_int(xsettings, "crf", 23);
	obs_data_set_bool(xsettings, "use_bufsize", true);
	obs_data_set_string(xsettings, "rate_control", "CRF");
	obs_data_set_string(xsettings, "profile", "high");
	obs_data_set_string(xsettings, "preset", "veryfast");
	obs_encoder_update(context->h264Recording, xsettings);
	obs_data_release(xsettings);
	obs_encoder_set_video(context->h264Recording, context->video_output);
	obs_output_set_video_encoder(context->fileOutput, context->h264Recording);
	context->aac = obs_audio_encoder_create("ffmpeg_aac", "aac", NULL, 0, NULL);
	obs_encoder_set_audio(context->aac, context->audio_t);
	obs_output_set_audio_encoder(context->fileOutput, context->aac, 0);
}

static inline size_t convert_time_to_frames(size_t sample_rate, uint64_t t)
{
	return (size_t)(t * (uint64_t)sample_rate / 1000000000ULL);
}

bool audio_input_callback(void *param, uint64_t start_ts_in, uint64_t end_ts_in, uint64_t *out_ts,
		uint32_t mixers, struct audio_output_data *mixes){
	struct replay_source *context = param;

	*out_ts = start_ts_in;
		
	if(!context->audio_t)
		return true;

	
	if(!context->start_save_timestamp || start_ts_in < context->start_save_timestamp)
		return true;
	
	uint64_t end_timestamp = context->start_save_timestamp + context->last_frame_timestamp - context->first_frame_timestamp;
	if(start_ts_in > end_timestamp)
		return true;

	size_t channels = audio_output_get_channels(context->audio_t);
	size_t sample_rate = audio_output_get_sample_rate(context->audio_t);
	size_t audio_size = AUDIO_OUTPUT_FRAMES * sizeof(float);

	pthread_mutex_lock(&context->audio_mutex);

	uint64_t i = 0;
	uint64_t duration_start = start_ts_in - context->start_save_timestamp;
	uint64_t duration_end = end_ts_in - context->start_save_timestamp; 
	while(i < context->audio_frame_count && context->audio_frames[i].timestamp < context->first_frame_timestamp)
	{
		i++;
	}
	if(i == context->audio_frame_count){
		pthread_mutex_unlock(&context->audio_mutex);
		return true;
	}
	
	while(i < context->audio_frame_count && context->audio_frames[i].timestamp - context->first_frame_timestamp < duration_start)
	{
		i++;
	}
	if(i == context->audio_frame_count){
		pthread_mutex_unlock(&context->audio_mutex);
		return true;
	}
	if(i)
		i--;

	while(i < context->audio_frame_count && duration_end >= context->audio_frames[i].timestamp - context->first_frame_timestamp)
	{
		size_t total_floats = AUDIO_OUTPUT_FRAMES;
		size_t start_point = 0;
		size_t start_point2 = 0;
		if (context->audio_frames[i].timestamp - context->first_frame_timestamp > duration_start) {
			start_point = convert_time_to_frames(sample_rate,context->audio_frames[i].timestamp - context->first_frame_timestamp - duration_start);
			if (start_point >= AUDIO_OUTPUT_FRAMES){
				pthread_mutex_unlock(&context->audio_mutex);
				return true;
			}

			total_floats -= start_point;
		}else if (context->audio_frames[i].timestamp - context->first_frame_timestamp < duration_start) {
			start_point2 = convert_time_to_frames(sample_rate,duration_start - (context->audio_frames[i].timestamp - context->first_frame_timestamp));
			if (start_point2 >= AUDIO_OUTPUT_FRAMES){
				i++;
				continue;
			}
		}
		if(context->audio_frames[i].frames - start_point2 < total_floats)
		{
			total_floats = context->audio_frames[i].frames - start_point2;
		}

		for (size_t mix_idx = 0; mix_idx < MAX_AUDIO_MIXES; mix_idx++) {
			for (size_t ch = 0; ch < channels; ch++) {
				register float *mix = mixes[mix_idx].data[ch];
				register float *aud = context->audio_frames[i].data[ch];
				register float *end;

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
	if(context->video_frame_count == 0)
	{
		context->saving_status = SAVING_STATUS_NONE;
		return;
	}
	if(context->saving_status != SAVING_STATUS_NONE && context->saving_status != SAVING_STATUS_STARTING)
		return;

	pthread_mutex_lock(&context->video_mutex);

	const uint32_t width = context->video_frames[0]->width;
	const uint32_t height = context->video_frames[0]->height;
	if (context->known_width != width || context->known_height != height) {
		video_t* t = obs_get_video();
		const struct video_output_info* ovi = video_output_get_info(t);
		
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
		if(r != VIDEO_OUTPUT_SUCCESS){
			context->saving_status = SAVING_STATUS_NONE;
			pthread_mutex_unlock(&context->video_mutex);
			return;
		}

		context->known_width = width;
		context->known_height = height;

		if(context->scaler)
		{
			video_scaler_destroy(context->scaler);
			context->scaler = NULL;
		}

		struct video_scale_info ssi;
		ssi.format = context->video_frames[0]->format;
		ssi.colorspace = ovi->colorspace;
		ssi.range = ovi->range;
		ssi.width = context->video_frames[0]->width;
		ssi.height =  context->video_frames[0]->height;
		struct video_scale_info dsi;
		dsi.format = vi.format;
		dsi.colorspace = ovi->colorspace;
		dsi.range = ovi->range;
		dsi.height = height;
		dsi.width = width;
		video_scaler_create(&context->scaler, &dsi, &ssi,VIDEO_SCALE_DEFAULT);
	}
	if(!context->audio_t)
	{
		audio_t* t = obs_get_audio();
		const struct audio_output_info* oai = audio_output_get_info(t);
		struct audio_output_info oi ;
		oi.name = "ReplayAudio";
		oi.speakers = oai->speakers;
		oi.samples_per_sec = oai->samples_per_sec;
		oi.format = oai->format;
		oi.input_param = context;
		oi.input_callback = audio_input_callback;
		const int r = audio_output_open(&context->audio_t, &oi);
		if(r != AUDIO_OUTPUT_SUCCESS){
			context->saving_status = SAVING_STATUS_NONE;
			pthread_mutex_unlock(&context->video_mutex);
			return;
		}
	}
	if(!context->fileOutput){
		if(context->lossless){
			InitFileOutputLossless(context);
		}else{
			InitFileOutput(context);
		}
	}

	char *filename = os_generate_formatted_filename(context->lossless?"avi":"flv", true, context->file_format);
	struct dstr path={NULL,0,0};
	dstr_copy(&path, context->directory);
	dstr_replace(&path, "\\", "/");
	if (dstr_end(&path) != '/')
		dstr_cat_ch(&path, '/');
	dstr_cat(&path, filename);
	bfree(filename);
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", path.array);
	obs_data_set_string(settings, "url", path.array);

	obs_output_update(context->fileOutput, settings);
	dstr_free(&path);
	obs_data_release(settings);

	context->video_save_position = 0;
	context->start_save_timestamp = os_gettime_ns();

	struct obs_source_frame* frame = context->video_frames[0];
	struct video_frame output_frame;
	if (video_output_lock_frame(context->video_output, &output_frame, 1, context->start_save_timestamp))
	{
		video_scaler_scale(context->scaler, output_frame.data, output_frame.linesize, frame->data,frame->linesize);
		video_output_unlock_frame(context->video_output);
	}
	pthread_mutex_unlock(&context->video_mutex);
	if(!obs_output_start(context->fileOutput))
	{
		const char * error = obs_output_get_last_error(context->fileOutput);
		context->saving_status = SAVING_STATUS_NONE;
		return;
	}
	context->saving_status = SAVING_STATUS_SAVING;
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
		obs_source_enum_filters(as, EnumAudioVideoFilter, c);
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
	pthread_mutex_lock(&c->video_mutex);
	pthread_mutex_lock(&c->audio_mutex);
	for(uint64_t i = 0; i < c->video_frame_count; i++)
	{
		struct obs_source_frame* frame = *(c->video_frames + i);
		if (os_atomic_dec_long(&frame->refs) <= 0) {
			obs_source_frame_destroy(frame);
			frame = NULL;
		}
	}
	if(c->video_frames)
		bfree(c->video_frames);
	c->video_frames = NULL;
	c->video_frame_count = 0;
	c->video_frame_position = 0;
	c->video_save_position = 0;
	for(uint64_t i = 0; i < c->audio_frame_count; i++)
	{
		free_audio_packet(&c->audio_frames[i]);
	}
	if(c->audio_frames)
		bfree(c->audio_frames);
	c->audio_frames = NULL;
	c->audio_frame_position = 0;
	c->audio_frame_count = 0;
	c->start_timestamp = os_gettime_ns();
	if(c->saving_status != SAVING_STATUS_NONE)
	{
		c->start_save_timestamp = c->start_timestamp;
	}
	c->pause_timestamp = 0;
	c->backward = c->backward_start;
	if(c->backward){
		if(c->speed_percent == 100){
			c->trim_end = c->start_delay * -1;
		}else{
			c->trim_end = c->start_delay * c->speed_percent / -100;
		}
		c->trim_front = 0;
	}else{
		if(c->speed_percent == 100){
			c->trim_front = c->start_delay * -1;
		}else{
			c->trim_front = c->start_delay * c->speed_percent / -100;
		}
		c->trim_end = 0;
	}
	
	if(vf){
		struct obs_source_frame *frame;
		pthread_mutex_lock(&vf->mutex);
		if(vf->video_frames.size){
			circlebuf_peek_front(&vf->video_frames, &frame, sizeof(struct obs_source_frame*));
			c->first_frame_timestamp = frame->timestamp;
			c->last_frame_timestamp = frame->timestamp;
		}
		c->video_frame_count = vf->video_frames.size/sizeof(struct obs_source_frame*);
		c->video_frames = bzalloc(c->video_frame_count * sizeof(struct obs_source_frame*));
		for(uint64_t i = 0; i < c->video_frame_count; i++)
		{
			circlebuf_pop_front(&vf->video_frames, &frame, sizeof(struct obs_source_frame*));
			c->last_frame_timestamp = frame->timestamp;
			*(c->video_frames + i) = frame;
		}
		if(c->backward){
			c->video_frame_position = c->video_frame_count-1;
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
		c->audio_frame_count = af->audio_frames.size/sizeof(struct obs_audio_data);
		c->audio_frames = bzalloc(c->audio_frame_count * sizeof(struct obs_audio_data));
		for(uint64_t i = 0; i < c->audio_frame_count; i++)
		{
			circlebuf_pop_front(&af->audio_frames, &audio, sizeof(struct obs_audio_data));
			if(!vf){
				c->last_frame_timestamp = audio.timestamp;
			}
			memcpy(&c->audio_frames[i], &audio, sizeof(struct obs_audio_data));
			for (size_t j = 0; j < MAX_AV_PLANES; j++) {
				if (!audio.data[j])
					break;

				c->audio_frames[i].data[j] = bmemdup(audio.data[j], c->audio_frames[i].frames * sizeof(float));
			}
		}
		pthread_mutex_unlock(&af->mutex);
	}
	pthread_mutex_unlock(&c->audio_mutex);
	pthread_mutex_unlock(&c->video_mutex);
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

static void replay_save_hotkey(void *data, obs_hotkey_id id,
		obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if(!pressed)
		return;

	if(c->saving_status == SAVING_STATUS_NONE)
		c->saving_status = SAVING_STATUS_STARTING;
}

void update_speed(struct replay_source *c, int new_speed)
{
	if(new_speed < 1)
		new_speed = 1;

	if(new_speed == c->speed_percent)
		return;
	if(c->video_frame_count)
	{
		struct obs_source_frame *peek_frame = c->video_frames[c->video_frame_position];
		uint64_t duration = peek_frame->timestamp - c->first_frame_timestamp;
		if(c->backward)
		{
			duration = c->last_frame_timestamp - peek_frame->timestamp;
		}
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
	if(c->backward){
		duration = (c->last_frame_timestamp - c->first_frame_timestamp) - duration;
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
		int64_t duration = timestamp - c->start_timestamp;
		if(c->speed_percent != 100)
		{
			duration = duration * c->speed_percent / 100;
		}
		if(!c->backward){
			duration = (c->last_frame_timestamp - c->first_frame_timestamp) - duration;
		}
		if(c->first_frame_timestamp + c->trim_front < c->last_frame_timestamp - duration)
		{			
			c->trim_end = duration;
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
	pthread_mutex_init(&context->video_mutex, NULL);
	pthread_mutex_init(&context->audio_mutex, NULL);

	replay_source_update(context, settings);

	context->replay_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.Replay",
			"Load replay",
			replay_hotkey, context);

	context->save_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.Save",
			"Save replay",
			replay_save_hotkey, context);
	
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

	context->backward_hotkey = obs_hotkey_register_source(source,
			"ReplaySource.Backward",
			obs_module_text("Backward"),
			replay_backward_hotkey, context);

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

	if (context->directory)
		bfree(context->directory);

	if (context->file_format)
		bfree(context->file_format);

	if(context->h264Recording)
	{
		obs_encoder_release(context->h264Recording);
		context->h264Recording = NULL;
	}

	if(context->aac)
	{
		obs_encoder_release(context->aac);
		context->aac = NULL;
	}

	if(context->fileOutput){
		obs_output_release(context->fileOutput);
		context->fileOutput = NULL;
	}


	if(context->video_output){
		video_output_close(context->video_output);
		context->video_output = NULL;
	}

	if(context->audio_t){
		audio_output_close(context->audio_t);
		context->audio_t = NULL;
	}


	pthread_mutex_lock(&context->video_mutex);

	for(uint64_t i = 0; i < context->video_frame_count; i++)
	{
		struct obs_source_frame* frame = context->video_frames[i];
		if (os_atomic_dec_long(&frame->refs) <= 0) {
			obs_source_frame_destroy(frame);
			frame = NULL;
		}
	}
	context->video_frame_count = 0;
	if(context->video_frames)
		bfree(context->video_frames);

	pthread_mutex_lock(&context->audio_mutex);
	for(uint64_t i = 0; i < context->audio_frame_count; i++)
	{
		free_audio_packet(&context->audio_frames[i]);
	}
	context->audio_frame_count = 0;
	if(context->audio_frames)
		bfree(context->audio_frames);
	
	if(context->scaler)
		video_scaler_destroy(context->scaler);

	pthread_mutex_unlock(&context->video_mutex);
	pthread_mutex_unlock(&context->audio_mutex);
	pthread_mutex_destroy(&context->video_mutex);
	pthread_mutex_destroy(&context->audio_mutex);
	bfree(context);
}

static void replay_output_frame(struct replay_source* context, struct obs_source_frame* frame)
{
	uint64_t t = frame->timestamp;
	if(context->backward)
	{
		frame->timestamp = context->last_frame_timestamp - frame->timestamp;
	}else{
		frame->timestamp -= context->first_frame_timestamp;
	}
	if(context->speed_percent != 100)
	{
		frame->timestamp = frame->timestamp * 100 / context->speed_percent;
	}
	frame->timestamp += context->start_timestamp;
	if(context->previous_frame_timestamp <= frame->timestamp){
		context->previous_frame_timestamp = frame->timestamp;
		obs_source_output_video(context->source, frame);
	}
	frame->timestamp = t;
}

void replay_source_end_action(struct replay_source* context)
{
	if(context->end_action == END_ACTION_HIDE || context->end_action == END_ACTION_PAUSE)
	{
		context->play = false;
		context->end = true;
	}
	else if(context->end_action == END_ACTION_REVERSE)
	{
		replay_backward_hotkey(context,0,NULL,true);
	}else
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

static void replay_source_tick(void *data, float seconds)
{
	struct replay_source *context = data;

	if(context->saving_status == SAVING_STATUS_STARTING){
		replay_save(context);
	}else if(context->saving_status == SAVING_STATUS_SAVING)
	{
		if(!obs_output_active(context->fileOutput))
		{
			const char* error = obs_output_get_last_error(context->fileOutput);
			context->saving_status = SAVING_STATUS_NONE;
		}else if(context->video_save_position >= context->video_frame_count){
			context->saving_status = SAVING_STATUS_STOPPING;
			obs_output_stop(context->fileOutput);
		}else{
			pthread_mutex_lock(&context->video_mutex);
			struct obs_source_frame* frame = context->video_frames[context->video_save_position];
			while(frame->timestamp < context->first_frame_timestamp + context->trim_front)
			{
				context->video_save_position++;
				if(context->video_save_position >= context->video_frame_count)
				{
					context->saving_status = SAVING_STATUS_STOPPING;
					break;
				}
				frame = context->video_frames[context->video_save_position];
			}
			uint64_t timestamp = frame->timestamp;
			if(context->start_save_timestamp > context->first_frame_timestamp)
			{
				timestamp += context->start_save_timestamp - context->first_frame_timestamp;
			}
			if(timestamp <= os_gettime_ns()){
				struct video_frame output_frame;
				if (video_output_lock_frame(context->video_output, &output_frame, 1, timestamp))
				{
					video_scaler_scale(context->scaler, output_frame.data, output_frame.linesize, frame->data,frame->linesize);
					video_output_unlock_frame(context->video_output);
				}
				context->video_save_position++;
				if(context->video_save_position >= context->video_frame_count)
				{
					context->saving_status = SAVING_STATUS_STOPPING;
					obs_output_stop(context->fileOutput);
				}else
				{
					frame = context->video_frames[context->video_save_position];
					if(frame->timestamp > context->last_frame_timestamp - context->trim_end)
					{
						context->saving_status = SAVING_STATUS_STOPPING;
						obs_output_stop(context->fileOutput);
					}
				}
			}
			pthread_mutex_unlock(&context->video_mutex);
		}
	}else if(context->saving_status == SAVING_STATUS_STOPPING)
	{
		if(!context->fileOutput || !obs_output_active(context->fileOutput)){
			context->saving_status = SAVING_STATUS_NONE;
		}else{
			const uint64_t timestamp = os_gettime_ns();
			if (timestamp - context->start_save_timestamp > context->last_frame_timestamp - context->first_frame_timestamp)
			{
				obs_output_stop(context->fileOutput);
				pthread_mutex_lock(&context->video_mutex);
				if(context->video_save_position >= context->video_frame_count)
				{
					context->video_save_position = context->video_frame_count - 1;
				}
				struct obs_source_frame* frame = context->video_frames[context->video_save_position];
				struct video_frame output_frame;
				if (video_output_lock_frame(context->video_output, &output_frame, 1, timestamp))
				{
					video_scaler_scale(context->scaler, output_frame.data, output_frame.linesize, frame->data,frame->linesize);
					video_output_unlock_frame(context->video_output);
					
				}
				pthread_mutex_unlock(&context->video_mutex);
			}
		}
	}else if(context->fileOutput)
	{
		obs_output_release(context->fileOutput);
		context->fileOutput = NULL;
		if(context->h264Recording)
		{
			obs_encoder_release(context->h264Recording);
			context->h264Recording = NULL;
		}
		if(context->aac)
		{
			obs_encoder_release(context->aac);
			context->aac = NULL;
		}
	}

	if(!context->video_frame_count && !context->audio_frame_count){
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
	const uint64_t timestamp = os_gettime_ns();

	pthread_mutex_lock(&context->video_mutex);
	if(context->video_frame_count){
		if(context->video_frame_position >= context->video_frame_count)
			context->video_frame_position = context->video_frame_count - 1;
		struct obs_source_frame * frame = context->video_frames[context->video_frame_position];
		if(context->backward)
		{
			if(context->restart)
			{

				context->video_frame_position = context->video_frame_count-1;
				frame = context->video_frames[context->video_frame_position];
				context->start_timestamp = timestamp;
				context->restart = false;
				if(context->trim_end != 0)
				{
					if(context->speed_percent == 100){
						context->start_timestamp -= context->trim_end;
					}else{
						context->start_timestamp -= context->trim_end * 100 / context->speed_percent;
					}
				
					if(context->trim_end < 0){
						uint64_t t = frame->timestamp;
						frame->timestamp = timestamp;
						context->previous_frame_timestamp = frame->timestamp;
						obs_source_output_video(context->source, frame);
						frame->timestamp = t;
						pthread_mutex_unlock(&context->video_mutex);
						return;
					}
					while(frame->timestamp > context->last_frame_timestamp - context->trim_end)
					{
						if(context->video_frame_position == 0)
						{
							context->video_frame_position = context->video_frame_count;
						}
						context->video_frame_position--;
						frame = context->video_frames[context->video_frame_position];
					}
				}
			}
			
			const int64_t video_duration = timestamp - (int64_t)context->start_timestamp;
			//TODO audio backwards
			int64_t source_duration = (context->last_frame_timestamp - frame->timestamp) * 100 / context->speed_percent;

			struct obs_source_frame* output_frame = NULL;
			while(context->play && video_duration >= source_duration)
			{
				output_frame = frame;
				if(frame->timestamp <= context->first_frame_timestamp + context->trim_front)
				{
					replay_source_end_action(context);
				}
				if(context->video_frame_position == 0){
					replay_source_end_action(context);
					break;
				}
				context->video_frame_position--;
				
				frame = context->video_frames[context->video_frame_position];

				source_duration = (context->last_frame_timestamp - frame->timestamp) * 100 / context->speed_percent;
			}
			if(output_frame){
				replay_output_frame(context, output_frame);
			}else if(context->video_frame_position == 0){
				replay_source_end_action(context);
			}
		}else{
			if(context->restart)
			{
				context->video_frame_position = 0;
				context->restart = false;
				context->start_timestamp = timestamp;
				context->audio_frame_position = 0;
				frame = context->video_frames[context->video_frame_position];			
				if(context->trim_front != 0){
					if(context->speed_percent == 100){
						context->start_timestamp -= context->trim_front;
					}else{
						context->start_timestamp -= context->trim_front * 100 / context->speed_percent;
					}
					if(context->trim_front < 0){
						uint64_t t = frame->timestamp;
						frame->timestamp = timestamp;
						context->previous_frame_timestamp = frame->timestamp;
						obs_source_output_video(context->source, frame);
						frame->timestamp = t;
						pthread_mutex_unlock(&context->video_mutex);
						return;
					}
					while(frame->timestamp < context->first_frame_timestamp + context->trim_front)
					{
						context->video_frame_position++;
						if(context->video_frame_position >= context->video_frame_count)
						{
							context->video_frame_position = 0;
						}
						frame = context->video_frames[context->video_frame_position];
					}
				}
			}
			if(context->start_timestamp > timestamp){
				pthread_mutex_unlock(&context->video_mutex);
				return;
			}
			const int64_t video_duration = (int64_t)timestamp - (int64_t)context->start_timestamp;

			if(context->audio_frame_count > 1){
				pthread_mutex_lock(&context->audio_mutex);
				struct obs_audio_data peek_audio = context->audio_frames[context->audio_frame_position];
				struct obs_audio_info info;
				obs_get_audio_info(&info);
				const int64_t frame_duration = (context->last_frame_timestamp - context->first_frame_timestamp)/context->video_frame_count;
				//const uint64_t duration = audio_frames_to_ns(info.samples_per_sec, peek_audio.frames);
				int64_t audio_duration = ((int64_t)peek_audio.timestamp - (int64_t)context->first_frame_timestamp) * 100 / context->speed_percent;
				while(context->play && video_duration + frame_duration > audio_duration)
				{
					if(peek_audio.timestamp > context->first_frame_timestamp - frame_duration && peek_audio.timestamp < context->last_frame_timestamp + frame_duration){
						context->audio.frames = peek_audio.frames;

						if(context->speed_percent != 100)
						{
							context->audio.timestamp = context->start_timestamp + (((int64_t)peek_audio.timestamp - (int64_t)context->first_frame_timestamp) * 100 / context->speed_percent);
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
					context->audio_frame_position++;
					if(context->audio_frame_position >= context->audio_frame_count){
						context->audio_frame_position = 0;
						break;
					}
					peek_audio = context->audio_frames[context->audio_frame_position];
					audio_duration = ((int64_t)peek_audio.timestamp - (int64_t)context->first_frame_timestamp) * 100 / context->speed_percent;
				}
				pthread_mutex_unlock(&context->audio_mutex);
			}
			int64_t source_duration = (frame->timestamp - context->first_frame_timestamp) * 100 / context->speed_percent;
			struct obs_source_frame* output_frame = NULL;
			while(context->play && video_duration >= source_duration){
				output_frame = frame;
				if(frame->timestamp >= context->last_frame_timestamp - context->trim_end)
				{
					replay_source_end_action(context);
				}
				context->video_frame_position++;
				if(context->video_frame_position >= context->video_frame_count)
				{
					context->video_frame_position = context->video_frame_count - 1;
					replay_source_end_action(context);
					break;
				}
				frame = context->video_frames[context->video_frame_position];
				source_duration = (frame->timestamp - context->first_frame_timestamp) * 100 / context->speed_percent;
			}
			if(output_frame){
				replay_output_frame(context, output_frame);
			}else if(context->video_frame_position >= context->video_frame_count -1){
				replay_source_end_action(context);
			}
		}
	}else if(context->audio_frame_count)
	{
		//no video, only audio
		struct obs_audio_data peek_audio = context->audio_frames[context->audio_frame_position];
		
		if(context->first_frame_timestamp == peek_audio.timestamp)
		{
			context->start_timestamp = timestamp;
			context->pause_timestamp = 0;
			context->restart = false;
		}
		else if(context->restart)
		{
			context->audio_frame_position = 0;
			peek_audio = context->audio_frames[context->audio_frame_position];
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
				pthread_mutex_unlock(&context->video_mutex);
				return;
			}
			while(peek_audio.timestamp < context->first_frame_timestamp + context->trim_front)
			{
					context->audio_frame_position++;
					if(context->audio_frame_position >= context->audio_frame_count){
						context->audio_frame_position = 0;
						break;
					}
					peek_audio = context->audio_frames[context->audio_frame_position];
			}
		}
		if(context->start_timestamp > timestamp){
			pthread_mutex_unlock(&context->video_mutex);
			return;
		}

		const int64_t video_duration = timestamp - context->start_timestamp;
		struct obs_audio_info info;
		obs_get_audio_info(&info);
		
		int64_t audio_duration = ((int64_t)peek_audio.timestamp - (int64_t)context->first_frame_timestamp) * 100 / context->speed_percent;

		while(context->play && context->audio_frame_count > 1 && video_duration >= audio_duration)
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
			context->audio_frame_position++;
			if(context->audio_frame_position >= context->audio_frame_count){
				context->audio_frame_position = 0;
				break;
			}
			peek_audio = context->audio_frames[context->audio_frame_position];
			audio_duration = ((int64_t)peek_audio.timestamp - (int64_t)context->first_frame_timestamp) * 100 / context->speed_percent;
		}
	}
	pthread_mutex_unlock(&context->video_mutex);
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
	obs_enum_scenes(EnumVideoSources, prop);
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
	obs_property_list_add_int(prop, "Reverse", END_ACTION_REVERSE);

	prop = obs_properties_add_list(props,SETTING_NEXT_SCENE,TEXT_NEXT_SCENE, OBS_COMBO_TYPE_EDITABLE,OBS_COMBO_FORMAT_STRING);
	obs_enum_scenes(EnumScenes, prop);

	obs_properties_add_int_slider(props, SETTING_SPEED,
			obs_module_text("SpeedPercentage"), 1, 200, 1);
	obs_properties_add_bool(props, SETTING_BACKWARD,"Backwards");

	obs_properties_add_path(props,SETTING_DIRECTORY,"Directory",OBS_PATH_DIRECTORY,NULL,NULL);
	obs_properties_add_text(props,SETTING_FILE_FORMAT,"Filename Formatting",OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props,SETTING_LOSSLESS,"Lossless");

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



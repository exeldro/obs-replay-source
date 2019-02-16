#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>
#include <media-io/audio-resampler.h>
#include <util/circlebuf.h>
#include "replay.h"
#include "obs-internal.h"
#include <string.h>

#define TEXFORMAT GS_BGRA


static const char *replay_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_FILTER_NAME;
}

static inline void copy_frame_data_line(struct obs_source_frame *dst,
		const struct video_data*src, uint32_t plane, uint32_t y)
{
	const uint32_t pos_src = y * src->linesize[plane];
	const uint32_t pos_dst = y * dst->linesize[plane];
	const uint32_t bytes = dst->linesize[plane] < src->linesize[plane] ?
		dst->linesize[plane] : src->linesize[plane];

	memcpy(dst->data[plane] + pos_dst, src->data[plane] + pos_src, bytes);
}

void replay_filter_raw_video(void* data, struct video_data* frame)
{
	struct replay_filter *filter = data;

	if (!frame || !frame->data[0])
		return;

	struct obs_source_frame *output;

	struct obs_source_frame *new_frame = obs_source_frame_create(VIDEO_FORMAT_BGRA, filter->known_width, filter->known_height);
	new_frame->refs = 1;
	new_frame->timestamp = frame->timestamp;

	if (new_frame->linesize[0] != frame->linesize[0])
		for (uint32_t y = 0; y < filter->known_height; y++)
			copy_frame_data_line(new_frame, frame, 0, y);
	else
		memcpy(new_frame->data[0], frame->data[0],
				new_frame->linesize[0] * filter->known_height);

	pthread_mutex_lock(&filter->mutex);

	circlebuf_push_back(&filter->video_frames, &new_frame,
			sizeof(struct obs_source_frame*));

	
	circlebuf_peek_front(&filter->video_frames, &output,
			sizeof(struct obs_source_frame*));

	uint64_t cur_duration = frame->timestamp - output->timestamp;
	while (cur_duration > 0 && cur_duration > filter->duration){

		circlebuf_pop_front(&filter->video_frames, NULL,
			sizeof(struct obs_source_frame*));

		if (os_atomic_dec_long(&output->refs) <= 0) {
			obs_source_frame_destroy(output);
			output = NULL;
		}
		if(filter->video_frames.size){
			circlebuf_peek_front(&filter->video_frames, &output, sizeof(struct obs_source_frame*));
			cur_duration = frame->timestamp - output->timestamp;
		}
		else
		{
			cur_duration = 0;
		}
	}
	pthread_mutex_unlock(&filter->mutex);
}

void replay_filter_offscreen_render(void* data, uint32_t cx, uint32_t cy)
{
	struct replay_filter *filter = data;

	obs_source_t* target = obs_filter_get_target(filter->src);
	if (!target) {
		return;
	}

	const uint32_t width = obs_source_get_base_width(target);
	const uint32_t height = obs_source_get_base_height(target);

	gs_texrender_reset(filter->texrender);

	if (gs_texrender_begin(filter->texrender, width, height)) {
		struct vec4 background;
		vec4_zero(&background);

		gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		obs_source_video_render(target);

		gs_blend_state_pop();
		gs_texrender_end(filter->texrender);

		if (filter->known_width != width || filter->known_height != height) {

			gs_stagesurface_destroy(filter->stagesurface);
			filter->stagesurface =
				gs_stagesurface_create(width, height, TEXFORMAT);

			struct video_output_info vi = {0};
			vi.format = VIDEO_FORMAT_BGRA;
			vi.width = width;
			vi.height = height;
			vi.fps_den = filter->ovi.fps_den;
			vi.fps_num = filter->ovi.fps_num;
			vi.cache_size = 16;
			vi.colorspace = VIDEO_CS_DEFAULT;
			vi.range = VIDEO_RANGE_DEFAULT;
			vi.name = obs_source_get_name(filter->src);

			video_output_close(filter->video_output);
			video_output_open(&filter->video_output, &vi);
			video_output_connect(filter->video_output, NULL, replay_filter_raw_video, filter);

			filter->known_width = width;
			filter->known_height = height;
		}

		struct video_frame output_frame;
		if (filter->video_output && video_output_lock_frame(filter->video_output,
			&output_frame, 1, obs_get_video_frame_time()))
		{
			if (filter->video_data) {
				gs_stagesurface_unmap(filter->stagesurface);
				filter->video_data = NULL;
			}

			gs_stage_texture(filter->stagesurface,
							 gs_texrender_get_texture(filter->texrender));
			gs_stagesurface_map(filter->stagesurface,
								&filter->video_data, &filter->video_linesize);

			const uint32_t linesize = output_frame.linesize[0];
			for (uint32_t i = 0; i < filter->known_height; ++i) {
				const uint32_t dst_offset = linesize * i;
				const uint32_t src_offset = filter->video_linesize * i;
				memcpy(output_frame.data[0] + dst_offset,
					filter->video_data + src_offset,
					linesize);
			}

			video_output_unlock_frame(filter->video_output);
		}
	}
}

static void replay_filter_update(void *data, obs_data_t *settings)
{
	struct replay_filter *filter = data;

	obs_remove_main_render_callback(replay_filter_offscreen_render, filter);
	
	uint64_t new_duration = (uint64_t)obs_data_get_int(settings, SETTING_DURATION) * MSEC_TO_NSEC;

	if (new_duration < filter->duration){
		pthread_mutex_lock(&filter->mutex);
		free_video_data(filter);
		pthread_mutex_unlock(&filter->mutex);
	}

	filter->duration = new_duration;

	obs_add_main_render_callback(replay_filter_offscreen_render, filter);

	replay_filter_check(filter);

	obs_source_t * s = obs_get_source_by_name(obs_source_get_name(filter->src));
	if(s)
	{
		if(obs_data_get_bool(settings, SETTING_SOUND_TRIGGER) && !filter->trigger_threshold)
		{
			filter->threshold_data = (struct replay_source*)s->context.data;
			filter->trigger_threshold = replay_trigger_threshold;
		}
		obs_source_release(s);
	}else
	{
		obs_source_filter_remove(obs_filter_get_parent(filter->src),filter->src);
	}
}


static void *replay_filter_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(source);

	struct replay_filter *context = bzalloc(sizeof(struct replay_filter));
	context->src = source;
	pthread_mutex_init(&context->mutex, NULL);

	context->texrender = gs_texrender_create(TEXFORMAT, GS_ZS_NONE);
	context->video_data = NULL;
	obs_get_video_info(&context->ovi);
	obs_get_audio_info(&context->oai);
	context->last_check = obs_get_video_frame_time();


	replay_filter_update(context, settings);

	return context;
}

static void replay_filter_destroy(void *data)
{
	struct replay_filter *filter = data;

	obs_remove_main_render_callback(replay_filter_offscreen_render, filter);
	pthread_mutex_lock(&filter->mutex);
	video_output_close(filter->video_output);
	filter->video_output = NULL;

	gs_stagesurface_unmap(filter->stagesurface);
	gs_stagesurface_destroy(filter->stagesurface);
	gs_texrender_destroy(filter->texrender);

	free_video_data(filter);
	free_video_data(filter);
	pthread_mutex_unlock(&filter->mutex);
	circlebuf_free(&filter->video_frames);
	circlebuf_free(&filter->audio_frames);
	pthread_mutex_destroy(&filter->mutex);
	bfree(data);
}

static obs_properties_t *replay_filter_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
	
	obs_properties_add_int(props, SETTING_DURATION, TEXT_DURATION, SETTING_DURATION_MIN, SETTING_DURATION_MAX, 1000);

	return props;
}

static void replay_filter_remove(void *data, obs_source_t *parent)
{
	struct replay_filter *filter = data;

	obs_remove_main_render_callback(replay_filter_offscreen_render, filter);
	pthread_mutex_lock(&filter->mutex);
	free_video_data(filter);
	free_audio_data(filter);
	pthread_mutex_unlock(&filter->mutex);
}

void replay_filter_tick(void* data, float seconds)
{
	struct replay_filter *filter = data;
	obs_get_video_info(&filter->ovi);
	replay_filter_check(filter);
}

void replay_filter_video_render(void* data, gs_effect_t* effect)
{
	UNUSED_PARAMETER(effect);
	struct replay_filter *filter = data;
	obs_source_skip_video_filter(filter->src);
}

struct obs_source_info replay_filter_info = {
	.id             = REPLAY_FILTER_ID,
	.type           = OBS_SOURCE_TYPE_FILTER,
	.output_flags   = OBS_SOURCE_VIDEO,
	.create         = replay_filter_create,
	.destroy        = replay_filter_destroy,
	.update         = replay_filter_update,
	.get_name       = replay_filter_get_name,
	.get_properties = replay_filter_properties,
	.filter_remove  = replay_filter_remove,
	.video_tick     = replay_filter_tick,
	.filter_audio   = replay_filter_audio,
	.video_render	= replay_filter_video_render,
};

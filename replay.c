#include "replay.h"
#include "obs-internal.h"
#include "../../UI/obs-frontend-api/obs-frontend-api.h"

obs_properties_t *replay_filter_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
	
	obs_properties_add_int(props, SETTING_DURATION, TEXT_DURATION, 1, 200, 1);

	return props;
}

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
				sizeof(struct obs_source_frame*));

		if (os_atomic_dec_long(&frame->refs) <= 0) {
			obs_source_frame_destroy(frame);
			frame = NULL;
		}
	}
}
static inline uint64_t uint64_diff(uint64_t ts1, uint64_t ts2)
{
	return (ts1 < ts2) ?  (ts2 - ts1) : (ts1 - ts2);
}

struct obs_audio_data *replay_filter_audio(void *data,
		struct obs_audio_data *audio)
{
	struct replay_filter *filter = data;
	struct obs_audio_data cached = *audio;

	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		if (!audio->data[i])
			break;

		cached.data[i] = bmemdup(audio->data[i],
				audio->frames * sizeof(float));
	}
	const uint64_t timestamp = cached.timestamp;
	uint64_t adjusted_time = timestamp + filter->timing_adjust;
	const uint64_t os_time = os_gettime_ns();
	if(filter->timing_adjust && uint64_diff(os_time, timestamp) < MAX_TS_VAR)
	{
		adjusted_time = timestamp;
		filter->timing_adjust = 0;
	}else if(uint64_diff(os_time, adjusted_time) > MAX_TS_VAR)
	{
		filter->timing_adjust = os_time - timestamp;
		adjusted_time = os_time;
	}
	cached.timestamp = adjusted_time;

	pthread_mutex_lock(&filter->mutex);

	circlebuf_push_back(&filter->audio_frames, &cached, sizeof(cached));
	
	circlebuf_peek_front(&filter->audio_frames, &cached, sizeof(cached));

	uint64_t cur_duration = adjusted_time - cached.timestamp;
	while (filter->audio_frames.size > sizeof(cached) && cur_duration >= filter->duration + MAX_TS_VAR){

		circlebuf_pop_front(&filter->audio_frames, NULL, sizeof(cached));

		free_audio_packet(&cached);
		circlebuf_peek_front(&filter->audio_frames, &cached, sizeof(cached));
		cur_duration = adjusted_time - cached.timestamp;
	}
	pthread_mutex_unlock(&filter->mutex);
	return audio;
}

static inline void copy_frame_data_line(struct obs_source_frame *dst,
		const struct obs_source_frame *src, uint32_t plane, uint32_t y)
{
	const uint32_t pos_src = y * src->linesize[plane];
	const uint32_t pos_dst = y * dst->linesize[plane];
	const uint32_t bytes = dst->linesize[plane] < src->linesize[plane] ?
		dst->linesize[plane] : src->linesize[plane];

	memcpy(dst->data[plane] + pos_dst, src->data[plane] + pos_src, bytes);
}

static inline void copy_frame_data_plane(struct obs_source_frame *dst,
		const struct obs_source_frame *src,
		uint32_t plane, uint32_t lines)
{
	if (dst->linesize[plane] != src->linesize[plane])
		for (uint32_t y = 0; y < lines; y++)
			copy_frame_data_line(dst, src, plane, y);
	else
		memcpy(dst->data[plane], src->data[plane],
				dst->linesize[plane] * lines);
}

static void copy_frame_data_line_y800(uint32_t *dst, uint8_t *src, uint8_t *end)
{
	while (src < end) {
		register uint32_t val = *(src++);
		val |= (val << 8);
		val |= (val << 16);
		*(dst++) = val;
	}
}

static inline void copy_frame_data_y800(struct obs_source_frame *dst,
		const struct obs_source_frame *src)
{
	uint32_t *ptr_dst;
	uint8_t  *ptr_src;
	uint8_t  *src_end;

	if ((src->linesize[0] * 4) != dst->linesize[0]) {
		for (uint32_t cy = 0; cy < src->height; cy++) {
			ptr_dst = (uint32_t*)
				(dst->data[0] + cy * dst->linesize[0]);
			ptr_src = (src->data[0] + cy * src->linesize[0]);
			src_end = ptr_src + src->width;

			copy_frame_data_line_y800(ptr_dst, ptr_src, src_end);
		}
	} else {
		ptr_dst = (uint32_t*)dst->data[0];
		ptr_src = (uint8_t *)src->data[0];
		src_end = ptr_src + src->height * src->linesize[0];

		copy_frame_data_line_y800(ptr_dst, ptr_src, src_end);
	}
}

void copy_frame_data(struct obs_source_frame *dst,
		const struct obs_source_frame *src)
{
	dst->flip         = src->flip;
	dst->full_range   = src->full_range;
	dst->timestamp    = src->timestamp;
	memcpy(dst->color_matrix, src->color_matrix, sizeof(float) * 16);
	if (!dst->full_range) {
		size_t const size = sizeof(float) * 3;
		memcpy(dst->color_range_min, src->color_range_min, size);
		memcpy(dst->color_range_max, src->color_range_max, size);
	}

	switch (src->format) {
	case VIDEO_FORMAT_I420:
		copy_frame_data_plane(dst, src, 0, dst->height);
		copy_frame_data_plane(dst, src, 1, dst->height/2);
		copy_frame_data_plane(dst, src, 2, dst->height/2);
		break;

	case VIDEO_FORMAT_NV12:
		copy_frame_data_plane(dst, src, 0, dst->height);
		copy_frame_data_plane(dst, src, 1, dst->height/2);
		break;

	case VIDEO_FORMAT_I444:
		copy_frame_data_plane(dst, src, 0, dst->height);
		copy_frame_data_plane(dst, src, 1, dst->height);
		copy_frame_data_plane(dst, src, 2, dst->height);
		break;

	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_UYVY:
	case VIDEO_FORMAT_NONE:
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
		copy_frame_data_plane(dst, src, 0, dst->height);
		break;

	case VIDEO_FORMAT_Y800:
		copy_frame_data_y800(dst, src);
		break;
	}
}

void obs_source_frame_copy(struct obs_source_frame * dst,const struct obs_source_frame *src)
{
	copy_frame_data(dst, src);
}

void obs_enum_scenes(bool (*enum_proc)(void*, obs_source_t*), void *param)
{
	struct obs_frontend_source_list l ={NULL,0,0};
	obs_frontend_get_scenes(&l);
	for (size_t i = 0; i < l.sources.num; i++) {
		if(!enum_proc(param, l.sources.array[i]))
			break;
	}
	obs_frontend_source_list_free(&l);
}


OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("replay-source", "en-US")

extern struct obs_source_info replay_filter_info;
extern struct obs_source_info replay_filter_audio_info;
extern struct obs_source_info replay_filter_async_info;
extern struct obs_source_info replay_source_info;

bool obs_module_load(void)
{
	obs_register_source(&replay_source_info);
	obs_register_source(&replay_filter_info);
	obs_register_source(&replay_filter_audio_info);
	obs_register_source(&replay_filter_async_info);
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
	return "Replay source";
}


const char *obs_module_description(void)
{
	return "Plugin to (slow motion) instant replay sources from memory.";
}

const char *obs_module_author(void)
{
	return "Exeldro";
}

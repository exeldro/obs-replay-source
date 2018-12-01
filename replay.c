#include "replay.h"


static inline void copy_frame_data_line(struct obs_source_frame *dst,
		const struct obs_source_frame *src, uint32_t plane, uint32_t y)
{
	uint32_t pos_src = y * src->linesize[plane];
	uint32_t pos_dst = y * dst->linesize[plane];
	uint32_t bytes = dst->linesize[plane] < src->linesize[plane] ?
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


OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("replay-source", "en-US")

extern struct obs_source_info replay_filter_info;
extern struct obs_source_info replay_filter_async_info;
extern struct obs_source_info replay_source_info;

bool obs_module_load(void)
{
	obs_register_source(&replay_source_info);
	obs_register_source(&replay_filter_info);
	obs_register_source(&replay_filter_async_info);
	return true;
}

void free_audio_packet(struct obs_audio_data *audio)
{
	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		bfree(audio->data[i]);
	memset(audio, 0, sizeof(*audio));
}


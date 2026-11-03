/* Private implementation included by replay-source.c, sharing its player.
 * All functions run under state_mutex except the enqueue-only hotkey callback.
 * Lock order: state -> video -> audio; filter locks never call player code.
 * The commands mutex is only held while moving commands, never executing them. */

static struct replay_type *replay_type_find(struct replay_source *c, uint64_t id)
{
	for (size_t i = 0; i < c->types.num; i++)
		if (c->types.array[i]->id == id)
			return c->types.array[i];
	return NULL;
}

static void replay_disconnect_filter(obs_source_t *parent, obs_source_t *filter, struct replay_source *c)
{
	struct replay_filter *capture = obs_obj_get_data(filter);
	if (capture) {
		pthread_mutex_lock(&capture->mutex);
		if (capture->threshold_data == c) {
			capture->threshold_data = NULL;
			capture->trigger_threshold = NULL;
		}
		pthread_mutex_unlock(&capture->mutex);
	}
	obs_source_filter_remove(parent, filter);
}

static void replay_disconnect_filters(struct replay_source *c)
{
	const char *names[] = {c->source_name, c->source_audio_name};
	for (size_t i = 0; i < 2; i++) {
		obs_source_t *parent = names[i] ? obs_get_source_by_name(names[i]) : NULL;
		if (!parent)
			continue;
		c->source_audio_filter = NULL;
		obs_source_enum_filters(parent, EnumAudioVideoFilter, c);
		if (c->source_audio_filter)
			replay_disconnect_filter(parent, c->source_audio_filter, c);
		obs_source_release(parent);
	}
	c->source_filter = c->source_audio_filter = NULL;
}

static void replay_queue_clear(struct replay_source *c, replay_queue *queue)
{
	while (queue->size) {
		struct replay clip;
		circlebuf_pop_front(queue, &clip, sizeof clip);
		replay_free_replay(&clip, c);
	}
	circlebuf_free(queue);
}

static void replay_player_clear(struct replay_source *c)
{
	pthread_mutex_lock(&c->video_mutex);
	pthread_mutex_lock(&c->audio_mutex);
	memset(&c->current_replay, 0, sizeof c->current_replay);
	c->play = false;
	c->end = true;
	c->restart = false;
	c->stepped = false;
	c->replay_position = 0;
	c->video_frame_position = c->audio_frame_position = 0;
	replay_queue_clear(c, &c->replays);
	pthread_mutex_unlock(&c->audio_mutex);
	pthread_mutex_unlock(&c->video_mutex);
	obs_source_output_video(c->source, NULL);
	obs_source_media_ended(c->source);
}

static void replay_select_legacy(struct replay_source *c)
{
	if (!c->active_type_id)
		return;
	replay_player_clear(c);
	c->replays = c->legacy_replays;
	memset(&c->legacy_replays, 0, sizeof c->legacy_replays);
	c->replay_position = c->legacy_position;
	c->active_type_id = 0;
	c->type_reverse_order = false;
	replay_update_position(c, true);
}

static void replay_type_store(struct replay_source *c, struct replay_type *type)
{
	if (!type || c->disabled)
		return;
	struct replay clip;
	if (!replay_capture(c, &clip, false))
		return;
	circlebuf_push_back(&type->clips, &clip, sizeof clip);
	while (type->clips.size / sizeof clip > (size_t)c->type_max) {
		circlebuf_pop_front(&type->clips, &clip, sizeof clip);
		replay_free_replay(&clip, c);
	}
	blog(LOG_INFO, "[replay_source: '%s'] stored replay in '%s' (%zu clips)",
	     obs_source_get_name(c->source), type->name, type->clips.size / sizeof clip);
}

static void replay_type_play(struct replay_source *c, struct replay_type *type)
{
	if (!type || !type->clips.size || c->disabled)
		return;
	replay_queue snapshot = {0};
	for (size_t pos = 0; pos < type->clips.size; pos += sizeof(struct replay)) {
		struct replay clip;
		replay_copy(&clip, circlebuf_data(&type->clips, pos));
		circlebuf_push_back(&snapshot, &clip, sizeof clip);
	}
	if (c->active_type_id) {
		replay_player_clear(c);
	} else {
		c->legacy_replays = c->replays;
		c->legacy_position = c->replay_position;
	}
	c->replays = snapshot;
	c->active_type_id = type->id;
	c->type_reverse_order = false;
	c->replay_position = 0;
	replay_update_position(c, true);
	replay_restart(c);
	replay_switch_scene(c->load_switch_scene_name);
}

static void replay_type_clear(struct replay_source *c, struct replay_type *type)
{
	if (!type)
		return;
	for (size_t i = c->pending_stores.num; i > 0; i--)
		if (c->pending_stores.array[i - 1].type_id == type->id)
			da_erase(c->pending_stores, i - 1);
	replay_queue_clear(c, &type->clips);
	if (c->active_type_id == type->id) {
		replay_player_clear(c);
		replay_update_text(c);
		replay_update_progress_crop(c, 0);
	}
}

static void replay_type_remove_clip(struct replay_source *c, uint64_t id)
{
	struct replay_type *type = replay_type_find(c, c->active_type_id);
	if (!type)
		return;
	size_t count = type->clips.size / sizeof(struct replay);
	for (size_t i = 0; i < count; i++) {
		struct replay clip;
		circlebuf_pop_front(&type->clips, &clip, sizeof clip);
		if (clip.id == id)
			replay_free_replay(&clip, c);
		else
			circlebuf_push_back(&type->clips, &clip, sizeof clip);
	}
}

static void replay_type_sync_trim(struct replay_source *c)
{
	struct replay_type *type = replay_type_find(c, c->active_type_id);
	if (!type)
		return;
	for (size_t pos = 0; pos < type->clips.size; pos += sizeof(struct replay)) {
		struct replay *clip = circlebuf_data(&type->clips, pos);
		if (clip->id == c->current_replay.id) {
			clip->trim_front = c->current_replay.trim_front;
			clip->trim_end = c->current_replay.trim_end;
			break;
		}
	}
}

/* Explicit frame stepping keeps the player paused across clip boundaries. */
static void replay_type_step_boundary(struct replay_source *c, bool forward)
{
	int count = (int)(c->replays.size / sizeof(struct replay));
	int next = c->replay_position + (forward ? 1 : -1);
	if (next < 0 || next >= count) {
		if (c->end_action == END_ACTION_LOOP || c->end_action == END_ACTION_LOOP_ALL)
			next = forward ? 0 : count - 1;
		else
			next = c->replay_position;
	}
	bool changed = next != c->replay_position;
	c->replay_position = next;
	if (changed)
		replay_update_position(c, true);
	/* Step onto the first frame of the next clip or last frame of the previous
	 * clip. At a non-looping list edge, keep the nearest boundary frame. */
	bool first = changed ? forward : !forward;
	uint64_t now = obs_get_video_frame_time();
	int64_t offset = 0;
	c->video_frame_position = 0;
	if (c->current_replay.video_frame_count) {
		c->video_frame_position = first ? 0 : c->current_replay.video_frame_count - 1;
		uint64_t timestamp = c->current_replay.video_frames[c->video_frame_position]->timestamp;
		offset = c->backward ? (int64_t)(c->current_replay.last_frame_timestamp - timestamp) :
			(int64_t)(timestamp - c->current_replay.first_frame_timestamp);
	}
	c->start_timestamp = now - (uint64_t)(int64_t)(offset * 100.0 / c->speed_percent);
	c->play = false;
	c->end = false;
	c->stepped = c->current_replay.video_frame_count != 0;
	c->pause_timestamp = now;
}

/* Called with video_mutex held by tick, just like legacy end actions. */
static bool replay_type_end_action(struct replay_source *c)
{
	int count = (int)(c->replays.size / sizeof(struct replay));
	if (!count || !c->play)
		return false;
	int next = c->replay_position + (c->type_reverse_order ? -1 : 1);
	if (next >= 0 && next < count) {
		c->replay_position = next;
		replay_update_position(c, false);
		return false;
	}
	if (c->end_action == END_ACTION_LOOP || c->end_action == END_ACTION_LOOP_ALL) {
		c->replay_position = 0;
		c->type_reverse_order = false;
		replay_update_position(c, false);
		return false;
	}
	if (c->end_action == END_ACTION_REVERSE || c->end_action == END_ACTION_REVERSE_ALL) {
		c->type_reverse_order = !c->type_reverse_order;
		replay_reverse_hotkey(c, 0, NULL, true);
		if (!c->current_replay.video_frame_count)
			c->audio_frame_position = 0;
		return false;
	}
	c->play = false;
	c->end = true;
	obs_source_media_ended(c->source);
	return true;
}

/* Audio-only typed queues use the same list-wide end actions as video.
 * Output uses copied samples when reversed, preserving all stored snapshots. */
static void replay_type_audio_tick(struct replay_source *c, uint64_t now)
{
	if (c->restart) {
		replay_update_position(c, false);
		c->play = true;
	}
	if (now < c->start_timestamp)
		return;
	struct replay *clip = &c->current_replay;
	uint64_t rate = clip->oai.samples_per_sec;
	if (!rate)
		return;
	while (c->audio_frame_position < clip->audio_frame_count) {
		uint64_t index = c->backward ? clip->audio_frame_count - 1 - c->audio_frame_position : c->audio_frame_position;
		struct obs_audio_data *packet = &clip->audio_frames[index];
		int64_t packet_duration = (int64_t)((uint64_t)packet->frames * SEC_TO_NSEC / rate);
		int64_t offset = c->backward ? (int64_t)(clip->last_frame_timestamp - packet->timestamp) - packet_duration :
			(int64_t)(packet->timestamp - clip->first_frame_timestamp);
		int64_t due = (int64_t)c->start_timestamp + (int64_t)(offset * 100.0 / c->speed_percent);
		if (due > (int64_t)now)
			break;
		c->audio_frame_position++;
		if ((int64_t)(packet->timestamp - clip->first_frame_timestamp) < clip->trim_front ||
		    packet->timestamp >= clip->last_frame_timestamp - clip->trim_end)
			continue;
		struct obs_source_audio output = {0};
		output.frames = packet->frames;
		output.timestamp = (uint64_t)due;
		output.speakers = clip->oai.speakers;
		output.format = clip->oai.format;
		output.samples_per_sec = (uint32_t)(rate * c->speed_percent / 100.0);
		for (size_t plane = 0; plane < MAX_AV_PLANES; plane++) {
			if (!packet->data[plane])
				continue;
			if (c->backward) {
				float *samples = bmemdup(packet->data[plane], packet->frames * sizeof(float));
				for (uint32_t i = 0; i < packet->frames / 2; i++) {
					float swap = samples[i];
					samples[i] = samples[packet->frames - 1 - i];
					samples[packet->frames - 1 - i] = swap;
				}
				output.data[plane] = (uint8_t *)samples;
			} else {
				output.data[plane] = packet->data[plane];
			}
		}
		obs_source_output_audio(c->source, &output);
		if (c->backward)
			for (size_t plane = 0; plane < MAX_AV_PLANES; plane++)
				bfree((void *)output.data[plane]);
	}
	int64_t end = (int64_t)clip->duration - (c->backward ? clip->trim_front : clip->trim_end);
	if ((int64_t)(now - c->start_timestamp) >= (int64_t)(end * 100.0 / c->speed_percent))
		replay_source_end_action(c);
}

static void replay_enqueue_hotkey(struct replay_source *c, obs_hotkey_func callback, obs_hotkey_id id)
{
	struct replay_command command = { .callback = callback, .hotkey = id,
		.timestamp = obs_get_video_frame_time() };
	pthread_mutex_lock(&c->commands_mutex);
	da_push_back(c->commands, &command);
	pthread_mutex_unlock(&c->commands_mutex);
}

static void replay_type_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		replay_enqueue_hotkey(data, NULL, id);
}

static void replay_commands_process(struct replay_source *c, uint64_t now)
{
	DARRAY(struct replay_command) commands;
	da_init(commands);
	pthread_mutex_lock(&c->commands_mutex);
	da_move(commands, c->commands);
	pthread_mutex_unlock(&c->commands_mutex);
	for (size_t i = 0; i < commands.num; i++) {
		struct replay_command command = commands.array[i];
		if (command.callback) {
			command.callback(c, command.hotkey, NULL, true);
			continue;
		}
		for (size_t j = 0; j < c->types.num; j++) {
			struct replay_type *type = c->types.array[j];
			if (command.hotkey == type->store_hotkey) {
				command.type_id = type->id;
				command.timestamp += c->retrieve_delay;
				if (command.timestamp <= now)
					replay_type_store(c, type);
				else
					da_push_back(c->pending_stores, &command);
				break;
			} else if (command.hotkey == type->play_hotkey) {
				replay_type_play(c, type);
				break;
			} else if (command.hotkey == type->clear_hotkey) {
				replay_type_clear(c, type);
				break;
			}
		}
	}
	da_free(commands);
	for (size_t i = 0; i < c->pending_stores.num;) {
		struct replay_command command = c->pending_stores.array[i];
		if (command.timestamp > now) {
			i++;
			continue;
		}
		da_erase(c->pending_stores, i);
		replay_type_store(c, replay_type_find(c, command.type_id));
	}
}

static void replay_type_descriptions(struct replay_type *type)
{
	struct dstr text = {0};
	dstr_printf(&text, "%s: %s", obs_module_text("StoreReplayType"), type->name);
	obs_hotkey_set_description(type->store_hotkey, text.array);
	dstr_printf(&text, "%s: %s", obs_module_text("PlayReplayType"), type->name);
	obs_hotkey_set_description(type->play_hotkey, text.array);
	dstr_printf(&text, "%s: %s", obs_module_text("ClearReplayType"), type->name);
	obs_hotkey_set_description(type->clear_hotkey, text.array);
	dstr_free(&text);
}

static struct replay_type *replay_type_create(struct replay_source *c, uint64_t id, const char *name, obs_data_t *record)
{
	struct replay_type *type = bzalloc(sizeof *type);
	type->id = id;
	type->name = bstrdup(name);
	const char *actions[] = {"Store", "Play", "Clear"};
	const char *keys[] = {"store_bindings", "play_bindings", "clear_bindings"};
	obs_hotkey_id *hotkeys[] = {&type->store_hotkey, &type->play_hotkey, &type->clear_hotkey};
	for (size_t i = 0; i < 3; i++) {
		struct dstr key = {0};
		dstr_printf(&key, "ReplaySource.Type.%" PRIu64 ".%s", id, actions[i]);
		*hotkeys[i] = obs_hotkey_register_source(c->source, key.array, name, replay_type_hotkey, c);
		dstr_free(&key);
		obs_data_array_t *bindings = obs_data_get_array(record, keys[i]);
		if (bindings)
			obs_hotkey_load(*hotkeys[i], bindings);
		obs_data_array_release(bindings);
	}
	replay_type_descriptions(type);
	return type;
}

static void replay_type_destroy(struct replay_source *c, struct replay_type *type)
{
	obs_hotkey_unregister(type->store_hotkey);
	obs_hotkey_unregister(type->play_hotkey);
	obs_hotkey_unregister(type->clear_hotkey);
	replay_type_clear(c, type);
	if (c->active_type_id == type->id) {
		c->active_type_id = 0;
		c->replays = c->legacy_replays;
		memset(&c->legacy_replays, 0, sizeof c->legacy_replays);
		c->replay_position = c->legacy_position;
		/* Restore a valid selection, but leave the legacy player stopped. */
		replay_update_position(c, true);
		c->play = false;
		c->end = true;
		c->pause_timestamp = obs_get_video_frame_time();
	}
	bfree(type->name);
	bfree(type);
}

static void replay_types_update(struct replay_source *c, obs_data_t *settings)
{
	DARRAY(struct replay_type *) next;
	da_init(next);
	obs_data_array_t *records = obs_data_get_array(settings, SETTING_REPLAY_TYPES);
	uint64_t next_id = (uint64_t)obs_data_get_int(settings, SETTING_REPLAY_TYPE_NEXT_ID);
	if (!next_id || next_id > INT64_MAX)
		next_id = 1;
	for (size_t i = 0; i < obs_data_array_count(records); i++) {
		obs_data_t *record = obs_data_array_item(records, i);
		int64_t id = obs_data_get_int(record, "id");
		if (id > 0 && (uint64_t)id >= next_id && id < INT64_MAX)
			next_id = (uint64_t)id + 1;
		obs_data_release(record);
	}
	for (size_t i = 0; i < obs_data_array_count(records); i++) {
		obs_data_t *record = obs_data_array_item(records, i);
		const char *name = obs_data_get_string(record, "name");
		bool valid = name && *name;
		for (size_t j = 0; j < next.num; j++)
			if (strcmp(next.array[j]->name, name) == 0)
				valid = false;
		if (!valid) {
			obs_data_release(record);
			continue;
		}
		int64_t raw_id = obs_data_get_int(record, "id");
		uint64_t id = raw_id > 0 && raw_id < INT64_MAX ? (uint64_t)raw_id : 0;
		for (size_t j = 0; j < next.num; j++)
			if (next.array[j]->id == id)
				id = 0;
		if (!id)
			id = next_id++;
		obs_data_set_int(record, "id", (long long)id);
		struct replay_type *type = replay_type_find(c, id);
		if (!type) {
			type = replay_type_create(c, id, name, record);
		} else if (strcmp(type->name, name) != 0) {
			bfree(type->name);
			type->name = bstrdup(name);
			replay_type_descriptions(type);
		}
		da_push_back(next, &type);
		obs_data_release(record);
	}
	obs_data_array_release(records);
	for (size_t i = 0; i < c->types.num; i++) {
		bool kept = false;
		for (size_t j = 0; j < next.num; j++)
			if (next.array[j] == c->types.array[i])
				kept = true;
		if (!kept)
			replay_type_destroy(c, c->types.array[i]);
	}
	da_free(c->types);
	da_move(c->types, next);
	int maximum = (int)obs_data_get_int(settings, SETTING_REPLAY_TYPE_MAX);
	c->type_max = maximum < 1 ? 1 : (maximum > 10 ? 10 : maximum);
	for (size_t i = 0; i < c->types.num; i++) {
		while (c->types.array[i]->clips.size / sizeof(struct replay) > (size_t)c->type_max) {
			struct replay clip;
			circlebuf_pop_front(&c->types.array[i]->clips, &clip, sizeof clip);
			replay_free_replay(&clip, c);
		}
	}
	obs_data_set_int(settings, SETTING_REPLAY_TYPE_NEXT_ID, (long long)next_id);
}

static void replay_types_save(void *data, obs_data_t *settings)
{
	struct replay_source *c = data;
	obs_data_array_t *records = obs_data_array_create();
	for (size_t i = 0; i < c->types.num; i++) {
		struct replay_type *type = c->types.array[i];
		obs_data_t *record = obs_data_create();
		obs_data_set_int(record, "id", (long long)type->id);
		obs_data_set_string(record, "name", type->name);
		const char *keys[] = {"store_bindings", "play_bindings", "clear_bindings"};
		obs_hotkey_id hotkeys[] = {type->store_hotkey, type->play_hotkey, type->clear_hotkey};
		for (size_t j = 0; j < 3; j++) {
			obs_data_array_t *bindings = obs_hotkey_save(hotkeys[j]);
			obs_data_set_array(record, keys[j], bindings);
			obs_data_array_release(bindings);
		}
		obs_data_array_push_back(records, record);
		obs_data_release(record);
	}
	obs_data_set_array(settings, SETTING_REPLAY_TYPES, records);
	obs_data_array_release(records);
}

static void replay_types_destroy(struct replay_source *c)
{
	for (size_t i = 0; i < c->types.num; i++)
		replay_type_destroy(c, c->types.array[i]);
	da_free(c->types);
	da_free(c->pending_stores);
	da_free(c->commands);
	replay_queue_clear(c, &c->legacy_replays);
}

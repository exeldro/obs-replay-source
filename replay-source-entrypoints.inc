/* Serialize OBS entry points with hotkey work performed on video_tick.
 * The recursive mutex also permits OBS signal handlers to query media state. */
#define LOCKED_VOID(name) \
static void name##_locked(void *data) \
{ \
	struct replay_source *c = data; \
	pthread_mutex_lock(&c->state_mutex); \
	name(data); \
	pthread_mutex_unlock(&c->state_mutex); \
}

#define LOCKED_ARG(name, type, arg) \
static void name##_locked(void *data, type arg) \
{ \
	struct replay_source *c = data; \
	pthread_mutex_lock(&c->state_mutex); \
	name(data, arg); \
	pthread_mutex_unlock(&c->state_mutex); \
}

#define LOCKED_RESULT(name, type) \
static type name##_locked(void *data) \
{ \
	struct replay_source *c = data; \
	if (!c) return (type)0; \
	pthread_mutex_lock(&c->state_mutex); \
	type result = name(data); \
	pthread_mutex_unlock(&c->state_mutex); \
	return result; \
}

LOCKED_VOID(replay_source_show)
LOCKED_VOID(replay_source_hide)
LOCKED_VOID(replay_source_active)
LOCKED_VOID(replay_source_deactive)
LOCKED_VOID(replay_next)
LOCKED_VOID(replay_previous)
LOCKED_VOID(replay_restart)
LOCKED_VOID(replay_stop)
LOCKED_ARG(replay_source_update, obs_data_t *, settings)
LOCKED_ARG(replay_types_save, obs_data_t *, settings)
LOCKED_ARG(replay_source_tick, float, seconds)
LOCKED_ARG(replay_play_pause, bool, pause)
LOCKED_ARG(replay_set_time, int64_t, seconds)
LOCKED_RESULT(replay_get_duration, int64_t)
LOCKED_RESULT(replay_get_time, int64_t)
LOCKED_RESULT(replay_get_state, enum obs_media_state)

static obs_properties_t *replay_source_properties_locked(void *data)
{
	struct replay_source *c = data;
	if (!c)
		return replay_source_properties(NULL);
	pthread_mutex_lock(&c->state_mutex);
	obs_properties_t *props = replay_source_properties(data);
	pthread_mutex_unlock(&c->state_mutex);
	return props;
}

#undef LOCKED_VOID
#undef LOCKED_ARG
#undef LOCKED_RESULT

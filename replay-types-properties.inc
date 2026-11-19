/* Native OBS properties for editing the list without changing stable IDs. */
static void replay_types_fill_selector(struct replay_source *c, obs_properties_t *props)
{
	obs_property_t *selector = obs_properties_get(props, SETTING_REPLAY_TYPE_SELECTED);
	obs_property_list_clear(selector);
	obs_property_list_add_int(selector, obs_module_text("SelectReplayType"), 0);
	for (size_t i = 0; i < c->types.num; i++)
		obs_property_list_add_int(selector, c->types.array[i]->name, (long long)c->types.array[i]->id);
}

static bool replay_type_selected(void *priv, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	struct replay_source *c = priv;
	pthread_mutex_lock(&c->state_mutex);
	struct replay_type *type = replay_type_find(c, (uint64_t)obs_data_get_int(settings, SETTING_REPLAY_TYPE_SELECTED));
	if (type)
		obs_data_set_string(settings, SETTING_REPLAY_TYPE_NAME, type->name);
	const char *buttons[] = {"replay_type_rename", "replay_type_remove", "replay_type_up", "replay_type_down"};
	for (size_t i = 0; i < 4; i++)
		obs_property_set_enabled(obs_properties_get(props, buttons[i]), type != NULL);
	obs_property_set_visible(obs_properties_get(props, "replay_type_error"), false);
	pthread_mutex_unlock(&c->state_mutex);
	return true;
}

static bool replay_type_edit(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct replay_source *c = data;
	pthread_mutex_lock(&c->state_mutex);
	obs_data_t *settings = obs_source_get_settings(c->source);
	const char *action = obs_property_name(property);
	struct replay_type *type = replay_type_find(c, (uint64_t)obs_data_get_int(settings, SETTING_REPLAY_TYPE_SELECTED));
	const char *raw = obs_data_get_string(settings, SETTING_REPLAY_TYPE_NAME);
	char *name = bstrdup(raw);
	strdepad(name);
	bool add = strcmp(action, "replay_type_add") == 0;
	bool rename = strcmp(action, "replay_type_rename") == 0;
	bool valid = *name != 0;
	if (add || rename) {
		for (size_t i = 0; i < c->types.num; i++)
			if (strcmp(c->types.array[i]->name, name) == 0 && (add || c->types.array[i] != type))
				valid = false;
		if (!valid || (!add && !type)) {
			obs_property_set_visible(obs_properties_get(props, "replay_type_error"), true);
			bfree(name);
			obs_data_release(settings);
			pthread_mutex_unlock(&c->state_mutex);
			return true;
		}
	}
	if (add) {
		uint64_t id = (uint64_t)obs_data_get_int(settings, SETTING_REPLAY_TYPE_NEXT_ID);
		if (!id || id >= INT64_MAX)
			id = 1;
		while (replay_type_find(c, id))
			id++;
		obs_data_t *record = obs_data_create();
		type = replay_type_create(c, id, name, record);
		obs_data_release(record);
		da_push_back(c->types, &type);
		obs_data_set_int(settings, SETTING_REPLAY_TYPE_NEXT_ID, (long long)id + 1);
		obs_data_set_int(settings, SETTING_REPLAY_TYPE_SELECTED, (long long)id);
	} else if (rename && type) {
		bfree(type->name);
		type->name = bstrdup(name);
		replay_type_descriptions(type);
	} else if (type) {
		for (size_t i = 0; i < c->types.num; i++) {
			if (c->types.array[i] != type)
				continue;
			if (strcmp(action, "replay_type_remove") == 0) {
				da_erase(c->types, i);
				replay_type_destroy(c, type);
				obs_data_set_int(settings, SETTING_REPLAY_TYPE_SELECTED, 0);
			} else if (strcmp(action, "replay_type_up") == 0 && i > 0) {
				c->types.array[i] = c->types.array[i - 1];
				c->types.array[i - 1] = type;
			} else if (strcmp(action, "replay_type_down") == 0 && i + 1 < c->types.num) {
				c->types.array[i] = c->types.array[i + 1];
				c->types.array[i + 1] = type;
			}
			break;
		}
	}
	bfree(name);
	replay_types_save(c, settings);
	replay_types_fill_selector(c, props);
	replay_type_selected(c, props, NULL, settings);
	obs_data_release(settings);
	pthread_mutex_unlock(&c->state_mutex);
	return true;
}

static void replay_types_properties(struct replay_source *c, obs_properties_t *props)
{
	obs_properties_t *group = obs_properties_create();
	obs_properties_add_group(props, "replay_types_group", obs_module_text("ReplayTypes"), OBS_GROUP_NORMAL, group);
	obs_properties_add_text(group, "replay_types_help", obs_module_text("ReplayTypesHelp"), OBS_TEXT_INFO);
	obs_property_t *selector = obs_properties_add_list(group, SETTING_REPLAY_TYPE_SELECTED, obs_module_text("ReplayType"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_modified_callback2(selector, replay_type_selected, c);
	obs_properties_add_text(group, SETTING_REPLAY_TYPE_NAME, obs_module_text("ReplayTypeName"), OBS_TEXT_DEFAULT);
	obs_properties_add_button2(group, "replay_type_add", obs_module_text("AddReplayType"), replay_type_edit, c);
	obs_properties_add_button2(group, "replay_type_rename", obs_module_text("RenameReplayType"), replay_type_edit, c);
	obs_properties_add_button2(group, "replay_type_remove", obs_module_text("RemoveReplayType"), replay_type_edit, c);
	obs_properties_add_button2(group, "replay_type_up", obs_module_text("MoveReplayTypeUp"), replay_type_edit, c);
	obs_properties_add_button2(group, "replay_type_down", obs_module_text("MoveReplayTypeDown"), replay_type_edit, c);
	obs_property_t *error = obs_properties_add_text(group, "replay_type_error", obs_module_text("ReplayTypeNameError"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(error, OBS_TEXT_INFO_ERROR);
	obs_property_set_visible(error, false);
	obs_properties_add_int(group, SETTING_REPLAY_TYPE_MAX, obs_module_text("MaxClipsPerType"), 1, 10, 1);
	replay_types_fill_selector(c, props);
}

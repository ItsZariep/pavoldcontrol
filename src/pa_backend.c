#include "pa_backend.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static void refresh_all(App *app);

static gboolean is_own_stream(const pa_proplist *pl)
{
	const gchar *pid_str = pa_proplist_gets(pl, PA_PROP_APPLICATION_PROCESS_ID);
	if (!pid_str) return FALSE;
	return atoi(pid_str) == (int) getpid();
}

// Helpers //

static const gchar *stream_icon(const pa_proplist *pl)
{
	const gchar *icon = pa_proplist_gets(pl, PA_PROP_APPLICATION_ICON_NAME);
	if (!icon) icon = pa_proplist_gets(pl, PA_PROP_MEDIA_ICON_NAME);
	return icon ? icon : "audio-x-generic";
}

static GHashTable *rows_for(App *app, RowKind kind)
{
	switch (kind)
	{
		case ROW_PLAYBACK: return app->playback_rows;
		case ROW_RECORDING: return app->recording_rows;
		case ROW_OUTPUT_DEV: return app->sink_rows;
		case ROW_INPUT_DEV: return app->source_rows;
		case ROW_CARD: return app->card_rows;
	}
	return NULL;
}

static GtkWidget *box_for(App *app, RowKind kind)
{
	switch (kind)
	{
		case ROW_PLAYBACK: return app->playback_box;
		case ROW_RECORDING: return app->recording_box;
		case ROW_OUTPUT_DEV: return app->sinks_box;
		case ROW_INPUT_DEV: return app->sources_box;
		case ROW_CARD: return app->cards_box;
	}
	return NULL;
}

static GtkWidget *placeholder_for(App *app, RowKind kind)
{
	switch (kind)
	{
		case ROW_PLAYBACK: return app->playback_placeholder;
		case ROW_RECORDING: return app->recording_placeholder;
		case ROW_OUTPUT_DEV: return app->sinks_placeholder;
		case ROW_INPUT_DEV: return app->sources_placeholder;
		case ROW_CARD: return app->cards_placeholder;
	}
	return NULL;
}

static void update_placeholder(App *app, RowKind kind)
{
	GHashTable *table = rows_for(app, kind);
	GtkWidget *ph = placeholder_for(app, kind);
	if (ph) gtk_widget_set_visible(ph, g_hash_table_size(table) == 0);
}

static StreamRow *ensure_row(App *app, RowKind kind, guint32 index)
{
	GHashTable *table = rows_for(app, kind);
	StreamRow *row = g_hash_table_lookup(table, GUINT_TO_POINTER(index));
	if (!row)
	{
		row = row_new(app, kind, index, box_for(app, kind));
		g_hash_table_insert(table, GUINT_TO_POINTER(index), row);
		update_placeholder(app, kind);
	}
	return row;
}

static void remove_row(App *app, RowKind kind, guint32 index)
{
	g_hash_table_remove(rows_for(app, kind), GUINT_TO_POINTER(index));
	update_placeholder(app, kind);
}

// Meters //

static void meter_read_cb(pa_stream *s, size_t length, void *userdata)
{
	StreamRow *row = userdata;
	const void *data;

	if (pa_stream_peek(s, &data, &length) < 0) return;
	if (data && length >= sizeof(float))
	{
		const float *samples = data;
		float peak = samples[length / sizeof(float) - 1];
		if (peak < 0.0f) peak = 0.0f;
		if (peak > 1.0f) peak = 1.0f;

		if (row->kind == ROW_OUTPUT_DEV || row->kind == ROW_INPUT_DEV)
		{
			gboolean muted = row->mute_btn &&
				gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(row->mute_btn));
			if (muted)
			{
				peak = 0.0f;
			}
			else if (row->n_channels > 0)
			{
				pa_volume_t max_vol = 0;
				for (guint8 c = 0; c < row->n_channels; c++)
					if (row->volume.values[c] > max_vol)
						max_vol = row->volume.values[c];
				float factor = (float) max_vol / (float) PA_VOLUME_NORM;
				if (factor < 0.0f) factor = 0.0f;
				peak *= factor;
			}
		}

		if (row->meter)
			gtk_level_bar_set_value(GTK_LEVEL_BAR(row->meter), (double) peak);
	}
	if (length > 0) pa_stream_drop(s);
}

void pa_backend_meter_attach(App *app, StreamRow *row, RowKind kind,
	const gchar *monitor_source, guint32 sink_input_index)
{
	if (!app->pa_ctx || row->meter_stream) return;

	pa_sample_spec ss;
	ss.format = PA_SAMPLE_FLOAT32NE;
	ss.rate = 25;
	ss.channels = 1;

	pa_stream *s = pa_stream_new(app->pa_ctx, "peak-meter", &ss, NULL);
	if (!s) return;

	pa_stream_set_read_callback(s, meter_read_cb, row);

	pa_buffer_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.fragsize = sizeof(float);
	attr.maxlength = (uint32_t) -1;

	pa_stream_flags_t flags = PA_STREAM_DONT_MOVE | PA_STREAM_PEAK_DETECT |
		PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_INHIBIT_AUTO_SUSPEND;

	if (kind == ROW_PLAYBACK)
	{
		pa_stream_set_monitor_stream(s, sink_input_index);
	}

	if (pa_stream_connect_record(s, monitor_source, &attr, flags) < 0)
	{
		pa_stream_unref(s);
		return;
	}
	row->meter_stream = s;
}

void pa_backend_meter_detach(StreamRow *row)
{
	if (!row->meter_stream) return;
	pa_stream_disconnect(row->meter_stream);
	pa_stream_unref(row->meter_stream);
	row->meter_stream = NULL;
}

static gint choice_compare(gconstpointer a, gconstpointer b)
{
	const RowChoice *ca = a;
	const RowChoice *cb = b;
	return g_strcmp0(ca->label, cb->label);
}

static GList *choices_from_name_table(GHashTable *names)
{
	GList *out = NULL;
	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, names);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		out = g_list_prepend(out, row_choice_new((const gchar *) key, (const gchar *) value));
	}
	return g_list_sort(out, choice_compare);
}

static GList *choices_from_sink_ports(const pa_sink_info *info)
{
	GList *out = NULL;
	for (uint32_t i = 0; i < info->n_ports; i++)
	{
		out = g_list_append(out, row_choice_new(info->ports[i]->name, info->ports[i]->description));
	}
	return out;
}

static GList *choices_from_source_ports(const pa_source_info *info)
{
	GList *out = NULL;
	for (uint32_t i = 0; i < info->n_ports; i++)
	{
		out = g_list_append(out, row_choice_new(info->ports[i]->name, info->ports[i]->description));
	}
	return out;
}

static GList *choices_from_profiles(App *app, const pa_card_info *info)
{
	GList *out = NULL;
	for (uint32_t i = 0; i < info->n_profiles; i++)
	{
		pa_card_profile_info2 *p = info->profiles2[i];
		if (app->settings.hide_unavailable_profiles && p->available == 0)
			continue;
		out = g_list_append(out, row_choice_new(p->name, p->description));
	}
	return out;
}

static void sink_input_cb(pa_context *c, const pa_sink_input_info *info, int eol,
	void *userdata)
{
	(void)c;
	App *app = userdata;
	if (eol || !info) return;

	StreamRow *row = ensure_row(app, ROW_PLAYBACK, info->index);
	const gchar *name = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
	if (!name) name = info->name;
	row_set_name(row, name, stream_icon(info->proplist));
	row_set_channels(row, info->channel_map.channels, &info->volume);
	row_set_mute(row, info->mute);

	row->is_virtual = (info->client == PA_INVALID_INDEX);
	gtk_widget_set_visible(row->root,
		app->settings.playback_filter == STREAM_FILTER_ALL ||
		(app->settings.playback_filter == STREAM_FILTER_APPLICATIONS && !row->is_virtual) ||
		(app->settings.playback_filter == STREAM_FILTER_VIRTUAL && row->is_virtual));

	const gchar *sink_name = g_hash_table_lookup(app->sink_index_to_name,
		GUINT_TO_POINTER(info->sink));
	GList *choices = choices_from_name_table(app->sink_names);
	row_set_choices(row, choices, sink_name);
	g_list_free_full(choices, row_choice_free);

	if (!row->meter_stream && sink_name)
	{
		pa_backend_meter_attach(app, row, ROW_PLAYBACK, NULL, info->index);
	}
}

static void source_output_cb(pa_context *c, const pa_source_output_info *info, int eol, void *userdata)
{
	(void)c;
	App *app = userdata;
	if (eol || !info) return;

	if (is_own_stream(info->proplist))
	{
		remove_row(app, ROW_RECORDING, info->index);
		return;
	}

	StreamRow *row = ensure_row(app, ROW_RECORDING, info->index);
	const gchar *name = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
	if (!name) name = info->name;
	row_set_name(row, name, stream_icon(info->proplist));
	row_set_channels(row, info->channel_map.channels, &info->volume);
	row_set_mute(row, info->mute);

	row->is_virtual = (info->client == PA_INVALID_INDEX);
	gtk_widget_set_visible(row->root,
		app->settings.recording_filter == STREAM_FILTER_ALL ||
		(app->settings.recording_filter == STREAM_FILTER_APPLICATIONS && !row->is_virtual) ||
		(app->settings.recording_filter == STREAM_FILTER_VIRTUAL && row->is_virtual));

	const gchar *source_name = g_hash_table_lookup(app->source_index_to_name,
		GUINT_TO_POINTER(info->source));
	GList *choices = choices_from_name_table(app->source_names);
	row_set_choices(row, choices, source_name);
	g_list_free_full(choices, row_choice_free);
}

static void sink_cb(pa_context *c, const pa_sink_info *info, int eol, void *userdata)
{
	(void)c;
	App *app = userdata;

	if (eol || !info)
	{
		return;
	}

	g_hash_table_insert(app->sink_names, g_strdup(info->name), g_strdup(info->description));
	g_hash_table_insert(app->sink_index_to_name, GUINT_TO_POINTER(info->index),
		g_strdup(info->name));

	StreamRow *row = ensure_row(app, ROW_OUTPUT_DEV, info->index);
	row_set_name(row, info->description, "audio-card");
	row_set_channels(row, info->channel_map.channels, &info->volume);
	row_set_mute(row, info->mute);
	row_set_device_name(row, info->name);
	row_set_preferred(row, g_strcmp0(info->name, app->default_sink_name) == 0);
	row_set_force_mono(row, app->settings.mono_output_devices);

	const gchar *active_port = info->active_port ? info->active_port->name : NULL;
	GList *choices = choices_from_sink_ports(info);
	row_set_choices(row, choices, active_port);
	g_list_free_full(choices, row_choice_free);

	row->is_hardware = (info->flags & PA_SINK_HARDWARE) != 0;
	gtk_widget_set_visible(row->root,
		app->settings.output_filter == OUTPUT_FILTER_ALL ||
		(app->settings.output_filter == OUTPUT_FILTER_HARDWARE && row->is_hardware) ||
		(app->settings.output_filter == OUTPUT_FILTER_VIRTUAL && !row->is_hardware));

	if (!row->meter_stream && app->settings.show_volume_meters)
	{
		pa_backend_meter_attach(app, row, ROW_OUTPUT_DEV, info->monitor_source_name, 0);
	}
	else if (row->meter_stream && !app->settings.show_volume_meters)
	{
		pa_backend_meter_detach(row);
	}
}

static void source_cb(pa_context *c, const pa_source_info *info, int eol, void *userdata)
{
	(void)c;
	App *app = userdata;
	if (eol || !info) return;

	g_hash_table_insert(app->source_names, g_strdup(info->name), g_strdup(info->description));
	g_hash_table_insert(app->source_index_to_name, GUINT_TO_POINTER(info->index),
		g_strdup(info->name));

	StreamRow *row = ensure_row(app, ROW_INPUT_DEV, info->index);
	row_set_name(row, info->description, "audio-input-microphone");
	row_set_channels(row, info->channel_map.channels, &info->volume);
	row_set_mute(row, info->mute);
	row_set_device_name(row, info->name);
	row_set_preferred(row, g_strcmp0(info->name, app->default_source_name) == 0);

	const gchar *active_port = info->active_port ? info->active_port->name : NULL;
	GList *choices = choices_from_source_ports(info);
	row_set_choices(row, choices, active_port);
	g_list_free_full(choices, row_choice_free);

	row->is_hardware = (info->flags & PA_SOURCE_HARDWARE) != 0;
	row->is_monitor = (info->monitor_of_sink != PA_INVALID_INDEX);

	gboolean visible;
	switch (app->settings.input_filter)
	{
		case INPUT_FILTER_ALL: visible = TRUE; break;
		case INPUT_FILTER_NO_MONITORS: visible = !row->is_monitor; break;
		case INPUT_FILTER_HARDWARE: visible = row->is_hardware && !row->is_monitor; break;
		case INPUT_FILTER_VIRTUAL: visible = !row->is_hardware && !row->is_monitor; break;
		case INPUT_FILTER_MONITORS: visible = row->is_monitor; break;
		default: visible = TRUE; break;
	}
	gtk_widget_set_visible(row->root, visible);

	if (!row->meter_stream && app->settings.show_volume_meters)
	{
		pa_backend_meter_attach(app, row, ROW_INPUT_DEV, info->name, 0);
	}
	else if (row->meter_stream && !app->settings.show_volume_meters)
	{
		pa_backend_meter_detach(row);
	}
}

static void card_cb(pa_context *c, const pa_card_info *info, int eol, void *userdata)
{
	(void)c;
	App *app = userdata;

	if (eol || !info)
	{
		return;
	}

	StreamRow *row = ensure_row(app, ROW_CARD, info->index);

	const gchar *desc = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_DESCRIPTION);
	row_set_name(row, desc ? desc : info->name, "audio-card");

	const gchar *active_profile = info->active_profile2 ? info->active_profile2->name : NULL;
	GList *choices = choices_from_profiles(app, info);
	row_set_profiles(row, choices, active_profile);
	g_list_free_full(choices, row_choice_free);
}

static void server_info_cb(pa_context *c, const pa_server_info *info, void *userdata)
{
	(void)c;
	App *app = userdata;
	g_free(app->default_sink_name);
	g_free(app->default_source_name);
	app->default_sink_name = g_strdup(info->default_sink_name);
	app->default_source_name = g_strdup(info->default_source_name);

	// re-run the sink/source queries so the "Preferred" toggles pick up the
	// (possibly changed) default device names
	pa_operation *o;
	if ((o = pa_context_get_sink_info_list(app->pa_ctx, sink_cb, app)))
		pa_operation_unref(o);
	if ((o = pa_context_get_source_info_list(app->pa_ctx, source_cb, app)))
		pa_operation_unref(o);
}

static void refresh_all(App *app)
{
	pa_operation *o;
	if ((o = pa_context_get_server_info(app->pa_ctx, server_info_cb, app)))
		pa_operation_unref(o);
	if ((o = pa_context_get_sink_info_list(app->pa_ctx, sink_cb, app)))
		pa_operation_unref(o);
	if ((o = pa_context_get_source_info_list(app->pa_ctx, source_cb, app)))
		pa_operation_unref(o);
	if ((o = pa_context_get_card_info_list(app->pa_ctx, card_cb, app)))
		pa_operation_unref(o);
	if ((o = pa_context_get_sink_input_info_list(app->pa_ctx, sink_input_cb, app)))
		pa_operation_unref(o);
	if ((o = pa_context_get_source_output_info_list(app->pa_ctx, source_output_cb, app)))
		pa_operation_unref(o);
}

void pa_backend_refresh_cards(App *app)
{
	if (!app->pa_ctx) return;
	pa_operation *o = pa_context_get_card_info_list(app->pa_ctx, card_cb, app);
	if (o) pa_operation_unref(o);
}

// subscription

static void subscribe_cb(pa_context *c, pa_subscription_event_type_t event_type,
	uint32_t idx, void *userdata)
{
	App *app = userdata;
	pa_subscription_event_type_t facility = event_type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
	pa_subscription_event_type_t action = event_type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
	pa_operation *o = NULL;

	if (action == PA_SUBSCRIPTION_EVENT_REMOVE)
	{
		switch (facility)
		{
			case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
				remove_row(app, ROW_PLAYBACK, idx); break;
			case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT:
				remove_row(app, ROW_RECORDING, idx); break;
			case PA_SUBSCRIPTION_EVENT_SINK:
				remove_row(app, ROW_OUTPUT_DEV, idx); break;
			case PA_SUBSCRIPTION_EVENT_SOURCE:
				remove_row(app, ROW_INPUT_DEV, idx); break;
			case PA_SUBSCRIPTION_EVENT_CARD:
				remove_row(app, ROW_CARD, idx); break;
			default: break;
		}
		return;
	}

	switch (facility)
	{
		case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
			o = pa_context_get_sink_input_info(c, idx, sink_input_cb, app);
			break;
		case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT:
			o = pa_context_get_source_output_info(c, idx, source_output_cb, app);
			break;
		case PA_SUBSCRIPTION_EVENT_SINK:
			o = pa_context_get_sink_info_by_index(c, idx, sink_cb, app);
			break;
		case PA_SUBSCRIPTION_EVENT_SOURCE:
			o = pa_context_get_source_info_by_index(c, idx, source_cb, app);
			break;
		case PA_SUBSCRIPTION_EVENT_CARD:
			o = pa_context_get_card_info_by_index(c, idx, card_cb, app);
			break;
		case PA_SUBSCRIPTION_EVENT_SERVER:
			o = pa_context_get_server_info(c, server_info_cb, app);
			break;
		default:
			break;
	}
	if (o)
	{
		pa_operation_unref(o);
	}
}

static void context_state_cb(pa_context *c, void *userdata)
{
	App *app = userdata;
	switch (pa_context_get_state(c))
	{
		case PA_CONTEXT_READY:
		{
			pa_context_set_subscribe_callback(c, subscribe_cb, app);
			pa_subscription_mask_t mask = PA_SUBSCRIPTION_MASK_SINK_INPUT |
				PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT |
				PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE |
				PA_SUBSCRIPTION_MASK_CARD | PA_SUBSCRIPTION_MASK_SERVER;
			pa_operation *o = pa_context_subscribe(c, mask, NULL, NULL);

			if (o) pa_operation_unref(o);
			refresh_all(app);
			break;
		}
		case PA_CONTEXT_FAILED:
			g_warning("PulseAudio connection lost");
			break;
		case PA_CONTEXT_TERMINATED:
			break;
		default:
			break;
	}
}

// lifecycle
gboolean pa_backend_start(App *app)
{
	app->pa_loop = pa_glib_mainloop_new(NULL);
	if (!app->pa_loop)
		return FALSE;

	pa_mainloop_api *api = pa_glib_mainloop_get_api(app->pa_loop);
	pa_proplist *pl = pa_proplist_new();
	pa_proplist_sets(pl, PA_PROP_APPLICATION_NAME, "pavoldcontrol");
	pa_proplist_sets(pl, PA_PROP_APPLICATION_ICON_NAME, "multimedia-volume-control");

	app->pa_ctx = pa_context_new_with_proplist(api, NULL, pl);
	pa_proplist_free(pl);
	if (!app->pa_ctx) return FALSE;

	pa_context_set_state_callback(app->pa_ctx, context_state_cb, app);
	if (pa_context_connect(app->pa_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0)
		return FALSE;
	return TRUE;
}

void pa_backend_stop(App *app)
{
	if (app->pa_ctx)
	{
		pa_context_disconnect(app->pa_ctx);
		pa_context_unref(app->pa_ctx);
		app->pa_ctx = NULL;
	}
	if (app->pa_loop)
	{
		pa_glib_mainloop_free(app->pa_loop);
		app->pa_loop = NULL;
	}
}

// Actions

void pa_backend_stream_set_volume(App *app, RowKind kind, guint32 index,
	const pa_cvolume *cvol)
{
	pa_operation *o = NULL;
	if (kind == ROW_PLAYBACK)
		o = pa_context_set_sink_input_volume(app->pa_ctx, index, cvol, NULL, NULL);
	else if (kind == ROW_RECORDING)
		o = pa_context_set_source_output_volume(app->pa_ctx, index, cvol, NULL, NULL);
	if (o)
		pa_operation_unref(o);
}

void pa_backend_stream_set_mute(App *app, RowKind kind, guint32 index, gboolean mute)
{
	pa_operation *o = NULL;
	if (kind == ROW_PLAYBACK)
		o = pa_context_set_sink_input_mute(app->pa_ctx, index, mute, NULL, NULL);
	else if (kind == ROW_RECORDING)
		o = pa_context_set_source_output_mute(app->pa_ctx, index, mute, NULL, NULL);
	if (o)
		pa_operation_unref(o);
}

void pa_backend_stream_move(App *app, RowKind kind, guint32 index, const gchar *device_name)
{
	pa_operation *o = NULL;
	if (kind == ROW_PLAYBACK)
		o = pa_context_move_sink_input_by_name(app->pa_ctx, index, device_name, NULL, NULL);
	else if (kind == ROW_RECORDING)
		o = pa_context_move_source_output_by_name(app->pa_ctx, index, device_name, NULL, NULL);
	if (o)
		pa_operation_unref(o);
}

void pa_backend_device_set_volume(App *app, RowKind kind, guint32 index,
	const pa_cvolume *cvol)
{
	pa_operation *o = NULL;
	if (kind == ROW_OUTPUT_DEV)
		o = pa_context_set_sink_volume_by_index(app->pa_ctx, index, cvol, NULL, NULL);
	else if (kind == ROW_INPUT_DEV)
		o = pa_context_set_source_volume_by_index(app->pa_ctx, index, cvol, NULL, NULL);
	if (o)
		pa_operation_unref(o);
}

void pa_backend_device_set_mute(App *app, RowKind kind, guint32 index, gboolean mute)
{
	pa_operation *o = NULL;
	if (kind == ROW_OUTPUT_DEV)
		o = pa_context_set_sink_mute_by_index(app->pa_ctx, index, mute, NULL, NULL);
	else if (kind == ROW_INPUT_DEV)
		o = pa_context_set_source_mute_by_index(app->pa_ctx, index, mute, NULL, NULL);
	if (o)
		pa_operation_unref(o);
}

void pa_backend_device_set_port(App *app, RowKind kind, guint32 index, const gchar *port)
{
	pa_operation *o = NULL;
	if (kind == ROW_OUTPUT_DEV)
		o = pa_context_set_sink_port_by_index(app->pa_ctx, index, port, NULL, NULL);
	else if (kind == ROW_INPUT_DEV)
		o = pa_context_set_source_port_by_index(app->pa_ctx, index, port, NULL, NULL);
	if (o)
		pa_operation_unref(o);
}

void pa_backend_device_set_default(App *app, RowKind kind, const gchar *name)
{
	pa_operation *o = NULL;
	if (kind == ROW_OUTPUT_DEV)
		o = pa_context_set_default_sink(app->pa_ctx, name, NULL, NULL);
	else if (kind == ROW_INPUT_DEV)
		o = pa_context_set_default_source(app->pa_ctx, name, NULL, NULL);
	if (o)
		pa_operation_unref(o);
}

void pa_backend_card_set_profile(App *app, guint32 index, const gchar *profile)
{
	pa_operation *o = pa_context_set_card_profile_by_index(app->pa_ctx, index,
		profile, NULL, NULL);

	if (o) pa_operation_unref(o);
}

// Filtering / settings application //

static gboolean row_passes_filter(App *app, StreamRow *row)
{
	switch (row->kind)
	{
		case ROW_PLAYBACK:
			switch (app->settings.playback_filter)
			{
				case STREAM_FILTER_APPLICATIONS: return !row->is_virtual;
				case STREAM_FILTER_VIRTUAL: return row->is_virtual;
				default: return TRUE;
			}
		case ROW_RECORDING:
			switch (app->settings.recording_filter)
			{
				case STREAM_FILTER_APPLICATIONS: return !row->is_virtual;
				case STREAM_FILTER_VIRTUAL: return row->is_virtual;
				default: return TRUE;
			}
		case ROW_OUTPUT_DEV:
			switch (app->settings.output_filter)
			{
				case OUTPUT_FILTER_HARDWARE: return row->is_hardware;
				case OUTPUT_FILTER_VIRTUAL: return !row->is_hardware;
				default: return TRUE;
			}
		case ROW_INPUT_DEV:
			switch (app->settings.input_filter)
			{
				case INPUT_FILTER_NO_MONITORS: return !row->is_monitor;
				case INPUT_FILTER_HARDWARE: return row->is_hardware && !row->is_monitor;
				case INPUT_FILTER_VIRTUAL: return !row->is_hardware && !row->is_monitor;
				case INPUT_FILTER_MONITORS: return row->is_monitor;
				default: return TRUE;
			}
		case ROW_CARD:
		default:
			return TRUE;
	}
}

void app_apply_filter(App *app, RowKind kind)
{
	GHashTable *table = rows_for(app, kind);
	if (!table) return;

	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, table);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		StreamRow *row = value;
		gtk_widget_set_visible(row->root, row_passes_filter(app, row));
	}
}

void app_apply_meter_setting(App *app)
{
	GHashTable *tables[] = { app->playback_rows, app->sink_rows, app->source_rows };
	RowKind kinds[] = { ROW_PLAYBACK, ROW_OUTPUT_DEV, ROW_INPUT_DEV };

	for (size_t t = 0; t < G_N_ELEMENTS(tables); t++)
	{
		GHashTableIter iter;
		gpointer key, value;
		g_hash_table_iter_init(&iter, tables[t]);
		while (g_hash_table_iter_next(&iter, &key, &value))
		{
			StreamRow *row = value;

			if (row->meter)
			{
				gtk_widget_set_visible(row->meter, app->settings.show_volume_meters);
			}

			if (app->settings.show_volume_meters && !row->meter_stream)
			{
				if (kinds[t] == ROW_PLAYBACK)
				{
					const gchar *sink_name = g_hash_table_lookup(app->sink_index_to_name,
						GUINT_TO_POINTER(row->index));
					if (sink_name)
						pa_backend_meter_attach(app, row, ROW_PLAYBACK, NULL, row->index);
				}
				else if (kinds[t] == ROW_OUTPUT_DEV)
				{
					const gchar *sink_name = row->device_name;
					if (sink_name)
					{
						gchar *mon = g_strconcat(sink_name, ".monitor", NULL);
						pa_backend_meter_attach(app, row, ROW_OUTPUT_DEV, mon, 0);
						g_free(mon);
					}
				}
				else if (kinds[t] == ROW_INPUT_DEV)
				{
					if (row->device_name)
						pa_backend_meter_attach(app, row, ROW_INPUT_DEV, row->device_name, 0);
				}
			}
			else if (!app->settings.show_volume_meters && row->meter_stream)
			{
				pa_backend_meter_detach(row);
			}
		}
	}
}
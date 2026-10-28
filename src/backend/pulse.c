#include "../a_backend.h"
#include "../a_common.h"

#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct
{
	pa_glib_mainloop *loop;
	pa_context *ctx;
} Pulse;

static inline Pulse *pulse_of(App *app)
{
	return app->backend;
}

static void refresh_all(App *app);

const gchar *a_backend_name(void)
{
	return "PulseAudio";
}

// Helpers //

static gboolean is_own_stream(const pa_proplist *pl)
{
	const gchar *pid_str = pa_proplist_gets(pl, PA_PROP_APPLICATION_PROCESS_ID);
	if (!pid_str) return FALSE;
	return atoi(pid_str) == (int) getpid();
}

static const gchar *stream_icon(const pa_proplist *pl)
{
	const gchar *icon = pa_proplist_gets(pl, PA_PROP_APPLICATION_ICON_NAME);
	if (!icon) icon = pa_proplist_gets(pl, PA_PROP_MEDIA_ICON_NAME);
	return icon ? icon : "audio-x-generic";
}

static void to_avol(AVolume *out, const pa_cvolume *in)
{
	memset(out, 0, sizeof(*out));
	out->channels = in->channels;
	for (guint8 c = 0; c < in->channels && c < A_VOLUME_MAX_CHANNELS; c++)
	{
		out->values[c] = in->values[c];
	}
}

static void to_cvol(pa_cvolume *out, const AVolume *in)
{
	pa_cvolume_init(out);
	out->channels = in->channels;
	for (guint8 c = 0; c < in->channels && c < PA_CHANNELS_MAX; c++)
	{
		out->values[c] = in->values[c];
	}
}

static void unref_op(pa_operation *o)
{
	if (o) pa_operation_unref(o);
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
		a_common_meter_report(row, samples[length / sizeof(float) - 1]);
	}
	if (length > 0) pa_stream_drop(s);
}

void a_backend_meter_attach(App *app, StreamRow *row)
{
	Pulse *p = pulse_of(app);
	if (!p || !p->ctx || row->meter_handle) return;

	gchar *monitor_source = NULL;
	switch (row->kind)
	{
		case ROW_PLAYBACK: break;
		case ROW_OUTPUT_DEV:
			if (!row->device_name) return;
			monitor_source = g_strconcat(row->device_name, ".monitor", NULL);
			break;
		case ROW_INPUT_DEV:
			if (!row->device_name) return;
			monitor_source = g_strdup(row->device_name);
			break;
		default: return;
	}

	pa_sample_spec ss;
	ss.format = PA_SAMPLE_FLOAT32NE;
	ss.rate = 25;
	ss.channels = 1;

	pa_stream *s = pa_stream_new(p->ctx, "peak-meter", &ss, NULL);
	if (!s)
	{
		g_free(monitor_source);
		return;
	}

	pa_stream_set_read_callback(s, meter_read_cb, row);

	pa_buffer_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.fragsize = sizeof(float);
	attr.maxlength = (uint32_t) -1;

	pa_stream_flags_t flags = PA_STREAM_DONT_MOVE | PA_STREAM_PEAK_DETECT |
		PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_INHIBIT_AUTO_SUSPEND;

	if (row->kind == ROW_PLAYBACK)
		pa_stream_set_monitor_stream(s, row->index);

	int rc = pa_stream_connect_record(s, monitor_source, &attr, flags);
	g_free(monitor_source);
	if (rc < 0)
	{
		pa_stream_unref(s);
		return;
	}
	row->meter_handle = s;
}

void a_backend_meter_detach(StreamRow *row)
{
	pa_stream *s = row->meter_handle;
	if (!s) return;
	pa_stream_set_read_callback(s, NULL, NULL);
	pa_stream_disconnect(s);
	pa_stream_unref(s);
	row->meter_handle = NULL;
}

// Choice lists //

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

// Info callbacks //

static void sink_input_cb(pa_context *c, const pa_sink_input_info *info, int eol,
	void *userdata)
{
	(void)c;
	App *app = userdata;
	if (eol || !info) return;

	StreamRow *row = a_common_ensure_row(app, ROW_PLAYBACK, info->index);
	const gchar *name = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
	if (!name) name = info->name;
	row_set_name(row, name, stream_icon(info->proplist));

	AVolume vol;
	to_avol(&vol, &info->volume);
	row_set_channels(row, info->channel_map.channels, &vol);
	row_set_mute(row, info->mute);

	row->is_virtual = (info->client == PA_INVALID_INDEX);
	gtk_widget_set_visible(row->root, a_common_row_passes_filter(app, row));

	const gchar *sink_name = g_hash_table_lookup(app->sink_index_to_name,
		GUINT_TO_POINTER(info->sink));
	GList *choices = a_common_choices_from_names(app->sink_names);
	row_set_choices(row, choices, sink_name);
	g_list_free_full(choices, row_choice_free);

	if (!row->meter_handle && app->settings.show_volume_meters)
		a_backend_meter_attach(app, row);
}

static void source_output_cb(pa_context *c, const pa_source_output_info *info, int eol, void *userdata)
{
	(void)c;
	App *app = userdata;
	if (eol || !info) return;

	if (is_own_stream(info->proplist))
	{
		a_common_remove_row(app, ROW_RECORDING, info->index);
		return;
	}

	StreamRow *row = a_common_ensure_row(app, ROW_RECORDING, info->index);
	const gchar *name = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
	if (!name) name = info->name;
	row_set_name(row, name, stream_icon(info->proplist));

	AVolume vol;
	to_avol(&vol, &info->volume);
	row_set_channels(row, info->channel_map.channels, &vol);
	row_set_mute(row, info->mute);

	row->is_virtual = (info->client == PA_INVALID_INDEX);
	gtk_widget_set_visible(row->root, a_common_row_passes_filter(app, row));

	const gchar *source_name = g_hash_table_lookup(app->source_index_to_name,
		GUINT_TO_POINTER(info->source));
	GList *choices = a_common_choices_from_names(app->source_names);
	row_set_choices(row, choices, source_name);
	g_list_free_full(choices, row_choice_free);
}

static void sink_cb(pa_context *c, const pa_sink_info *info, int eol, void *userdata)
{
	(void)c;
	App *app = userdata;
	if (eol || !info) return;

	g_hash_table_insert(app->sink_names, g_strdup(info->name), g_strdup(info->description));
	g_hash_table_insert(app->sink_index_to_name, GUINT_TO_POINTER(info->index),
		g_strdup(info->name));

	StreamRow *row = a_common_ensure_row(app, ROW_OUTPUT_DEV, info->index);
	row_set_name(row, info->description, "audio-card");

	AVolume vol;
	to_avol(&vol, &info->volume);
	row_set_channels(row, info->channel_map.channels, &vol);
	row_set_mute(row, info->mute);
	row_set_device_name(row, info->name);
	row_set_preferred(row, g_strcmp0(info->name, app->default_sink_name) == 0);
	row_set_force_mono(row, app->settings.mono_output_devices);

	const gchar *active_port = info->active_port ? info->active_port->name : NULL;
	GList *choices = choices_from_sink_ports(info);
	row_set_choices(row, choices, active_port);
	g_list_free_full(choices, row_choice_free);

	row->is_hardware = (info->flags & PA_SINK_HARDWARE) != 0;
	gtk_widget_set_visible(row->root, a_common_row_passes_filter(app, row));

	if (!row->meter_handle && app->settings.show_volume_meters)
		a_backend_meter_attach(app, row);
	else if (row->meter_handle && !app->settings.show_volume_meters)
		a_backend_meter_detach(row);
}

static void source_cb(pa_context *c, const pa_source_info *info, int eol, void *userdata)
{
	(void)c;
	App *app = userdata;
	if (eol || !info) return;

	g_hash_table_insert(app->source_names, g_strdup(info->name), g_strdup(info->description));
	g_hash_table_insert(app->source_index_to_name, GUINT_TO_POINTER(info->index),
		g_strdup(info->name));

	StreamRow *row = a_common_ensure_row(app, ROW_INPUT_DEV, info->index);
	row_set_name(row, info->description, "audio-input-microphone");

	AVolume vol;
	to_avol(&vol, &info->volume);
	row_set_channels(row, info->channel_map.channels, &vol);
	row_set_mute(row, info->mute);
	row_set_device_name(row, info->name);
	row_set_preferred(row, g_strcmp0(info->name, app->default_source_name) == 0);

	const gchar *active_port = info->active_port ? info->active_port->name : NULL;
	GList *choices = choices_from_source_ports(info);
	row_set_choices(row, choices, active_port);
	g_list_free_full(choices, row_choice_free);

	row->is_hardware = (info->flags & PA_SOURCE_HARDWARE) != 0;
	row->is_monitor = (info->monitor_of_sink != PA_INVALID_INDEX);
	gtk_widget_set_visible(row->root, a_common_row_passes_filter(app, row));

	if (!row->meter_handle && app->settings.show_volume_meters)
		a_backend_meter_attach(app, row);
	else if (row->meter_handle && !app->settings.show_volume_meters)
		a_backend_meter_detach(row);
}

static void card_cb(pa_context *c, const pa_card_info *info, int eol, void *userdata)
{
	(void)c;
	App *app = userdata;
	if (eol || !info) return;

	StreamRow *row = a_common_ensure_row(app, ROW_CARD, info->index);

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
	Pulse *p = pulse_of(app);
	unref_op(pa_context_get_sink_info_list(p->ctx, sink_cb, app));
	unref_op(pa_context_get_source_info_list(p->ctx, source_cb, app));
}

static void refresh_all(App *app)
{
	Pulse *p = pulse_of(app);
	unref_op(pa_context_get_server_info(p->ctx, server_info_cb, app));
	unref_op(pa_context_get_sink_info_list(p->ctx, sink_cb, app));
	unref_op(pa_context_get_source_info_list(p->ctx, source_cb, app));
	unref_op(pa_context_get_card_info_list(p->ctx, card_cb, app));
	unref_op(pa_context_get_sink_input_info_list(p->ctx, sink_input_cb, app));
	unref_op(pa_context_get_source_output_info_list(p->ctx, source_output_cb, app));
}

void a_backend_refresh_cards(App *app)
{
	Pulse *p = pulse_of(app);
	if (!p || !p->ctx) return;
	unref_op(pa_context_get_card_info_list(p->ctx, card_cb, app));
}

// Subscription //

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
				a_common_remove_row(app, ROW_PLAYBACK, idx); break;
			case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT:
				a_common_remove_row(app, ROW_RECORDING, idx); break;
			case PA_SUBSCRIPTION_EVENT_SINK:
				a_common_remove_row(app, ROW_OUTPUT_DEV, idx); break;
			case PA_SUBSCRIPTION_EVENT_SOURCE:
				a_common_remove_row(app, ROW_INPUT_DEV, idx); break;
			case PA_SUBSCRIPTION_EVENT_CARD:
				a_common_remove_row(app, ROW_CARD, idx); break;
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
	unref_op(o);
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
			unref_op(pa_context_subscribe(c, mask, NULL, NULL));
			refresh_all(app);
			break;
		}
		case PA_CONTEXT_FAILED:
			g_warning("PulseAudio connection lost");
			break;
		default:
			break;
	}
}

// Lifecycle //

gboolean a_backend_start(App *app)
{
	Pulse *p = g_new0(Pulse, 1);
	app->backend = p;

	p->loop = pa_glib_mainloop_new(NULL);
	if (!p->loop)
		return FALSE;

	pa_mainloop_api *api = pa_glib_mainloop_get_api(p->loop);
	pa_proplist *pl = pa_proplist_new();
	pa_proplist_sets(pl, PA_PROP_APPLICATION_NAME, "pavoldcontrol");
	pa_proplist_sets(pl, PA_PROP_APPLICATION_ICON_NAME, "multimedia-volume-control");

	p->ctx = pa_context_new_with_proplist(api, NULL, pl);
	pa_proplist_free(pl);
	if (!p->ctx) return FALSE;

	pa_context_set_state_callback(p->ctx, context_state_cb, app);
	if (pa_context_connect(p->ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0)
		return FALSE;
	return TRUE;
}

void a_backend_stop(App *app)
{
	Pulse *p = pulse_of(app);
	if (!p) return;

	if (p->ctx)
	{
		pa_context_disconnect(p->ctx);
		pa_context_unref(p->ctx);
		p->ctx = NULL;
	}
	if (p->loop)
	{
		pa_glib_mainloop_free(p->loop);
		p->loop = NULL;
	}
	g_free(p);
	app->backend = NULL;
}

// Actions //

void a_backend_stream_set_volume(App *app, RowKind kind, guint32 index, const AVolume *vol)
{
	pa_cvolume cvol;
	to_cvol(&cvol, vol);
	if (kind == ROW_PLAYBACK)
		unref_op(pa_context_set_sink_input_volume(pulse_of(app)->ctx, index, &cvol, NULL, NULL));
	else if (kind == ROW_RECORDING)
		unref_op(pa_context_set_source_output_volume(pulse_of(app)->ctx, index, &cvol, NULL, NULL));
}

void a_backend_stream_set_mute(App *app, RowKind kind, guint32 index, gboolean mute)
{
	pa_context *c = pulse_of(app)->ctx;
	if (kind == ROW_PLAYBACK)
		unref_op(pa_context_set_sink_input_mute(c, index, mute, NULL, NULL));
	else if (kind == ROW_RECORDING)
		unref_op(pa_context_set_source_output_mute(c, index, mute, NULL, NULL));
}

void a_backend_stream_move(App *app, RowKind kind, guint32 index, const gchar *device_name)
{
	pa_context *c = pulse_of(app)->ctx;
	if (kind == ROW_PLAYBACK)
		unref_op(pa_context_move_sink_input_by_name(c, index, device_name, NULL, NULL));
	else if (kind == ROW_RECORDING)
		unref_op(pa_context_move_source_output_by_name(c, index, device_name, NULL, NULL));
}

void a_backend_device_set_volume(App *app, RowKind kind, guint32 index, const AVolume *vol)
{
	pa_cvolume cvol;
	to_cvol(&cvol, vol);
	pa_context *c = pulse_of(app)->ctx;
	if (kind == ROW_OUTPUT_DEV)
		unref_op(pa_context_set_sink_volume_by_index(c, index, &cvol, NULL, NULL));
	else if (kind == ROW_INPUT_DEV)
		unref_op(pa_context_set_source_volume_by_index(c, index, &cvol, NULL, NULL));
}

void a_backend_device_set_mute(App *app, RowKind kind, guint32 index, gboolean mute)
{
	pa_context *c = pulse_of(app)->ctx;
	if (kind == ROW_OUTPUT_DEV)
		unref_op(pa_context_set_sink_mute_by_index(c, index, mute, NULL, NULL));
	else if (kind == ROW_INPUT_DEV)
		unref_op(pa_context_set_source_mute_by_index(c, index, mute, NULL, NULL));
}

void a_backend_device_set_port(App *app, RowKind kind, guint32 index, const gchar *port)
{
	pa_context *c = pulse_of(app)->ctx;
	if (kind == ROW_OUTPUT_DEV)
		unref_op(pa_context_set_sink_port_by_index(c, index, port, NULL, NULL));
	else if (kind == ROW_INPUT_DEV)
		unref_op(pa_context_set_source_port_by_index(c, index, port, NULL, NULL));
}

void a_backend_device_set_default(App *app, RowKind kind, const gchar *name)
{
	pa_context *c = pulse_of(app)->ctx;
	if (kind == ROW_OUTPUT_DEV)
		unref_op(pa_context_set_default_sink(c, name, NULL, NULL));
	else if (kind == ROW_INPUT_DEV)
		unref_op(pa_context_set_default_source(c, name, NULL, NULL));
}

void a_backend_card_set_profile(App *app, guint32 index, const gchar *profile)
{
	unref_op(pa_context_set_card_profile_by_index(pulse_of(app)->ctx, index, profile,
		NULL, NULL));
}

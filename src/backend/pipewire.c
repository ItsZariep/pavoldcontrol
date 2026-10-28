#define _GNU_SOURCE

#include "../a_backend.h"
#include "../a_common.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/param/profile.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/pod/iter.h>
#include <spa/utils/json.h>
#include <spa/utils/result.h>

#define METER_NODE_NAME "pavoldcontrol-meter"

typedef struct PW PW;
typedef struct Obj Obj;

typedef enum
{
	K_NODE,
	K_DEVICE,
	K_METADATA
} Kind;

typedef struct
{
	gchar *name;
	gchar *desc;
	gboolean unavailable;
} ProfileInfo;

typedef struct
{
	gint index;
	gint direction;
	gchar *name;
	gchar *desc;
	GArray *devices; // gint: card.profile.device values this route applies to
} RouteInfo;

typedef struct
{
	uint32_t output_node;
	uint32_t input_node;
} LinkInfo;

typedef struct
{
	PW *pw;
	StreamRow *row;
	struct pw_stream *stream;
	struct spa_hook listener;
} Meter;

struct Obj
{
	PW *pw;
	uint32_t id;
	uint32_t serial;
	Kind kind;

	void *proxy; // typed per kind: pw_node, pw_device or pw_metadata
	struct spa_hook object_listener;

	GHashTable *props; // string -> string

	// nodes
	RowKind row_kind;
	gboolean have_volume;
	gboolean mute;
	AVolume volume;

	// devices (cards)
	GHashTable *profiles; // index -> ProfileInfo
	gint active_profile;
	GHashTable *routes; // index -> RouteInfo
	GHashTable *active_routes; // card.profile.device -> route index
};

struct PW
{
	App *app;

	struct pw_loop *loop;
	GSource *source;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_registry *registry;
	struct spa_hook registry_listener;

	GHashTable *objs; // id -> Obj*
	GHashTable *links; // id -> LinkInfo*
	Obj *metadata;

	gchar *default_sink;
	gchar *default_source;

	guint refresh_id;
};

static inline PW *pw_of(App *app)
{
	return app->backend;
}

const gchar *a_backend_name(void)
{
	return "PipeWire";
}

// GLib main loop integration //

typedef struct
{
	GSource base;
	struct pw_loop *loop;
	GPollFD pfd;
} LoopSource;

static gboolean loop_prepare(GSource *source, gint *timeout)
{
	(void)source;
	*timeout = -1;
	return FALSE;
}

static gboolean loop_check(GSource *source)
{
	LoopSource *ls = (LoopSource *) source;
	return (ls->pfd.revents & (G_IO_IN | G_IO_ERR | G_IO_HUP)) != 0;
}

static gboolean loop_dispatch(GSource *source, GSourceFunc callback, gpointer user_data)
{
	(void)callback;
	(void)user_data;
	LoopSource *ls = (LoopSource *) source;

	pw_loop_enter(ls->loop);
	pw_loop_iterate(ls->loop, 0);
	pw_loop_leave(ls->loop);
	return G_SOURCE_CONTINUE;
}

static GSourceFuncs loop_funcs = {
	.prepare = loop_prepare,
	.check = loop_check,
	.dispatch = loop_dispatch,
};

static GSource *loop_source_new(struct pw_loop *loop)
{
	GSource *source = g_source_new(&loop_funcs, sizeof(LoopSource));
	LoopSource *ls = (LoopSource *) source;
	ls->loop = loop;
	ls->pfd.fd = pw_loop_get_fd(loop);
	ls->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
	g_source_add_poll(source, &ls->pfd);
	g_source_attach(source, NULL);
	return source;
}

// Small helpers //

static Obj *obj_lookup(PW *pw, uint32_t id)
{
	return g_hash_table_lookup(pw->objs, GUINT_TO_POINTER(id));
}

static const gchar *prop(const Obj *o, const gchar *key)
{
	return g_hash_table_lookup(o->props, key);
}

static void props_merge(Obj *o, const struct spa_dict *dict)
{
	const struct spa_dict_item *item;
	if (!dict) return;
	spa_dict_for_each(item, dict)
	{
		if (!item->key || !item->value) continue;
		g_hash_table_insert(o->props, g_strdup(item->key), g_strdup(item->value));
	}
	const gchar *serial = prop(o, PW_KEY_OBJECT_SERIAL);
	if (serial) o->serial = (uint32_t) g_ascii_strtoull(serial, NULL, 10);
}

static gboolean prop_is_true(const Obj *o, const gchar *key)
{
	const gchar *v = prop(o, key);
	return v && (g_str_equal(v, "true") || g_str_equal(v, "1"));
}

// returns a newly allocated "name" from a {"name":"..."} JSON string
static gchar *json_name(const gchar *json)
{
	struct spa_json it[2];
	char key[128], val[512];

	if (!json) return NULL;
	spa_json_init(&it[0], json, strlen(json));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
		return NULL;

	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0)
	{
		if (g_str_equal(key, "name"))
		{
			if (spa_json_get_string(&it[1], val, sizeof(val)) > 0)
				return g_strdup(val);
			return NULL;
		}
		const char *skip;
		if (spa_json_next(&it[1], &skip) <= 0) break;
	}
	return NULL;
}

static gboolean classify_node(const gchar *media_class, RowKind *kind)
{
	if (!media_class) return FALSE;
	if (g_str_equal(media_class, "Audio/Sink") || g_str_equal(media_class, "Audio/Sink/Virtual"))
		*kind = ROW_OUTPUT_DEV;
	else if (g_str_equal(media_class, "Audio/Source") ||
		g_str_equal(media_class, "Audio/Source/Virtual"))
		*kind = ROW_INPUT_DEV;
	else if (g_str_equal(media_class, "Stream/Output/Audio"))
		*kind = ROW_PLAYBACK;
	else if (g_str_equal(media_class, "Stream/Input/Audio"))
		*kind = ROW_RECORDING;
	else
		return FALSE;
	return TRUE;
}

static gboolean is_own_node(const Obj *o)
{
	const gchar *pid = prop(o, PW_KEY_APP_PROCESS_ID);
	if (pid && atoi(pid) == (int) getpid()) return TRUE;
	const gchar *name = prop(o, PW_KEY_NODE_NAME);
	return name && g_str_has_prefix(name, METER_NODE_NAME);
}

static const gchar *stream_icon(const Obj *o)
{
	const gchar *icon = prop(o, PW_KEY_APP_ICON_NAME);
	if (!icon) icon = prop(o, PW_KEY_MEDIA_ICON_NAME);
	return icon ? icon : "audio-x-generic";
}

static const gchar *node_description(const Obj *o)
{
	const gchar *d = prop(o, PW_KEY_NODE_DESCRIPTION);
	if (!d) d = prop(o, PW_KEY_NODE_NICK);
	if (!d) d = prop(o, PW_KEY_NODE_NAME);
	return d;
}

static Obj *find_node_by_name(PW *pw, const gchar *name, RowKind kind)
{
	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, pw->objs);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		Obj *o = value;
		if (o->kind == K_NODE && o->row_kind == kind &&
			g_strcmp0(prop(o, PW_KEY_NODE_NAME), name) == 0)
			return o;
	}
	return NULL;
}

static Obj *device_of(Obj *node)
{
	const gchar *id = prop(node, PW_KEY_DEVICE_ID);
	if (!id) return NULL;
	Obj *dev = obj_lookup(node->pw, (uint32_t) g_ascii_strtoull(id, NULL, 10));
	return (dev && dev->kind == K_DEVICE) ? dev : NULL;
}

static gint card_profile_device(Obj *node)
{
	const gchar *v = prop(node, "card.profile.device");
	return v ? atoi(v) : -1;
}

// Refresh scheduling //

static void refresh_all(PW *pw);

static gboolean refresh_idle(gpointer user_data)
{
	PW *pw = user_data;
	pw->refresh_id = 0;
	refresh_all(pw);
	return G_SOURCE_REMOVE;
}

// Coalesces bursts of registry events into one UI refresh
static void schedule_refresh(PW *pw)
{
	if (!pw->refresh_id)
		pw->refresh_id = g_idle_add(refresh_idle, pw);
}

// Node -> row //

static void node_apply_volume(Obj *o, StreamRow *row)
{
	if (o->have_volume) row_set_channels(row, o->volume.channels, &o->volume);
	row_set_mute(row, o->mute);
}

// Linked device of a stream, by node.name
static const gchar *stream_target_name(Obj *stream)
{
	PW *pw = stream->pw;
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init(&iter, pw->links);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		const LinkInfo *l = value;
		Obj *peer = NULL;
		RowKind want;

		if (stream->row_kind == ROW_PLAYBACK && l->output_node == stream->id)
		{
			peer = obj_lookup(pw, l->input_node);
			want = ROW_OUTPUT_DEV;
		}
		else if (stream->row_kind == ROW_RECORDING && l->input_node == stream->id)
		{
			peer = obj_lookup(pw, l->output_node);
			want = ROW_INPUT_DEV;
		}
		else
			continue;

		if (peer && peer->kind == K_NODE && peer->row_kind == want)
			return prop(peer, PW_KEY_NODE_NAME);
	}
	return NULL;
}

static gint route_compare(gconstpointer a, gconstpointer b)
{
	const RouteInfo *ra = a;
	const RouteInfo *rb = b;
	return ra->index - rb->index;
}

// Ports (device routes) that apply to this node. *active gets the name of the
// active one (owned by the route table, do not free).
static GList *node_port_choices(Obj *node, const gchar **active)
{
	*active = NULL;
	Obj *dev = device_of(node);
	gint cpd = card_profile_device(node);
	if (!dev || cpd < 0) return NULL;

	gint dir = (node->row_kind == ROW_OUTPUT_DEV) ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;

	GList *routes = g_list_sort(g_hash_table_get_values(dev->routes), route_compare);
	GList *out = NULL;
	for (GList *l = routes; l; l = l->next)
	{
		const RouteInfo *r = l->data;
		if (r->direction != dir) continue;

		gboolean applies = FALSE;
		for (guint i = 0; r->devices && i < r->devices->len; i++)
		{
			if (g_array_index(r->devices, gint, i) == cpd)
				applies = TRUE;
		}
		if (!applies) continue;

		out = g_list_append(out, row_choice_new(r->name, r->desc ? r->desc : r->name));
	}
	g_list_free(routes);

	gpointer idx;
	if (g_hash_table_lookup_extended(dev->active_routes, GINT_TO_POINTER(cpd), NULL, &idx))
	{
		const RouteInfo *r = g_hash_table_lookup(dev->routes, idx);
		if (r) *active = r->name;
	}
	return out;
}

static void meter_sync(App *app, StreamRow *row)
{
	if (app->settings.show_volume_meters && !row->meter_handle)
		a_backend_meter_attach(app, row);
	else if (!app->settings.show_volume_meters && row->meter_handle)
		a_backend_meter_detach(row);
}

static void refresh_stream(Obj *o)
{
	App *app = o->pw->app;

	if (is_own_node(o))
	{
		a_common_remove_row(app, o->row_kind, o->id);
		return;
	}

	StreamRow *row = a_common_ensure_row(app, o->row_kind, o->id);

	const gchar *name = prop(o, PW_KEY_APP_NAME);
	if (!name) name = node_description(o);
	row_set_name(row, name, stream_icon(o));
	node_apply_volume(o, row);

	row->is_virtual = prop_is_true(o, PW_KEY_NODE_VIRTUAL) || prop(o, PW_KEY_NODE_LINK_GROUP);
	gtk_widget_set_visible(row->root, a_common_row_passes_filter(app, row));

	GHashTable *names = (o->row_kind == ROW_PLAYBACK) ? app->sink_names : app->source_names;
	GList *choices = a_common_choices_from_names(names);
	row_set_choices(row, choices, stream_target_name(o));
	g_list_free_full(choices, row_choice_free);

	meter_sync(app, row);
}

static void refresh_device_node(Obj *o)
{
	PW *pw = o->pw;
	App *app = pw->app;
	gboolean is_sink = (o->row_kind == ROW_OUTPUT_DEV);
	const gchar *node_name = prop(o, PW_KEY_NODE_NAME);
	const gchar *desc = node_description(o);

	if (node_name)
	{
		g_hash_table_insert(is_sink ? app->sink_names : app->source_names,
			g_strdup(node_name), g_strdup(desc));
		g_hash_table_insert(is_sink ? app->sink_index_to_name : app->source_index_to_name,
			GUINT_TO_POINTER(o->id), g_strdup(node_name));
	}

	StreamRow *row = a_common_ensure_row(app, o->row_kind, o->id);
	row_set_name(row, desc, is_sink ? "audio-card" : "audio-input-microphone");
	node_apply_volume(o, row);
	row_set_device_name(row, node_name);
	row_set_preferred(row, g_strcmp0(node_name, is_sink ? pw->default_sink : pw->default_source) == 0);
	if (is_sink)
		row_set_force_mono(row, app->settings.mono_output_devices);

	const gchar *active_port = NULL;
	GList *choices = node_port_choices(o, &active_port);
	row_set_choices(row, choices, active_port);
	g_list_free_full(choices, row_choice_free);

	row->is_hardware = prop(o, PW_KEY_DEVICE_ID) && !prop_is_true(o, PW_KEY_NODE_VIRTUAL);
	row->is_monitor = FALSE; // PipeWire sinks have no separate monitor source nodes
	gtk_widget_set_visible(row->root, a_common_row_passes_filter(app, row));

	meter_sync(app, row);
}

static void node_refresh(Obj *o)
{
	if (o->pw->app->shutting_down) return;

	switch (o->row_kind)
	{
		case ROW_PLAYBACK:
		case ROW_RECORDING:
			refresh_stream(o);
			break;
		case ROW_OUTPUT_DEV:
		case ROW_INPUT_DEV:
			refresh_device_node(o);
			break;
		default:
			break;
	}
}

// Volume / mute only
static void node_refresh_volume(Obj *o)
{
	StreamRow *row = NULL;
	App *app = o->pw->app;
	if (app->shutting_down) return;

	switch (o->row_kind)
	{
		case ROW_PLAYBACK: row = g_hash_table_lookup(app->playback_rows, GUINT_TO_POINTER(o->id)); break;
		case ROW_RECORDING: row = g_hash_table_lookup(app->recording_rows, GUINT_TO_POINTER(o->id)); break;
		case ROW_OUTPUT_DEV: row = g_hash_table_lookup(app->sink_rows, GUINT_TO_POINTER(o->id)); break;
		case ROW_INPUT_DEV: row = g_hash_table_lookup(app->source_rows, GUINT_TO_POINTER(o->id)); break;
		default: break;
	}
	if (row) node_apply_volume(o, row);
}

// Card -> row //

static gint profile_compare(gconstpointer a, gconstpointer b)
{
	return GPOINTER_TO_INT(a) - GPOINTER_TO_INT(b);
}

static void device_refresh(Obj *d)
{
	App *app = d->pw->app;
	if (app->shutting_down) return;

	StreamRow *row = a_common_ensure_row(app, ROW_CARD, d->id);

	const gchar *desc = prop(d, PW_KEY_DEVICE_DESCRIPTION);
	if (!desc) desc = prop(d, PW_KEY_DEVICE_NICK);
	if (!desc) desc = prop(d, PW_KEY_DEVICE_NAME);
	row_set_name(row, desc, "audio-card");

	GList *keys = g_list_sort(g_hash_table_get_keys(d->profiles), profile_compare);
	GList *choices = NULL;
	const gchar *active = NULL;
	for (GList *l = keys; l; l = l->next)
	{
		const ProfileInfo *p = g_hash_table_lookup(d->profiles, l->data);
		if (GPOINTER_TO_INT(l->data) == d->active_profile)
			active = p->name;
		if (app->settings.hide_unavailable_profiles && p->unavailable &&
			GPOINTER_TO_INT(l->data) != d->active_profile)
			continue;
		choices = g_list_append(choices, row_choice_new(p->name, p->desc ? p->desc : p->name));
	}
	g_list_free(keys);

	row_set_profiles(row, choices, active);
	g_list_free_full(choices, row_choice_free);
}

static void refresh_all(PW *pw)
{
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init(&iter, pw->objs);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		Obj *o = value;
		if (o->kind == K_DEVICE) device_refresh(o);
	}
	g_hash_table_iter_init(&iter, pw->objs);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		Obj *o = value;
		if (o->kind == K_NODE) node_refresh(o);
	}
}

void a_backend_refresh_cards(App *app)
{
	PW *pw = pw_of(app);
	if (!pw) return;

	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, pw->objs);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		Obj *o = value;
		if (o->kind == K_DEVICE) device_refresh(o);
	}
}

// Object lifetime //

static void profile_info_free(gpointer data)
{
	ProfileInfo *p = data;
	g_free(p->name);
	g_free(p->desc);
	g_free(p);
}

static void route_info_free(gpointer data)
{
	RouteInfo *r = data;
	g_free(r->name);
	g_free(r->desc);
	if (r->devices) g_array_free(r->devices, TRUE);
	g_free(r);
}

static Obj *obj_new(PW *pw, uint32_t id, Kind kind, const struct spa_dict *props)
{
	Obj *o = g_new0(Obj, 1);
	o->pw = pw;
	o->id = id;
	o->kind = kind;
	o->active_profile = -1;
	o->props = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	if (kind == K_DEVICE)
	{
		o->profiles = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, profile_info_free);
		o->routes = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, route_info_free);
		o->active_routes = g_hash_table_new(g_direct_hash, g_direct_equal);
	}
	props_merge(o, props);
	return o;
}

// ui: also drop rows / name table entries
static void obj_free(Obj *o, gboolean ui)
{
	PW *pw = o->pw;
	App *app = pw->app;

	if (ui && !app->shutting_down)
	{
		if (o->kind == K_NODE)
		{
			a_common_remove_row(app, o->row_kind, o->id);
			const gchar *name = prop(o, PW_KEY_NODE_NAME);
			if (name && o->row_kind == ROW_OUTPUT_DEV)
			{
				g_hash_table_remove(app->sink_names, name);
				g_hash_table_remove(app->sink_index_to_name, GUINT_TO_POINTER(o->id));
			}
			else if (name && o->row_kind == ROW_INPUT_DEV)
			{
				g_hash_table_remove(app->source_names, name);
				g_hash_table_remove(app->source_index_to_name, GUINT_TO_POINTER(o->id));
			}
		}
		else if (o->kind == K_DEVICE)
		{
			a_common_remove_row(app, ROW_CARD, o->id);
		}
	}

	if (pw->metadata == o) pw->metadata = NULL;

	spa_hook_remove(&o->object_listener);
	if (o->proxy) pw_proxy_destroy(o->proxy);

	g_hash_table_destroy(o->props);
	if (o->profiles) g_hash_table_destroy(o->profiles);
	if (o->routes) g_hash_table_destroy(o->routes);
	if (o->active_routes) g_hash_table_destroy(o->active_routes);
	g_free(o);
}

// Node events //

static void node_parse_props(Obj *o, const struct spa_pod *param)
{
	struct spa_pod_prop *p;
	struct spa_pod_object *obj = (struct spa_pod_object *) param;

	SPA_POD_OBJECT_FOREACH(obj, p)
	{
		switch (p->key)
		{
			case SPA_PROP_mute:
			{
				bool mute;
				if (spa_pod_get_bool(&p->value, &mute) == 0)
					o->mute = mute;
				break;
			}
			case SPA_PROP_channelVolumes:
			{
				float vols[A_VOLUME_MAX_CHANNELS];
				uint32_t n = spa_pod_copy_array(&p->value, SPA_TYPE_Float, vols,
					A_VOLUME_MAX_CHANNELS);
				if (n == 0) break;

				memset(&o->volume, 0, sizeof(o->volume));
				o->volume.channels = (guint8) n;
				for (uint32_t c = 0; c < n; c++)
				{
					// PipeWire volumes are linear, the UI uses PulseAudio's cubic scale
					double lin = vols[c] > 0.0f ? (double) vols[c] : 0.0;
					o->volume.values[c] = (guint32) lround(cbrt(lin) * (double) A_VOLUME_NORM);
				}
				o->have_volume = TRUE;
				break;
			}
			default:
				break;
		}
	}
}

static void node_event_info(void *data, const struct pw_node_info *info)
{
	Obj *o = data;
	if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS)
	{
		props_merge(o, info->props);
		node_refresh(o);
	}
}

static void node_event_param(void *data, int seq, uint32_t id, uint32_t index,
	uint32_t next, const struct spa_pod *param)
{
	(void)seq;
	(void)index;
	(void)next;
	Obj *o = data;

	if (id != SPA_PARAM_Props || !param || !spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Props))
		return;
	node_parse_props(o, param);
	node_refresh_volume(o);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_event_info,
	.param = node_event_param,
};

// Device events //

static void parse_profile(Obj *d, uint32_t id, const struct spa_pod *param)
{
	int32_t index = -1;
	const char *name = NULL, *desc = NULL;
	uint32_t avail = SPA_PARAM_AVAILABILITY_unknown;

	if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamProfile, NULL,
			SPA_PARAM_PROFILE_index, SPA_POD_Int(&index),
			SPA_PARAM_PROFILE_name, SPA_POD_OPT_String(&name),
			SPA_PARAM_PROFILE_description, SPA_POD_OPT_String(&desc),
			SPA_PARAM_PROFILE_available, SPA_POD_OPT_Id(&avail)) < 0)
		return;

	if (id == SPA_PARAM_Profile)
	{
		d->active_profile = index;
		return;
	}

	ProfileInfo *p = g_new0(ProfileInfo, 1);
	p->name = g_strdup(name);
	p->desc = g_strdup(desc);
	p->unavailable = (avail == SPA_PARAM_AVAILABILITY_no);
	if (!p->name)
	{
		profile_info_free(p);
		return;
	}
	g_hash_table_insert(d->profiles, GINT_TO_POINTER(index), p);
}

static void parse_route(Obj *d, uint32_t id, const struct spa_pod *param)
{
	int32_t index = -1, device = -1;
	uint32_t direction = SPA_DIRECTION_OUTPUT;
	const char *name = NULL, *desc = NULL;
	struct spa_pod *devices = NULL;

	if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, NULL,
			SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
			SPA_PARAM_ROUTE_direction, SPA_POD_OPT_Id(&direction),
			SPA_PARAM_ROUTE_name, SPA_POD_OPT_String(&name),
			SPA_PARAM_ROUTE_description, SPA_POD_OPT_String(&desc),
			SPA_PARAM_ROUTE_device, SPA_POD_OPT_Int(&device),
			SPA_PARAM_ROUTE_devices, SPA_POD_OPT_Pod(&devices)) < 0)
		return;

	if (id == SPA_PARAM_Route)
	{
		if (device >= 0)
			g_hash_table_insert(d->active_routes, GINT_TO_POINTER(device), GINT_TO_POINTER(index));
		return;
	}

	if (!name) return;

	RouteInfo *r = g_new0(RouteInfo, 1);
	r->index = index;
	r->direction = (gint) direction;
	r->name = g_strdup(name);
	r->desc = g_strdup(desc);
	r->devices = g_array_new(FALSE, FALSE, sizeof(gint));
	if (devices && spa_pod_is_array(devices))
	{
		int32_t vals[64];
		uint32_t n = spa_pod_copy_array(devices, SPA_TYPE_Int, vals, SPA_N_ELEMENTS(vals));
		for (uint32_t i = 0; i < n; i++)
		{
			gint v = vals[i];
			g_array_append_val(r->devices, v);
		}
	}
	g_hash_table_insert(d->routes, GINT_TO_POINTER(index), r);
}

static void device_event_param(void *data, int seq, uint32_t id, uint32_t index,
	uint32_t next, const struct spa_pod *param)
{
	(void)seq;
	(void)next;
	Obj *d = data;
	if (!param) return;

	switch (id)
	{
		case SPA_PARAM_EnumProfile:
			if (index == 0) g_hash_table_remove_all(d->profiles);
			parse_profile(d, id, param);
			break;
		case SPA_PARAM_Profile:
			parse_profile(d, id, param);
			break;
		case SPA_PARAM_EnumRoute:
			if (index == 0) g_hash_table_remove_all(d->routes);
			parse_route(d, id, param);
			break;
		case SPA_PARAM_Route:
			if (index == 0) g_hash_table_remove_all(d->active_routes);
			parse_route(d, id, param);
			break;
		default:
			return;
	}
	schedule_refresh(d->pw);
}

static void device_event_info(void *data, const struct pw_device_info *info)
{
	Obj *d = data;
	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PROPS)
	{
		props_merge(d, info->props);
		schedule_refresh(d->pw);
	}
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.info = device_event_info,
	.param = device_event_param,
};

// Metadata events //

static int metadata_event_property(void *data, uint32_t subject, const char *key,
	const char *type, const char *value)
{
	(void)type;
	Obj *o = data;
	PW *pw = o->pw;

	if (subject != 0) return 0;

	if (!key)
	{
		g_clear_pointer(&pw->default_sink, g_free);
		g_clear_pointer(&pw->default_source, g_free);
	}
	else if (g_str_equal(key, "default.audio.sink"))
	{
		g_free(pw->default_sink);
		pw->default_sink = json_name(value);
	}
	else if (g_str_equal(key, "default.audio.source"))
	{
		g_free(pw->default_source);
		pw->default_source = json_name(value);
	}
	else
		return 0;

	schedule_refresh(pw);
	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_event_property,
};

// Registry //

static void add_node(PW *pw, uint32_t id, const struct spa_dict *props)
{
	RowKind kind;
	if (!classify_node(spa_dict_lookup(props, PW_KEY_MEDIA_CLASS), &kind))
		return;

	Obj *o = obj_new(pw, id, K_NODE, props);
	o->row_kind = kind;
	o->proxy = pw_registry_bind(pw->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
	if (!o->proxy)
	{
		obj_free(o, FALSE);
		return;
	}

	pw_node_add_listener(o->proxy, &o->object_listener, &node_events, o);
	uint32_t ids[] = { SPA_PARAM_Props };
	pw_node_subscribe_params(o->proxy, ids, SPA_N_ELEMENTS(ids));

	g_hash_table_insert(pw->objs, GUINT_TO_POINTER(id), o);
	schedule_refresh(pw);
}

static void add_device(PW *pw, uint32_t id, const struct spa_dict *props)
{
	const gchar *mc = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
	if (!mc || !g_str_equal(mc, "Audio/Device"))
		return;

	Obj *o = obj_new(pw, id, K_DEVICE, props);
	o->proxy = pw_registry_bind(pw->registry, id, PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0);
	if (!o->proxy)
	{
		obj_free(o, FALSE);
		return;
	}

	pw_device_add_listener(o->proxy, &o->object_listener, &device_events, o);
	uint32_t ids[] = { SPA_PARAM_EnumProfile, SPA_PARAM_Profile,
		SPA_PARAM_EnumRoute, SPA_PARAM_Route };
	pw_device_subscribe_params(o->proxy, ids, SPA_N_ELEMENTS(ids));

	g_hash_table_insert(pw->objs, GUINT_TO_POINTER(id), o);
	schedule_refresh(pw);
}

static void add_metadata(PW *pw, uint32_t id, const struct spa_dict *props)
{
	const gchar *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
	if (pw->metadata || !name || !g_str_equal(name, "default"))
		return;

	Obj *o = obj_new(pw, id, K_METADATA, props);
	o->proxy = pw_registry_bind(pw->registry, id, PW_TYPE_INTERFACE_Metadata,
		PW_VERSION_METADATA, 0);
	if (!o->proxy)
	{
		obj_free(o, FALSE);
		return;
	}

	pw_metadata_add_listener(o->proxy, &o->object_listener, &metadata_events, o);
	g_hash_table_insert(pw->objs, GUINT_TO_POINTER(id), o);
	pw->metadata = o;
}

static void add_link(PW *pw, uint32_t id, const struct spa_dict *props)
{
	const gchar *out = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
	const gchar *in = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
	if (!out || !in) return;

	LinkInfo *l = g_new0(LinkInfo, 1);
	l->output_node = (uint32_t) g_ascii_strtoull(out, NULL, 10);
	l->input_node = (uint32_t) g_ascii_strtoull(in, NULL, 10);
	g_hash_table_insert(pw->links, GUINT_TO_POINTER(id), l);
	schedule_refresh(pw);
}

static void registry_event_global(void *data, uint32_t id, uint32_t permissions,
	const char *type, uint32_t version, const struct spa_dict *props)
{
	(void)permissions;
	(void)version;
	PW *pw = data;
	if (!props) return;

	if (g_str_equal(type, PW_TYPE_INTERFACE_Node))
		add_node(pw, id, props);
	else if (g_str_equal(type, PW_TYPE_INTERFACE_Device))
		add_device(pw, id, props);
	else if (g_str_equal(type, PW_TYPE_INTERFACE_Metadata))
		add_metadata(pw, id, props);
	else if (g_str_equal(type, PW_TYPE_INTERFACE_Link))
		add_link(pw, id, props);
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	PW *pw = data;

	Obj *o = obj_lookup(pw, id);
	if (o)
	{
		g_hash_table_steal(pw->objs, GUINT_TO_POINTER(id));
		obj_free(o, TRUE);
		schedule_refresh(pw);
		return;
	}
	if (g_hash_table_remove(pw->links, GUINT_TO_POINTER(id)))
		schedule_refresh(pw);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void core_event_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	(void)data;
	(void)seq;
	// ENOENT is routine: a meter stream whose target went away, or an object
	// removed while a request for it was in flight
	if (res == -ENOENT)
		g_debug("PipeWire error on id %u: %s (%s)", id, message, spa_strerror(res));
	else
		g_warning("PipeWire error on id %u: %s (%s)", id, message, spa_strerror(res));
	if (id == PW_ID_CORE && res == -EPIPE)
		g_warning("PipeWire connection lost");
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_event_error,
};

// Lifecycle //

gboolean a_backend_start(App *app)
{
	pw_init(NULL, NULL);

	PW *pw = g_new0(PW, 1);
	pw->app = app;
	app->backend = pw;
	pw->objs = g_hash_table_new(g_direct_hash, g_direct_equal);
	pw->links = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

	pw->loop = pw_loop_new(NULL);
	if (!pw->loop) return FALSE;
	pw->source = loop_source_new(pw->loop);

	pw->context = pw_context_new(pw->loop,
		pw_properties_new(PW_KEY_APP_NAME, "pavoldcontrol",
			PW_KEY_APP_ICON_NAME, "multimedia-volume-control", NULL), 0);
	if (!pw->context) return FALSE;

	pw->core = pw_context_connect(pw->context, NULL, 0);
	if (!pw->core) return FALSE;
	pw_core_add_listener(pw->core, &pw->core_listener, &core_events, pw);

	pw->registry = pw_core_get_registry(pw->core, PW_VERSION_REGISTRY, 0);
	if (!pw->registry) return FALSE;
	pw_registry_add_listener(pw->registry, &pw->registry_listener, &registry_events, pw);
	return TRUE;
}

void a_backend_stop(App *app)
{
	PW *pw = pw_of(app);
	if (!pw) return;

	if (pw->refresh_id)
		g_source_remove(pw->refresh_id);

	if (pw->registry)
	{
		spa_hook_remove(&pw->registry_listener);
	}

	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, pw->objs);
	while (g_hash_table_iter_next(&iter, &key, &value))
		obj_free(value, FALSE);
	g_hash_table_destroy(pw->objs);
	g_hash_table_destroy(pw->links);

	if (pw->registry)
		pw_proxy_destroy((struct pw_proxy *) pw->registry);
	if (pw->core)
	{
		spa_hook_remove(&pw->core_listener);
		pw_core_disconnect(pw->core);
	}
	if (pw->context)
		pw_context_destroy(pw->context);
	if (pw->source)
	{
		g_source_destroy(pw->source);
		g_source_unref(pw->source);
	}
	if (pw->loop)
		pw_loop_destroy(pw->loop);

	g_free(pw->default_sink);
	g_free(pw->default_source);
	g_free(pw);
	app->backend = NULL;
	pw_deinit();
}

// Actions //

static void node_set_props(App *app, guint32 id, gboolean volume, const AVolume *vol,
	gboolean mute)
{
	PW *pw = pw_of(app);
	Obj *o = pw ? obj_lookup(pw, id) : NULL;
	if (!o || o->kind != K_NODE) return;

	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod *pod;

	if (volume)
	{
		float vols[A_VOLUME_MAX_CHANNELS];
		uint32_t n = MIN((uint32_t) vol->channels, (uint32_t) A_VOLUME_MAX_CHANNELS);
		if (n == 0) return;
		for (uint32_t c = 0; c < n; c++)
		{
			double r = (double) vol->values[c] / (double) A_VOLUME_NORM;
			vols[c] = (float) (r * r * r);
		}
		pod = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
			SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, n, vols));
	}
	else
	{
		pod = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
			SPA_PROP_mute, SPA_POD_Bool(mute));
	}
	pw_node_set_param(o->proxy, SPA_PARAM_Props, 0, pod);
}

void a_backend_stream_set_volume(App *app, RowKind kind, guint32 index, const AVolume *vol)
{
	(void)kind;
	node_set_props(app, index, TRUE, vol, FALSE);
}

void a_backend_stream_set_mute(App *app, RowKind kind, guint32 index, gboolean mute)
{
	(void)kind;
	node_set_props(app, index, FALSE, NULL, mute);
}

void a_backend_device_set_volume(App *app, RowKind kind, guint32 index, const AVolume *vol)
{
	(void)kind;
	node_set_props(app, index, TRUE, vol, FALSE);
}

void a_backend_device_set_mute(App *app, RowKind kind, guint32 index, gboolean mute)
{
	(void)kind;
	node_set_props(app, index, FALSE, NULL, mute);
}

static void metadata_set(PW *pw, uint32_t subject, const char *key, const char *type,
	const char *value)
{
	if (!pw->metadata)
	{
		g_warning("PipeWire \"default\" metadata is not available (is a session manager running?)");
		return;
	}
	pw_metadata_set_property(pw->metadata->proxy, subject, key, type, value);
}

void a_backend_stream_move(App *app, RowKind kind, guint32 index, const gchar *device_name)
{
	PW *pw = pw_of(app);
	if (!pw) return;

	RowKind target_kind = (kind == ROW_PLAYBACK) ? ROW_OUTPUT_DEV : ROW_INPUT_DEV;
	Obj *target = find_node_by_name(pw, device_name, target_kind);
	if (!target) return;

	// current session managers read target.object (serial); older ones target.node (id)
	gchar *serial = g_strdup_printf("%u", target->serial);
	gchar *id = g_strdup_printf("%u", target->id);
	metadata_set(pw, index, "target.object", "Spa:Id", serial);
	metadata_set(pw, index, "target.node", "Spa:Id", id);
	g_free(serial);
	g_free(id);
}

void a_backend_device_set_default(App *app, RowKind kind, const gchar *name)
{
	PW *pw = pw_of(app);
	if (!pw || !name) return;

	gchar *escaped = g_strescape(name, NULL);
	gchar *json = g_strdup_printf("{\"name\":\"%s\"}", escaped);
	metadata_set(pw, 0, kind == ROW_OUTPUT_DEV ? "default.configured.audio.sink"
		: "default.configured.audio.source", "Spa:String:JSON", json);
	g_free(escaped);
	g_free(json);
}

void a_backend_device_set_port(App *app, RowKind kind, guint32 index, const gchar *port)
{
	(void)kind;
	PW *pw = pw_of(app);
	Obj *node = pw ? obj_lookup(pw, index) : NULL;
	if (!node || node->kind != K_NODE) return;

	Obj *dev = device_of(node);
	gint cpd = card_profile_device(node);
	if (!dev || cpd < 0) return;

	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, dev->routes);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		const RouteInfo *r = value;
		if (g_strcmp0(r->name, port) != 0) continue;

		uint8_t buffer[256];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		struct spa_pod *pod = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
			SPA_PARAM_ROUTE_index, SPA_POD_Int(r->index),
			SPA_PARAM_ROUTE_device, SPA_POD_Int(cpd),
			SPA_PARAM_ROUTE_save, SPA_POD_Bool(true));
		pw_device_set_param(dev->proxy, SPA_PARAM_Route, 0, pod);
		return;
	}
}

void a_backend_card_set_profile(App *app, guint32 index, const gchar *profile)
{
	PW *pw = pw_of(app);
	Obj *dev = pw ? obj_lookup(pw, index) : NULL;
	if (!dev || dev->kind != K_DEVICE) return;

	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, dev->profiles);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		const ProfileInfo *p = value;
		if (g_strcmp0(p->name, profile) != 0) continue;

		uint8_t buffer[256];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		struct spa_pod *pod = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
			SPA_PARAM_PROFILE_index, SPA_POD_Int(GPOINTER_TO_INT(key)),
			SPA_PARAM_PROFILE_save, SPA_POD_Bool(true));
		pw_device_set_param(dev->proxy, SPA_PARAM_Profile, 0, pod);
		return;
	}
}

// Meters //

static void meter_event_process(void *data)
{
	Meter *m = data;
	struct pw_buffer *b = pw_stream_dequeue_buffer(m->stream);
	if (!b) return;

	float peak = 0.0f;
	struct spa_data *d = &b->buffer->datas[0];
	if (d->data && d->chunk)
	{
		uint32_t offs = SPA_MIN(d->chunk->offset, d->maxsize);
		uint32_t size = SPA_MIN(d->chunk->size, d->maxsize - offs);
		const float *samples = SPA_PTROFF(d->data, offs, float);
		uint32_t n = size / sizeof(float);
		for (uint32_t i = 0; i < n; i++)
		{
			float v = fabsf(samples[i]);
			if (v > peak) peak = v;
		}
	}
	pw_stream_queue_buffer(m->stream, b);

	a_common_meter_report(m->row, peak);
}

static void meter_event_state_changed(void *data, enum pw_stream_state old,
	enum pw_stream_state state, const char *error)
{
	(void)old;
	(void)error;
	Meter *m = data;
	if (state != PW_STREAM_STATE_STREAMING)
		a_common_meter_report(m->row, 0.0f);
}

static const struct pw_stream_events meter_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = meter_event_state_changed,
	.process = meter_event_process,
};

void a_backend_meter_attach(App *app, StreamRow *row)
{
	PW *pw = pw_of(app);
	if (!pw || !pw->core || row->meter_handle) return;

	Obj *target = obj_lookup(pw, row->index);
	if (!target || target->kind != K_NODE) return;

	struct pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_NODE_NAME, METER_NODE_NAME,
		PW_KEY_STREAM_MONITOR, "true",
		PW_KEY_NODE_PASSIVE, "true", // don't wake up suspended devices
		PW_KEY_NODE_DONT_RECONNECT, "true",
		PW_KEY_NODE_LATENCY, "1024/48000",
		NULL);

	// a sink or a playback stream is metered through its output (monitor)
	if (row->kind == ROW_OUTPUT_DEV || row->kind == ROW_PLAYBACK)
		pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
	pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%u", target->serial);

	struct pw_stream *stream = pw_stream_new(pw->core, METER_NODE_NAME, props);
	if (!stream) return;

	Meter *m = g_new0(Meter, 1);
	m->pw = pw;
	m->row = row;
	m->stream = stream;
	pw_stream_add_listener(stream, &m->listener, &meter_events, m);

	uint8_t buffer[256];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_audio_info_raw info = {
		.format = SPA_AUDIO_FORMAT_F32,
		.channels = 1,
		.position = { SPA_AUDIO_CHANNEL_MONO },
	};
	const struct spa_pod *params[1];
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

	// no RT_PROCESS: process() runs on the main loop
	if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
		PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0)
	{
		spa_hook_remove(&m->listener);
		pw_stream_destroy(stream);
		g_free(m);
		return;
	}
	row->meter_handle = m;
}

void a_backend_meter_detach(StreamRow *row)
{
	Meter *m = row->meter_handle;
	if (!m) return;

	spa_hook_remove(&m->listener);
	pw_stream_destroy(m->stream);
	g_free(m);
	row->meter_handle = NULL;
}

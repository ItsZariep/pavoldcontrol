#include "settings.h"
#include <glib/gstdio.h>
#include <string.h>

#define SETTINGS_GROUP "pavoldcontrol"
#define SETTINGS_FILENAME "pavoldcontrol.ini"

static gchar *settings_path(void)
{
	return g_build_filename(g_get_user_config_dir(), SETTINGS_FILENAME, NULL);
}

static StreamFilter clamp_stream_filter(gint v)
{
	if (v < STREAM_FILTER_ALL || v > STREAM_FILTER_VIRTUAL) return STREAM_FILTER_ALL;
	return (StreamFilter) v;
}

static OutputFilter clamp_output_filter(gint v)
{
	if (v < OUTPUT_FILTER_ALL || v > OUTPUT_FILTER_VIRTUAL) return OUTPUT_FILTER_ALL;
	return (OutputFilter) v;
}

static InputFilter clamp_input_filter(gint v)
{
	if (v < INPUT_FILTER_ALL || v > INPUT_FILTER_MONITORS) return INPUT_FILTER_ALL;
	return (InputFilter) v;
}

void settings_load(Settings *s)
{
	memset(s, 0, sizeof(*s));
	s->playback_filter = STREAM_FILTER_ALL;
	s->recording_filter = STREAM_FILTER_ALL;
	s->output_filter = OUTPUT_FILTER_ALL;
	s->input_filter = INPUT_FILTER_ALL;
	s->show_volume_meters = TRUE;
	s->hide_unavailable_profiles = TRUE;
	s->mono_output_devices = FALSE;

	gchar *path = settings_path();
	GKeyFile *kf = g_key_file_new();
	GError *err = NULL;

	if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err))
	{
		// unreadable: keep defaults
		g_clear_error(&err);
		g_key_file_free(kf);
		g_free(path);
		return;
	}

	gint v;

	v = g_key_file_get_integer(kf, SETTINGS_GROUP, "playback_filter", &err);
	if (!err) s->playback_filter = clamp_stream_filter(v);
	g_clear_error(&err);

	v = g_key_file_get_integer(kf, SETTINGS_GROUP, "recording_filter", &err);
	if (!err) s->recording_filter = clamp_stream_filter(v);
	g_clear_error(&err);

	v = g_key_file_get_integer(kf, SETTINGS_GROUP, "output_filter", &err);
	if (!err) s->output_filter = clamp_output_filter(v);
	g_clear_error(&err);

	v = g_key_file_get_integer(kf, SETTINGS_GROUP, "input_filter", &err);
	if (!err) s->input_filter = clamp_input_filter(v);
	g_clear_error(&err);

	gboolean b;

	b = g_key_file_get_boolean(kf, SETTINGS_GROUP, "show_volume_meters", &err);
	if (!err) s->show_volume_meters = b;
	g_clear_error(&err);

	b = g_key_file_get_boolean(kf, SETTINGS_GROUP, "hide_unavailable_profiles", &err);
	if (!err) s->hide_unavailable_profiles = b;
	g_clear_error(&err);

	b = g_key_file_get_boolean(kf, SETTINGS_GROUP, "mono_output_devices", &err);
	if (!err) s->mono_output_devices = b;
	g_clear_error(&err);

	g_key_file_free(kf);
	g_free(path);
}

void settings_save(const Settings *s)
{
	GKeyFile *kf = g_key_file_new();

	g_key_file_set_integer(kf, SETTINGS_GROUP, "playback_filter", s->playback_filter);
	g_key_file_set_integer(kf, SETTINGS_GROUP, "recording_filter", s->recording_filter);
	g_key_file_set_integer(kf, SETTINGS_GROUP, "output_filter", s->output_filter);
	g_key_file_set_integer(kf, SETTINGS_GROUP, "input_filter", s->input_filter);

	g_key_file_set_boolean(kf, SETTINGS_GROUP, "show_volume_meters", s->show_volume_meters);
	g_key_file_set_boolean(kf, SETTINGS_GROUP, "hide_unavailable_profiles", s->hide_unavailable_profiles);
	g_key_file_set_boolean(kf, SETTINGS_GROUP, "mono_output_devices", s->mono_output_devices);

	gchar *dir = g_build_filename(g_get_user_config_dir(), NULL);
	g_mkdir_with_parents(dir, 0700);
	g_free(dir);

	gchar *path = settings_path();
	GError *err = NULL;
	if (!g_key_file_save_to_file(kf, path, &err))
	{
		g_warning("Could not save settings to %s: %s", path, err->message);
		g_clear_error(&err);
	}

	g_free(path);
	g_key_file_free(kf);
}

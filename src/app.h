#ifndef APP_H
#define APP_H

#include <gtk/gtk.h>
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>

#include "settings.h"

typedef enum
{
	ROW_PLAYBACK, //sink-input
	ROW_RECORDING, // source-output
	ROW_OUTPUT_DEV, // sink
	ROW_INPUT_DEV, // source
	ROW_CARD // card
} RowKind;

#define ROW_MAX_CHANNELS 8

typedef struct App App;

typedef struct {
	RowKind kind;
	guint32 index; // object index
	App *app;

	GtkWidget *root; // top-level container inserted into the list box
	GtkWidget *name_label;
	GtkWidget *combo_label; // "on"/"from" text next to combo, NULL for cards
	GtkWidget *combo; // "on/from/port" selector, NULL for cards
	GtkWidget *mute_btn;
	GtkWidget *lock_btn;
	GtkWidget *preferred_btn; // device rows only, may be NULL
	GtkWidget *profile_combo; // card rows only
	GtkWidget *profile_label;
	GtkWidget *slider_box;
	GtkWidget *sliders[ROW_MAX_CHANNELS];
	GtkWidget *meter; // GtkLevelBar, may be NULL

	guint8 n_channels;
	pa_cvolume volume;
	gboolean channels_locked;
	gboolean updating; // guards signal feedback loops while we set values

	gchar *device_name; // sink/source name, for ROW_OUTPUT_DEV/ROW_INPUT_DEV
	pa_stream *meter_stream;

	gboolean is_virtual;
	gboolean is_hardware;
	gboolean is_monitor;

	guint volume_debounce_id;
} StreamRow;

struct App {
	GtkWidget *window;
	GtkWidget *notebook;

	GtkWidget *playback_box;
	GtkWidget *recording_box;
	GtkWidget *sinks_box;
	GtkWidget *sources_box;
	GtkWidget *cards_box;

	GtkWidget *playback_placeholder;
	GtkWidget *recording_placeholder;
	GtkWidget *sinks_placeholder;
	GtkWidget *sources_placeholder;
	GtkWidget *cards_placeholder;

	// Settings
	GtkWidget *playback_filter_combo;
	GtkWidget *recording_filter_combo;
	GtkWidget *sinks_filter_combo;
	GtkWidget *sources_filter_combo;

	GtkWidget *meters_check;
	GtkWidget *hide_profiles_check;
	GtkWidget *mono_check;
	Settings settings;

	// index -> StreamRow* (owns the StreamRow)
	GHashTable *playback_rows;
	GHashTable *recording_rows;
	GHashTable *sink_rows;
	GHashTable *source_rows;
	GHashTable *card_rows;

	// name -> g_strdup'd description, used to populate combo boxes
	GHashTable *sink_names; // sink name -> description
	GHashTable *source_names; // source name -> description

	GHashTable *sink_index_to_name;
	GHashTable *source_index_to_name;

	pa_glib_mainloop *pa_loop;
	pa_context *pa_ctx;

	gchar *default_sink_name;
	gchar *default_source_name;
	gboolean shutting_down;
};

typedef struct {
	gchar *id;
	gchar *label;
} RowChoice;

RowChoice *row_choice_new(const gchar *id, const gchar *label);
void row_choice_free(gpointer data);

void row_free(gpointer data);
StreamRow *row_new(App *app, RowKind kind, guint32 index, GtkWidget *parent_box);
void row_set_name(StreamRow *row, const gchar *name, const gchar *icon_name);
void row_set_channels(StreamRow *row, uint8_t n_channels, const pa_cvolume *vol);
void row_set_mute(StreamRow *row, gboolean mute);

void row_set_choices(StreamRow *row, GList *choices, const gchar *active_id);
void row_set_profiles(StreamRow *row, GList *choices, const gchar *active_id);
void row_set_preferred(StreamRow *row, gboolean is_default);
void row_set_device_name(StreamRow *row, const gchar *name);

void row_set_force_mono(StreamRow *row, gboolean force);

void app_apply_filter(App *app, RowKind kind);
void app_apply_meter_setting(App *app);

#endif

#include "a_common.h"
#include "a_backend.h"

// Helpers //

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

StreamRow *a_common_ensure_row(App *app, RowKind kind, guint32 index)
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

void a_common_remove_row(App *app, RowKind kind, guint32 index)
{
	g_hash_table_remove(rows_for(app, kind), GUINT_TO_POINTER(index));
	update_placeholder(app, kind);
}

static gint choice_compare(gconstpointer a, gconstpointer b)
{
	const RowChoice *ca = a;
	const RowChoice *cb = b;
	return g_strcmp0(ca->label, cb->label);
}

GList *a_common_choices_from_names(GHashTable *names)
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

// Meters //

void a_common_meter_report(StreamRow *row, float peak)
{
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
			guint32 max_vol = 0;
			for (guint8 c = 0; c < row->n_channels; c++)
				if (row->volume.values[c] > max_vol)
					max_vol = row->volume.values[c];
			peak *= (float) max_vol / (float) A_VOLUME_NORM;
		}
	}

	if (row->meter)
		gtk_level_bar_set_value(GTK_LEVEL_BAR(row->meter), (double) peak);
}

// Filtering / settings application //

gboolean a_common_row_passes_filter(App *app, StreamRow *row)
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
	GHashTable *table = NULL;
	switch (kind)
	{
		case ROW_PLAYBACK: table = app->playback_rows; break;
		case ROW_RECORDING: table = app->recording_rows; break;
		case ROW_OUTPUT_DEV: table = app->sink_rows; break;
		case ROW_INPUT_DEV: table = app->source_rows; break;
		case ROW_CARD: table = app->card_rows; break;
	}
	if (!table) return;

	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, table);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		StreamRow *row = value;
		gtk_widget_set_visible(row->root, a_common_row_passes_filter(app, row));
	}
}

void app_apply_meter_setting(App *app)
{
	GHashTable *tables[] = { app->playback_rows, app->sink_rows, app->source_rows };

	for (size_t t = 0; t < G_N_ELEMENTS(tables); t++)
	{
		GHashTableIter iter;
		gpointer key, value;
		g_hash_table_iter_init(&iter, tables[t]);
		while (g_hash_table_iter_next(&iter, &key, &value))
		{
			StreamRow *row = value;

			if (row->meter)
				gtk_widget_set_visible(row->meter, app->settings.show_volume_meters);

			if (app->settings.show_volume_meters && !row->meter_handle)
				a_backend_meter_attach(app, row);
			else if (!app->settings.show_volume_meters && row->meter_handle)
				a_backend_meter_detach(row);
		}
	}
}

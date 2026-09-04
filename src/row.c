#include "app.h"
#include "pa_backend.h"
#include <string.h>
#include <math.h>
#include <glib/gi18n.h>

RowChoice *row_choice_new(const gchar *id, const gchar *label)
{
	RowChoice *c = g_new0(RowChoice, 1);
	c->id = g_strdup(id);
	c->label = g_strdup(label);
	return c;
}

void row_choice_free(gpointer data)
{
	RowChoice *c = data;
	if (!c) return;
	g_free(c->id);
	g_free(c->label);
	g_free(c);
}

// Signal handlers

typedef struct
{
	App *app;
	StreamRow *row;
} RowCtx;

static void free_row_ctx(gpointer data, GClosure *closure)
{
	(void)closure;
	g_free(data);
}

static pa_volume_t gtk_value_to_pa(double v)
{
	return (pa_volume_t) lround(v * (double) PA_VOLUME_NORM / 100.0);
}

static gchar *format_slider_value(GtkScale *scale, gdouble value, gpointer user_data)
{
	(void)scale;
	(void)user_data;

	pa_volume_t vol = gtk_value_to_pa(value);
	double db = pa_sw_volume_to_dB(vol);
	if (db <= PA_DECIBEL_MININFTY)
	{
		return g_strdup_printf("%.0f%% (-\xe2\x88\x9e dB)", value);
	}
	return g_strdup_printf("%.0f%% (%.2fdB)", value, db);
}

static void update_slider_visibility(StreamRow *row)
{
	gboolean show_all = !row->channels_locked && row->n_channels > 1;
	for (guint8 c = 0; c < row->n_channels; c++)
	{
		if (!row->sliders[c]) continue;
		gtk_widget_set_visible(row->sliders[c], c == 0 || show_all);
	}
}

static gboolean apply_volume_debounced(gpointer user_data)
{
	RowCtx *ctx = user_data;
	StreamRow *row = ctx->row;

	row->volume_debounce_id = 0;

	if (row->kind == ROW_PLAYBACK || row->kind == ROW_RECORDING)
	{
		pa_backend_stream_set_volume(ctx->app, row->kind, row->index, &row->volume);
	}
	else
	{
		pa_backend_device_set_volume(ctx->app, row->kind, row->index, &row->volume);
	}

	return G_SOURCE_REMOVE;
}

static void apply_volume_from_sliders(RowCtx *ctx, gint changed_channel)
{
	StreamRow *row = ctx->row;

	if (row->updating)
		return;

	double changed_value = gtk_range_get_value(GTK_RANGE(row->sliders[changed_channel]));

	pa_volume_t changed_vol = gtk_value_to_pa(changed_value);

	row->updating = TRUE;

	for (guint8 c = 0; c < row->n_channels; c++)
	{
		if (row->channels_locked || row->n_channels == 1)
		{
			gtk_range_set_value(GTK_RANGE(row->sliders[c]), changed_value);

			row->volume.values[c] = changed_vol;
		}
		else if ((gint)c == changed_channel)
		{
			row->volume.values[c] = changed_vol;
		}
	}
	row->updating = FALSE;
}

static void on_slider_changed(GtkRange *range, gpointer user_data)
{
	RowCtx *ctx = user_data;
	StreamRow *row = ctx->row;

	for (guint8 c = 0; c < row->n_channels; c++)
	{
		if (row->sliders[c] == GTK_WIDGET(range))
		{
			apply_volume_from_sliders(ctx, c);

			if (row->volume_debounce_id != 0)
			{
				g_source_remove(row->volume_debounce_id);
				row->volume_debounce_id = 0;
			}
			row->volume_debounce_id = g_timeout_add(50, apply_volume_debounced, ctx);

			return;
		}
	}
}

static void on_mute_toggled(GtkToggleButton *btn, gpointer user_data)
{
	RowCtx *ctx = user_data;
	if (ctx->row->updating) return;
	gboolean mute = gtk_toggle_button_get_active(btn);

	if (ctx->row->kind == ROW_PLAYBACK || ctx->row->kind == ROW_RECORDING)
		pa_backend_stream_set_mute(ctx->app, ctx->row->kind, ctx->row->index, mute);
	else
		pa_backend_device_set_mute(ctx->app, ctx->row->kind, ctx->row->index, mute);
}

static void on_lock_toggled(GtkToggleButton *btn, gpointer user_data)
{
	RowCtx *ctx = user_data;
	ctx->row->channels_locked = gtk_toggle_button_get_active(btn);
	update_slider_visibility(ctx->row);
}

static void on_combo_changed(GtkComboBox *combo, gpointer user_data)
{
	RowCtx *ctx = user_data;
	if (ctx->row->updating) return;
	const gchar *id = gtk_combo_box_get_active_id(combo);
	if (!id) return;

	switch (ctx->row->kind)
	{
		case ROW_PLAYBACK:
		case ROW_RECORDING:
			pa_backend_stream_move(ctx->app, ctx->row->kind, ctx->row->index, id);
			break;
		case ROW_OUTPUT_DEV:
		case ROW_INPUT_DEV:
			pa_backend_device_set_port(ctx->app, ctx->row->kind, ctx->row->index, id);
			break;
		case ROW_CARD:
			pa_backend_card_set_profile(ctx->app, ctx->row->index, id);
			break;
	}
}

static void on_preferred_clicked(GtkButton *btn, gpointer user_data)
{
	(void)btn;
	RowCtx *ctx = user_data;
	if (ctx->row->device_name)
	{
		pa_backend_device_set_default(ctx->app, ctx->row->kind, ctx->row->device_name);
	}
}

// construction //

static GtkWidget *make_icon(const gchar *icon_name)
{
	return gtk_image_new_from_icon_name(icon_name ? icon_name : "audio-card",
		GTK_ICON_SIZE_LARGE_TOOLBAR);
}

StreamRow *row_new(App *app, RowKind kind, guint32 index, GtkWidget *parent_box)
{
	StreamRow *row = g_new0(StreamRow, 1);
	row->kind = kind;
	row->index = index;
	row->app = app;
	row->channels_locked = TRUE;

	RowCtx *ctx = g_new0(RowCtx, 1);
	ctx->app = app;
	ctx->row = row;

	GtkWidget *frame = gtk_frame_new(NULL);
	gtk_widget_set_margin_start(frame, 6);
	gtk_widget_set_margin_end(frame, 6);
	gtk_widget_set_margin_top(frame, 4);
	gtk_widget_set_margin_bottom(frame, 4);

	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
	gtk_container_add(GTK_CONTAINER(frame), outer);

	// header row: icon, name, [on/from/port combo], mute, lock, preferred
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_pack_start(GTK_BOX(outer), header, FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(header), make_icon(NULL), FALSE, FALSE, 0);

	row->name_label = gtk_label_new(NULL);
	gtk_label_set_ellipsize(GTK_LABEL(row->name_label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_halign(row->name_label, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(header), row->name_label, TRUE, TRUE, 0);

	if (kind == ROW_CARD)
	{
		GtkWidget *lbl = gtk_label_new(_("Profile"));
		row->profile_label = lbl;
		gtk_box_pack_start(GTK_BOX(header), lbl, FALSE, FALSE, 0);
		row->profile_combo = gtk_combo_box_text_new();
		g_signal_connect_data(row->profile_combo, "changed",
			G_CALLBACK(on_combo_changed), ctx, free_row_ctx, 0);
		gtk_box_pack_start(GTK_BOX(header), row->profile_combo, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(parent_box), frame, FALSE, FALSE, 0);
		gtk_widget_show_all(frame);

		if (row->meter)
			gtk_widget_set_visible(row->meter, app->settings.show_volume_meters);

		row->root = frame;
		return row;
	}

	const gchar *combo_prefix = (kind == ROW_PLAYBACK) ? _("on") : _("from");
	row->combo_label = gtk_label_new(combo_prefix);
	gtk_box_pack_start(GTK_BOX(header), row->combo_label, FALSE, FALSE, 0);
	row->combo = gtk_combo_box_text_new();
	gtk_widget_set_size_request(row->combo, 140, -1);
	g_signal_connect_data(row->combo, "changed", G_CALLBACK(on_combo_changed), ctx,
		NULL, 0);
	gtk_box_pack_start(GTK_BOX(header), row->combo, FALSE, FALSE, 0);

	// Mute
	row->mute_btn = gtk_toggle_button_new();
	gtk_button_set_image(GTK_BUTTON(row->mute_btn),
		gtk_image_new_from_icon_name("audio-volume-muted-symbolic",
		GTK_ICON_SIZE_BUTTON));
	g_signal_connect_data(row->mute_btn, "toggled",
		G_CALLBACK(on_mute_toggled), ctx, NULL, 0);
	gtk_box_pack_start(GTK_BOX(header), row->mute_btn, FALSE, FALSE, 0);


	// Lock
	row->lock_btn = gtk_toggle_button_new();
	gtk_button_set_image(GTK_BUTTON(row->lock_btn),
		gtk_image_new_from_icon_name("changes-prevent-symbolic",
		GTK_ICON_SIZE_BUTTON));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->lock_btn), TRUE);
	gtk_widget_set_tooltip_text(row->lock_btn, _("Lock channels together"));

	RowCtx *ctx3 = g_new0(RowCtx, 1);
	ctx3->app = app;
	ctx3->row = row;
	g_signal_connect_data(row->lock_btn, "toggled",
		G_CALLBACK(on_lock_toggled), ctx3, free_row_ctx, 0);
	gtk_box_pack_start(GTK_BOX(header), row->lock_btn, FALSE, FALSE, 0);


	// Preferred
	if (kind == ROW_OUTPUT_DEV || kind == ROW_INPUT_DEV)
	{
		row->preferred_btn = gtk_toggle_button_new();

		gtk_button_set_image(GTK_BUTTON(row->preferred_btn),
			gtk_image_new_from_icon_name("emblem-default-symbolic",
			GTK_ICON_SIZE_BUTTON));

		gtk_widget_set_tooltip_text(row->preferred_btn, _("Preferred"));

		RowCtx *ctx2 = g_new0(RowCtx, 1);
		ctx2->app = app;
		ctx2->row = row;
		g_signal_connect_data(row->preferred_btn, "clicked",
			G_CALLBACK(on_preferred_clicked), ctx2, free_row_ctx, 0);

		gtk_box_pack_start(GTK_BOX(header), row->preferred_btn,
			FALSE, FALSE, 0);
	}

	// Channel sliders
	row->slider_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_box_pack_start(GTK_BOX(outer), row->slider_box, FALSE, FALSE, 0);

	// peak meter, playback and device rows
	if (kind == ROW_PLAYBACK || kind == ROW_OUTPUT_DEV || kind == ROW_INPUT_DEV)
	{
		row->meter = gtk_level_bar_new_for_interval(0.0, 1.0);
		gtk_level_bar_set_mode(GTK_LEVEL_BAR(row->meter), GTK_LEVEL_BAR_MODE_CONTINUOUS);

		// Built-in "low"/"high" offsets color the low end red, which is
		// backwards for a peak meter. Replace with an own "clip" offset
		gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(row->meter), GTK_LEVEL_BAR_OFFSET_LOW);
		gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(row->meter), GTK_LEVEL_BAR_OFFSET_HIGH);
		gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(row->meter), "clip", 0.8);

		gtk_widget_set_visible(row->meter, app->settings.show_volume_meters);
		gtk_box_pack_start(GTK_BOX(outer), row->meter, FALSE, FALSE, 0);
	}

	gtk_box_pack_start(GTK_BOX(parent_box), frame, FALSE, FALSE, 0);
	gtk_widget_show_all(frame);

	if (row->meter)
		gtk_widget_set_visible(row->meter, app->settings.show_volume_meters);

	row->root = frame;

	// g_object_set_data keeps ctx reachable for on_slider_change
	g_object_set_data_full(G_OBJECT(frame), "row-ctx", ctx, g_free);
	return row;
}

void row_set_force_mono(StreamRow *row, gboolean force)
{
	if (row->kind != ROW_OUTPUT_DEV) return;
	if (!row->lock_btn) return;

	if (force)
	{
		row->channels_locked = TRUE;
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->lock_btn), TRUE);
		gtk_widget_set_sensitive(row->lock_btn, FALSE);
	}
	else
	{
		gtk_widget_set_sensitive(row->lock_btn, TRUE);
	}
	update_slider_visibility(row);
}

void row_set_name(StreamRow *row, const gchar *name, const gchar *icon_name)
{
	gtk_label_set_text(GTK_LABEL(row->name_label), name ? name : "");
	if (icon_name)
	{
		GtkWidget *parent = gtk_widget_get_parent(row->name_label);
		GList *children = gtk_container_get_children(GTK_CONTAINER(parent));
		if (children)
		{
			gtk_image_set_from_icon_name(GTK_IMAGE(children->data), icon_name,
				GTK_ICON_SIZE_LARGE_TOOLBAR);
		}
		g_list_free(children);
	}
}

static void destroy_child(GtkWidget *child, gpointer user_data)
{
	(void)user_data;
	gtk_widget_destroy(child);
}

void row_set_channels(StreamRow *row, uint8_t n_channels, const pa_cvolume *vol)
{
	if (n_channels > ROW_MAX_CHANNELS) n_channels = ROW_MAX_CHANNELS;

	RowCtx *ctx = g_object_get_data(G_OBJECT(row->root), "row-ctx");

	if (row->n_channels != n_channels)
	{
		gtk_container_foreach(GTK_CONTAINER(row->slider_box), destroy_child, NULL);
		for (guint8 c = 0; c < n_channels; c++)
		{
			GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0,
				150.0, 1.0);
			gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
			gtk_widget_set_hexpand(scale, TRUE);
			g_signal_connect(scale, "format-value", G_CALLBACK(format_slider_value), NULL);
			if (ctx)
			{
				g_signal_connect(scale, "value-changed", G_CALLBACK(on_slider_changed), ctx);
			}
			gtk_box_pack_start(GTK_BOX(row->slider_box), scale, FALSE, FALSE, 0);
			row->sliders[c] = scale;
		}
		gtk_widget_show_all(row->slider_box);
		row->n_channels = n_channels;
		update_slider_visibility(row);
	}

	row->volume = *vol;
	row->updating = TRUE;
	for (guint8 c = 0; c < n_channels; c++)
	{
		double pct = 100.0 * (double) vol->values[c] / (double) PA_VOLUME_NORM;
		gtk_range_set_value(GTK_RANGE(row->sliders[c]), pct);
	}
	row->updating = FALSE;
}

void row_set_mute(StreamRow *row, gboolean mute)
{
	if (!row->mute_btn) return;
	gboolean was_updating = row->updating;
	row->updating = TRUE;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->mute_btn), mute);
	row->updating = was_updating;
}

void row_set_device_name(StreamRow *row, const gchar *name)
{
	g_free(row->device_name);
	row->device_name = g_strdup(name);
}

void row_set_preferred(StreamRow *row, gboolean is_default)
{
	if (!row->preferred_btn) return;
	gboolean was_updating = row->updating;
	row->updating = TRUE;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->preferred_btn), is_default);
	row->updating = was_updating;
}

static void fill_combo(GtkComboBoxText *combo, GList *choices, const gchar *active_id)
{
	gtk_combo_box_text_remove_all(combo);
	for (GList *l = choices; l; l = l->next)
	{
		RowChoice *c = l->data;
		gtk_combo_box_text_append(combo, c->id, c->label);
	}
	if (active_id)
	{
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), active_id);
	}
}

void row_set_choices(StreamRow *row, GList *choices, const gchar *active_id)
{
	if (!row->combo)
		return;
	gboolean has_choices = choices != NULL;
	gtk_widget_set_visible(row->combo, has_choices);
	if (row->combo_label)
		gtk_widget_set_visible(row->combo_label, has_choices);
	if (!has_choices)
		return;
	gboolean was_updating = row->updating;
	row->updating = TRUE;
	fill_combo(GTK_COMBO_BOX_TEXT(row->combo), choices, active_id);
	row->updating = was_updating;
}

void row_set_profiles(StreamRow *row, GList *choices, const gchar *active_id)
{
	if (!row->profile_combo)
		return;
	gboolean has_choices = choices != NULL;
	gtk_widget_set_visible(row->profile_combo, has_choices);
	if (row->profile_label)
		gtk_widget_set_visible(row->profile_label, has_choices);
	if (!has_choices)
		return;
	gboolean was_updating = row->updating;
	row->updating = TRUE;
	fill_combo(GTK_COMBO_BOX_TEXT(row->profile_combo), choices, active_id);
	row->updating = was_updating;
}

void row_free(gpointer data)
{
	StreamRow *row = data;

	if (!row)
		return;
	if (row->volume_debounce_id != 0)
		g_source_remove(row->volume_debounce_id);
	if (row->meter_stream)
		pa_backend_meter_detach(row);
	if (row->root && row->app && !row->app->shutting_down)
		gtk_widget_destroy(row->root);
	g_free(row->device_name);
	g_free(row);
}

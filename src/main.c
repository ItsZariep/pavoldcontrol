#include "app.h"
#include "pa_backend.h"
#include "settings.h"

#include <glib/gi18n.h>
#include <locale.h>

#ifndef GETTEXT_PACKAGE
#define GETTEXT_PACKAGE "pavoldcontrol"
#endif

#ifndef LOCALEDIR
#define LOCALEDIR "/usr/share/locale"
#endif

static GtkWidget *make_placeholder(const gchar *text)
{
	GtkWidget *lbl = gtk_label_new(text);
	gtk_widget_set_margin_top(lbl, 24);
	gtk_widget_set_margin_bottom(lbl, 24);
	gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "dim-label");
	return lbl;
}

static GtkWidget *make_page(const gchar *placeholder_text, GtkWidget **out_box,
	GtkWidget **out_placeholder)
{
	GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *placeholder = make_placeholder(placeholder_text);
	gtk_box_pack_start(GTK_BOX(box), placeholder, FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(scroller), box);

	*out_box = box;
	*out_placeholder = placeholder;
	return scroller;
}

// Footer (combobox //)

static void on_filter_changed(GtkComboBox *combo, gpointer user_data)
{
	App *app = user_data;
	RowKind kind = (RowKind)(guintptr) g_object_get_data(G_OBJECT(combo), "row-kind");
	gint active = gtk_combo_box_get_active(combo);
	if (active < 0) return;

	switch (kind)
	{
		case ROW_PLAYBACK: app->settings.playback_filter = (StreamFilter) active; break;
		case ROW_RECORDING: app->settings.recording_filter = (StreamFilter) active; break;
		case ROW_OUTPUT_DEV: app->settings.output_filter = (OutputFilter) active; break;
		case ROW_INPUT_DEV: app->settings.input_filter = (InputFilter) active; break;
		default: return;
	}

	app_apply_filter(app, kind);
	settings_save(&app->settings);
}

static GtkWidget *make_filter_footer(App *app, RowKind kind, const gchar * const *labels,
	gint n_labels, gint active, GtkWidget **out_combo)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_box_pack_start(GTK_BOX(box), sep, FALSE, FALSE, 4);

	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(row), 6);
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

	GtkWidget *label = gtk_label_new(_("Show:"));
	gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);

	GtkWidget *combo = gtk_combo_box_text_new();
	for (gint i = 0; i < n_labels; i++)
	{
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), _(labels[i]));
	}
	gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
	g_object_set_data(G_OBJECT(combo), "row-kind", GUINT_TO_POINTER(kind));
	g_signal_connect(combo, "changed", G_CALLBACK(on_filter_changed), app);
	gtk_box_pack_start(GTK_BOX(row), combo, TRUE, TRUE, 0);

	if (out_combo) *out_combo = combo;
	return box;
}

// Checkboxes

static void on_meters_toggled(GtkToggleButton *btn, gpointer user_data)
{
	App *app = user_data;
	app->settings.show_volume_meters = gtk_toggle_button_get_active(btn);
	app_apply_meter_setting(app);
	settings_save(&app->settings);
}

static void on_hide_profiles_toggled(GtkToggleButton *btn, gpointer user_data)
{
	App *app = user_data;
	app->settings.hide_unavailable_profiles = gtk_toggle_button_get_active(btn);
	settings_save(&app->settings);
	pa_backend_refresh_cards(app);
}

static void on_mono_toggled(GtkToggleButton *btn, gpointer user_data)
{
	App *app = user_data;
	app->settings.mono_output_devices = gtk_toggle_button_get_active(btn);
	settings_save(&app->settings);

	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, app->sink_rows);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		StreamRow *row = value;
		row_set_force_mono(row, app->settings.mono_output_devices);
	}
}

static GtkWidget *make_config_footer(App *app)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_box_pack_start(GTK_BOX(box), sep, FALSE, FALSE, 4);

	GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_container_set_border_width(GTK_CONTAINER(inner), 6);
	gtk_box_pack_start(GTK_BOX(box), inner, FALSE, FALSE, 0);

	app->meters_check = gtk_check_button_new_with_label(_("Show volume meters"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->meters_check),
		app->settings.show_volume_meters);
	g_signal_connect(app->meters_check, "toggled", G_CALLBACK(on_meters_toggled), app);
	gtk_box_pack_start(GTK_BOX(inner), app->meters_check, FALSE, FALSE, 0);

	app->hide_profiles_check = gtk_check_button_new_with_label(_("Hide unavailable card profiles"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->hide_profiles_check),
		app->settings.hide_unavailable_profiles);
	g_signal_connect(app->hide_profiles_check, "toggled",
		G_CALLBACK(on_hide_profiles_toggled), app);
	gtk_box_pack_start(GTK_BOX(inner), app->hide_profiles_check, FALSE, FALSE, 0);

	app->mono_check = gtk_check_button_new_with_label(_("Configure all output devices as mono"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->mono_check),
		app->settings.mono_output_devices);
	g_signal_connect(app->mono_check, "toggled", G_CALLBACK(on_mono_toggled), app);
	gtk_box_pack_start(GTK_BOX(inner), app->mono_check, FALSE, FALSE, 0);

	return box;
}

// Window: Notebook //

static void build_ui(App *app)
{
	app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(app->window), _("Volume Control"));
	gtk_window_set_default_size(GTK_WINDOW(app->window), 640, 480);

	app->notebook = gtk_notebook_new();
	gtk_container_add(GTK_CONTAINER(app->window), app->notebook);

	struct
	{
		const gchar *label;
		const gchar *placeholder;
		GtkWidget **box;
		GtkWidget **placeholder_widget;
	}

	pages[] =
	{
		{_("Playback"), _("No applications are playing audio."),
			&app->playback_box, &app->playback_placeholder},
		{_("Recording"), _("No applications are recording audio."),
			&app->recording_box, &app->recording_placeholder},
		{_("Output Devices"), _("No output devices available."),
			&app->sinks_box, &app->sinks_placeholder},
		{_("Input Devices"), _("No input devices available."),
			&app->sources_box, &app->sources_placeholder},
		{_("Configuration"), _("No audio hardware detected."),
			&app->cards_box, &app->cards_placeholder},
	};

	for (size_t i = 0; i < G_N_ELEMENTS(pages); i++)
	{
		GtkWidget *scroller = make_page(pages[i].placeholder, pages[i].box,
			pages[i].placeholder_widget);

		GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		gtk_box_pack_start(GTK_BOX(page), scroller, TRUE, TRUE, 0);

		if (i == 0) // Playback
		{
			static const gchar *labels[] = {N_("All Streams"), N_("Applications"), N_("Virtual Streams")};
			GtkWidget *footer = make_filter_footer(app, ROW_PLAYBACK, labels, 3,
				app->settings.playback_filter, &app->playback_filter_combo);
			gtk_box_pack_start(GTK_BOX(page), footer, FALSE, FALSE, 0);
		}
		else if (i == 1) // Recording
		{
			static const gchar *labels[] = {N_("All Streams"), N_("Applications"), N_("Virtual Streams")};
			GtkWidget *footer = make_filter_footer(app, ROW_RECORDING, labels, 3,
				app->settings.recording_filter, &app->recording_filter_combo);
			gtk_box_pack_start(GTK_BOX(page), footer, FALSE, FALSE, 0);
		}
		else if (i == 2) // Output Devices
		{
			static const gchar *labels[] =
				{N_("All output devices"), N_("Hardware output devices"), N_("Virtual output devices")};
			GtkWidget *footer = make_filter_footer(app, ROW_OUTPUT_DEV, labels, 3,
				app->settings.output_filter, &app->sinks_filter_combo);
			gtk_box_pack_start(GTK_BOX(page), footer, FALSE, FALSE, 0);
		}
		else if (i == 3) // Input Devices
		{
			static const gchar *labels[] = {N_("All input devices"), N_("All except monitors"),
				N_("Hardware input devices"), N_("Virtual input devices"), N_("Monitors")};
			GtkWidget *footer = make_filter_footer(app, ROW_INPUT_DEV, labels, 5,
				app->settings.input_filter, &app->sources_filter_combo);
			gtk_box_pack_start(GTK_BOX(page), footer, FALSE, FALSE, 0);
		}
		else if (i == 4) // Configuration
		{
			GtkWidget *footer = make_config_footer(app);
			gtk_box_pack_start(GTK_BOX(page), footer, FALSE, FALSE, 0);
		}

		gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), page,
			gtk_label_new(pages[i].label));
	}

	g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	gtk_widget_show_all(app->window);
}

static App *app_new(void)
{
	App *app = g_new0(App, 1);

	settings_load(&app->settings);

	app->playback_rows = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, row_free);
	app->recording_rows = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, row_free);
	app->sink_rows = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, row_free);
	app->source_rows = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, row_free);
	app->card_rows = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, row_free);

	app->sink_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	app->source_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	app->sink_index_to_name = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	app->source_index_to_name =
		g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

	return app;
}

static void app_free(App *app)
{
	pa_backend_stop(app);
	settings_save(&app->settings);

	app->shutting_down = TRUE;

	g_hash_table_destroy(app->playback_rows);
	g_hash_table_destroy(app->recording_rows);
	g_hash_table_destroy(app->sink_rows);
	g_hash_table_destroy(app->source_rows);
	g_hash_table_destroy(app->card_rows);

	g_hash_table_destroy(app->sink_names);
	g_hash_table_destroy(app->source_names);
	g_hash_table_destroy(app->sink_index_to_name);
	g_hash_table_destroy(app->source_index_to_name);

	g_free(app->default_sink_name);
	g_free(app->default_source_name);
	g_free(app);
}

int main(int argc, char **argv)
{
	setlocale(LC_ALL, "");
	bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
	textdomain(GETTEXT_PACKAGE);

	gtk_init(&argc, &argv);

	App *app = app_new();
	build_ui(app);

	if (!pa_backend_start(app))
	{
		GtkWidget *dialog = gtk_message_dialog_new(
			GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
			GTK_BUTTONS_CLOSE, "%s", _("Could not connect to PulseAudio."));
		gtk_dialog_run(GTK_DIALOG(dialog));
		gtk_widget_destroy(dialog);
		app_free(app);
		return 1;
	}
	gtk_main();

	app_free(app);
	return 0;
}

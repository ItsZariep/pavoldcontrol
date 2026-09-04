#ifndef SETTINGS_H
#define SETTINGS_H

#include <glib.h>

typedef enum
{
	STREAM_FILTER_ALL,
	STREAM_FILTER_APPLICATIONS,
	STREAM_FILTER_VIRTUAL
} StreamFilter;

typedef enum
{
	OUTPUT_FILTER_ALL,
	OUTPUT_FILTER_HARDWARE,
	OUTPUT_FILTER_VIRTUAL
} OutputFilter;

typedef enum
{
	INPUT_FILTER_ALL,
	INPUT_FILTER_NO_MONITORS,
	INPUT_FILTER_HARDWARE,
	INPUT_FILTER_VIRTUAL,
	INPUT_FILTER_MONITORS
} InputFilter;

typedef struct
{
	StreamFilter playback_filter;
	StreamFilter recording_filter;
	OutputFilter output_filter;
	InputFilter input_filter;

	gboolean show_volume_meters;
	gboolean hide_unavailable_profiles;
	gboolean mono_output_devices;
} Settings;

void settings_load(Settings *s);
void settings_save(const Settings *s);

#endif

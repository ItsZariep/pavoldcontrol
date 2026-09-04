#ifndef PA_BACKEND_H
#define PA_BACKEND_H

#include "app.h"

gboolean pa_backend_start(App *app);
void pa_backend_stop(App *app);

// Streams (sink-inputs / source-outputs) //
void pa_backend_stream_set_volume(App *app, RowKind kind, guint32 index,const pa_cvolume *cvol);
void pa_backend_stream_set_mute(App *app, RowKind kind, guint32 index, gboolean mute);
void pa_backend_stream_move(App *app, RowKind kind, guint32 index, const gchar *device_name);

// Devices (sinks / sources) //
void pa_backend_device_set_volume(App *app, RowKind kind, guint32 index,const pa_cvolume *cvol);
void pa_backend_device_set_mute(App *app, RowKind kind, guint32 index, gboolean mute);
void pa_backend_device_set_port(App *app, RowKind kind, guint32 index, const gchar *port);
void pa_backend_device_set_default(App *app, RowKind kind, const gchar *name);

// Cards //
void pa_backend_card_set_profile(App *app, guint32 index, const gchar *profile);

// Meters //
void pa_backend_meter_attach(App *app, StreamRow *row, RowKind kind,
	const gchar *monitor_source, guint32 sink_input_index);
void pa_backend_meter_detach(StreamRow *row);

void pa_backend_refresh_cards(App *app);
#endif

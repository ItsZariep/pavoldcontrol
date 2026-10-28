#ifndef A_BACKEND_H
#define A_BACKEND_H

#include "app.h"

// backend identification
const gchar *a_backend_name(void);

gboolean a_backend_start(App *app);
void a_backend_stop(App *app);

// Streams (playback / recording) //
void a_backend_stream_set_volume(App *app, RowKind kind, guint32 index, const AVolume *vol);
void a_backend_stream_set_mute(App *app, RowKind kind, guint32 index, gboolean mute);
void a_backend_stream_move(App *app, RowKind kind, guint32 index, const gchar *device_name);

// Devices (sinks / sources) //
void a_backend_device_set_volume(App *app, RowKind kind, guint32 index, const AVolume *vol);
void a_backend_device_set_mute(App *app, RowKind kind, guint32 index, gboolean mute);
void a_backend_device_set_port(App *app, RowKind kind, guint32 index, const gchar *port);
void a_backend_device_set_default(App *app, RowKind kind, const gchar *name);

// Cards //
void a_backend_card_set_profile(App *app, guint32 index, const gchar *profile);
void a_backend_refresh_cards(App *app);

// Meters //
void a_backend_meter_attach(App *app, StreamRow *row);
void a_backend_meter_detach(StreamRow *row);

#endif

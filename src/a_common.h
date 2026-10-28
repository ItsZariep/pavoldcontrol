#ifndef A_COMMON_H
#define A_COMMON_H

// Helpers shared by all backends (row bookkeeping, filters, meters).

#include "app.h"

StreamRow *a_common_ensure_row(App *app, RowKind kind, guint32 index);
void a_common_remove_row(App *app, RowKind kind, guint32 index);

// TRUE if the row should currently be shown according to the settings
gboolean a_common_row_passes_filter(App *app, StreamRow *row);

// peak is 0..1 and is raw signal level (devices get scaled by their volume)
void a_common_meter_report(StreamRow *row, float peak);

// Sorted (by label) choices from a name -> description table
GList *a_common_choices_from_names(GHashTable *names);

#endif

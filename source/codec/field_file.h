#ifndef MP3PATCH_FIELD_FILE_H
#define MP3PATCH_FIELD_FILE_H
#include "layer3.h"
typedef struct FieldFile FieldFile;
FieldFile *field_file_open(const char *path, const char **error);
/* Divided frames carry fresh spectral fields. Packed bytes are available only on native frames. */
FieldFile *field_file_open_at(const char *path, int factor, const char **error);
int field_file_source_rate(const FieldFile *f);
int field_file_rewind(FieldFile *f, const char **error);
void field_file_close(FieldFile *f);
int field_file_rate(const FieldFile *f);
int field_file_channels(const FieldFile *f);
/* The consumer owns the returned slot until it calls advance. When no slot
   is available, status is 0 (refill), 1 (end), or -1 (error). */
const EncodedFrame *field_file_peek(FieldFile *f, int *status);
void field_file_advance(FieldFile *f);
const char *field_file_error(const FieldFile *f);
#endif

#ifndef MPEG_SUBBAND_READER_H
#define MPEG_SUBBAND_READER_H
#include "subband.h"
typedef struct Reader Reader;
Reader *reader_open(const char *path, int layer, int divisor, const char **error);
int reader_rewind(Reader *r, const char **error);
void reader_close(Reader *r);
Format reader_format(Reader *r);
const BandFrame *reader_peek(Reader *r, int *terminal);
void reader_advance(Reader *r);
const char *reader_error(Reader *r);
#endif

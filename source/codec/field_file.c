#include "field_file.h"
#include "spectral_rate.h"
#include "codec_clock.h"
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

enum { SLOTS = 16, PRIME = 4 };
struct FieldFile {
    FILE *file;
    off_t begin, end;
    int hz, target_hz, channels, thread_live;
    unsigned long source_granules, output_frames;
    int input_bitrates[SLOTS];
    SpectralRate *conversion;
    EncodedFrame *input;
    Synthesis *reader;
    EncodedFrame ring[SLOTS];
    _Atomic unsigned rd, wr;
    _Atomic int stop, terminal;
    const char *error;
    dispatch_semaphore_t wake;
    pthread_t thread;
};
static int at(FILE *fp, off_t where, void *buf, size_t n) {
    return !fseeko(fp, where, SEEK_SET) && fread(buf, 1, n, fp) == n;
}
static unsigned le32(const unsigned char *b) {
    return b[0] | ((unsigned)b[1] << 8) | ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
}
static int bounds(FieldFile *f, const char **error) {
    unsigned char b[32];
    FILE *fp = f->file;
    if (fseeko(fp, 0, SEEK_END) || (f->end = ftello(fp)) < 0)
        return 0;
    while (f->end - f->begin >= 10 && at(fp, f->begin, b, 10) && !memcmp(b, "ID3", 3)) {
        if (b[3] < 2 || b[3] > 4 || ((b[6] | b[7] | b[8] | b[9]) & 128)) {
            *error = "invalid ID3 header";
            return 0;
        }
        unsigned size = ((unsigned)b[6] << 21) | ((unsigned)b[7] << 14) | (b[8] << 7) | b[9];
        f->begin += 10 + size + (b[3] == 4 && (b[5] & 16) ? 10 : 0);
        if (f->begin > f->end) {
            *error = "truncated ID3 tag";
            return 0;
        }
    }
    if (f->end - f->begin >= 128 && at(fp, f->end - 128, b, 3) && !memcmp(b, "TAG", 3))
        f->end -= 128;
    if (f->end - f->begin >= 32 && at(fp, f->end - 32, b, 32) && !memcmp(b, "APETAGEX", 8)) {
        unsigned size = le32(b + 12);
        if (size < 32 || size > (uint64_t)(f->end - f->begin)) {
            *error = "invalid APE tag";
            return 0;
        }
        f->end -= size;
        if (f->end - f->begin >= 32 && at(fp, f->end - 32, b, 8) && !memcmp(b, "APETAGEX", 8))
            f->end -= 32;
    }
    /* Permit leading padding, but require indexed Layer III at the first frame. */
    if (fseeko(fp, f->begin, SEEK_SET))
        return 0;
    while (f->begin < f->end && fgetc(fp) == 0)
        ++f->begin;
    if (f->end - f->begin < 4 || !at(fp, f->begin, b, 4) || !(f->hz = layer3_header_rate(b))) {
        *error = "expected a bitrate-indexed Layer III MP3";
        return 0;
    }
    f->channels = (b[3] >> 6) == 3 ? 1 : 2;
    return 1;
}
static int info_frame(const unsigned char *h, int n) {
    int stereo = (h[3] >> 6) != 3, side = (h[1] & 8) ? (stereo ? 32 : 17) : (stereo ? 17 : 9);
    int offsets[2] = {4 + side, 4 + side + ((h[1] & 1) ? 0 : 2)};
    for (int i = 0; i < 2; i++) {
        int k = offsets[i];
        if (k + 8 <= n && (!memcmp(h + k, "Xing", 4) || !memcmp(h + k, "Info", 4)))
            return 1;
    }
    return n >= 54 && !memcmp(h + 36, "VBRI", 4);
}
/* Sole producer. Inspect metadata and consume the reservoir sequentially. */
static void refill(FieldFile *f, int target) {
    unsigned char bytes[L3_FRAME_MAX];
    while (!atomic_load(&f->stop) && !atomic_load(&f->terminal)) {
        unsigned wr = atomic_load(&f->wr);
        if (wr - atomic_load(&f->rd) >= (unsigned)target)
            return;
        off_t pos = ftello(f->file);
        EncodedFrame *slot = &f->ring[wr % SLOTS];
        if (pos == f->end) {
            if (f->conversion && spectral_rate_finish(f->conversion, slot->fields[0])) {
                slot->length = 0;
                slot->kbps = f->input_bitrates[f->output_frames++ % SLOTS];
                slot->rate = f->target_hz;
                slot->channels = f->channels;
                slot->granules = 1;
                atomic_store(&f->wr, wr + 1);
                continue;
            }
            atomic_store(&f->terminal, 1);
            return;
        }
        const char *problem = "truncated or invalid MP3 frame";
        if (pos < 0 || f->end - pos < 4 || fread(bytes, 1, 4, f->file) != 4)
            goto error;
        int n = layer3_frame_length(bytes);
        if (!n || n > f->end - pos || fread(bytes + 4, 1, n - 4, f->file) != (size_t)(n - 4))
            goto error;
        if (layer3_header_rate(bytes) != f->hz || ((bytes[3] >> 6) == 3 ? 1 : 2) != f->channels) {
            problem = "source rate or channel count changes within the file";
            goto error;
        }
        EncodedFrame *input = f->conversion ? f->input : slot;
        int decoded = layer3_read_fields(f->reader, bytes, n, input);
        if (pos == f->begin && info_frame(bytes, n))
            continue;
        if (!decoded) {
            problem = "invalid Layer III data or missing reservoir history";
            goto error;
        }
        if (f->conversion) {
            /* A divided output has one granule. Each incoming frame can
               complete at most one, including two-granule MPEG-1 frames. */
            int ready = 0;
            int factor = f->hz / f->target_hz;
            for (int gr = 0; gr < input->granules; ++gr) {
                if (f->source_granules % factor == 0)
                    f->input_bitrates[(f->source_granules / factor) % SLOTS] = input->kbps;
                ++f->source_granules;
                ready |= spectral_rate_fields(f->conversion, input->fields[gr], slot->fields[0]);
            }
            if (!ready)
                continue;
            slot->length = 0;
            slot->kbps = f->input_bitrates[f->output_frames++ % SLOTS];
            slot->rate = f->target_hz;
            slot->channels = f->channels;
            slot->granules = 1;
        }
        atomic_store(&f->wr, wr + 1);
        continue;
    error:
        f->error = problem;
        atomic_store(&f->terminal, -1);
    }
}
static void *produce(void *arg) {
    FieldFile *f = arg;
    while (!atomic_load(&f->stop)) {
        refill(f, SLOTS);
        if (atomic_load(&f->terminal))
            break;
        dispatch_semaphore_wait(f->wake, DISPATCH_TIME_FOREVER);
    }
    return NULL;
}
static void join(FieldFile *f) {
    if (!f->thread_live)
        return;
    atomic_store(&f->stop, 1);
    dispatch_semaphore_signal(f->wake);
    pthread_join(f->thread, NULL);
    f->thread_live = 0;
}
int field_file_rewind(FieldFile *f, const char **error) {
    join(f);
    atomic_store(&f->rd, 0);
    atomic_store(&f->wr, 0);
    atomic_store(&f->stop, 0);
    atomic_store(&f->terminal, 0);
    f->error = NULL;
    f->source_granules = f->output_frames = 0;
    if (fseeko(f->file, f->begin, SEEK_SET)) {
        *error = "cannot rewind file";
        return 0;
    }
    layer3_clear(f->reader);
    spectral_rate_reset(f->conversion);
    refill(f, PRIME);
    if (atomic_load(&f->terminal) < 0 || !atomic_load(&f->wr)) {
        *error = f->error ? f->error : "no audio frames in file";
        return 0;
    }
    if (!atomic_load(&f->terminal)) {
        if (pthread_create(&f->thread, NULL, produce, f)) {
            *error = "cannot start file reader";
            return 0;
        }
        f->thread_live = 1;
    }
    return 1;
}
FieldFile *field_file_open(const char *path, const char **error) {
    return field_file_open_at(path, 1, error);
}
FieldFile *field_file_open_at(const char *path, int factor, const char **error) {
    FieldFile *f = calloc(1, sizeof(*f));
    *error = "cannot allocate file reader";
    if (!f)
        return NULL;
    f->reader = layer3_new();
    f->wake = dispatch_semaphore_create(0);
    if (!f->reader || !f->wake)
        goto fail;
    *error = "cannot open MP3";
    if (!(f->file = fopen(path, "rb")) || !bounds(f, error))
        goto fail;
    f->target_hz = codec_divide(f->hz, factor);
    if (f->target_hz != f->hz) {
        f->conversion = spectral_rate_new(f->hz, f->target_hz, f->channels);
        f->input = calloc(1, sizeof(*f->input));
        if (!f->conversion || !f->input) {
            *error = "cannot prepare spectral rate conversion";
            goto fail;
        }
    }
    if (!field_file_rewind(f, error))
        goto fail;
    return f;
fail:
    field_file_close(f);
    return NULL;
}
void field_file_close(FieldFile *f) {
    if (!f)
        return;
    join(f);
    if (f->file)
        fclose(f->file);
    if (f->wake)
        dispatch_release(f->wake);
    spectral_rate_free(f->conversion);
    free(f->input);
    layer3_free(f->reader);
    free(f);
}
int field_file_rate(const FieldFile *f) {
    return f->target_hz;
}
int field_file_source_rate(const FieldFile *f) {
    return f->hz;
}
int field_file_channels(const FieldFile *f) { return f->channels; }
const char *field_file_error(const FieldFile *f) { return f->error; }
const EncodedFrame *field_file_peek(FieldFile *f, int *status) {
    unsigned rd = atomic_load(&f->rd);
    /* Acquire terminal before wr. A final published frame always precedes EOF. */
    *status = atomic_load(&f->terminal);
    if (rd == atomic_load(&f->wr))
        return NULL;
    *status = 0;
    return &f->ring[rd % SLOTS];
}
void field_file_advance(FieldFile *f) {
    atomic_fetch_add(&f->rd, 1);
    dispatch_semaphore_signal(f->wake);
}

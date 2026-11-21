#include "reader.h"
#include "convert.h"
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
enum { SLOTS = 24, PRIME = 3 };
struct Reader {
    FILE *file;
    off_t begin, end;
    int layer, live, dest;
    Format original;
    BandFrame frames[SLOTS];
    Packet input;
    Converter converter;
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
static int bounds(Reader *f, const char **error) {
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
    /* Skip leading padding before the first indexed frame. */
    if (fseeko(fp, f->begin, SEEK_SET))
        return 0;
    while (f->begin < f->end && fgetc(fp) == 0)
        ++f->begin;
    if (f->end - f->begin < 4 || !at(fp, f->begin, b, 4) || !sb_header(b, &f->original) ||
        f->original.layer != f->layer) {
        *error = "expected an indexed MPEG Layer I or II file matching this object";
        return 0;
    }

    return 1;
}
static void refill(Reader *r, int count) {
    uint8_t bytes[SB_PACKET];
    while (!atomic_load(&r->stop) && !atomic_load(&r->terminal)) {
        unsigned wr = atomic_load(&r->wr);
        if (wr - atomic_load(&r->rd) >= (unsigned)count)
            return;
        BandFrame *slot = &r->frames[wr % SLOTS];
        off_t pos = ftello(r->file);
        if (pos == r->end) {
            if (convert_finish(&r->converter, slot)) {
                atomic_store(&r->wr, wr + 1);
                continue;
            }
            atomic_store(&r->terminal, 1);
            return;
        }
        const char *problem = "truncated or invalid MPEG audio frame";
        if (pos < 0 || r->end - pos < 4 || fread(bytes, 1, 4, r->file) != 4)
            goto fail;
        Format head;
        int len = sb_header(bytes, &head);
        if (len <= 4 || len > SB_PACKET || len > r->end - pos ||
            fread(bytes + 4, 1, len - 4, r->file) != (size_t)len - 4)
            goto fail;
        if (head.layer != r->layer || head.hz != r->original.hz ||
            head.channels != r->original.channels) {
            problem = "sample rate, layer or channel count changes within file";
            goto fail;
        }
        if (!sb_unpack(bytes, len, &r->input))
            goto fail;
        if (convert_frame(&r->converter, &r->input.fields, slot))
            atomic_store(&r->wr, wr + 1);
        continue;
    fail:
        r->error = problem;
        atomic_store(&r->terminal, -1);
    }
}
static void *produce(void *arg) {
    Reader *r = arg;
    while (!atomic_load(&r->stop)) {
        refill(r, SLOTS);
        if (atomic_load(&r->terminal))
            break;
        dispatch_semaphore_wait(r->wake, DISPATCH_TIME_FOREVER);
    }
    return NULL;
}
static void join(Reader *r) {
    if (r->live) {
        atomic_store(&r->stop, 1);
        dispatch_semaphore_signal(r->wake);
        pthread_join(r->thread, NULL);
        r->live = 0;
    }
}
int reader_rewind(Reader *r, const char **error) {
    join(r);
    atomic_store(&r->rd, 0);
    atomic_store(&r->wr, 0);
    atomic_store(&r->terminal, 0);
    atomic_store(&r->stop, 0);
    r->error = NULL;
    if (fseeko(r->file, r->begin, SEEK_SET)) {
        *error = "cannot rewind file";
        return 0;
    }
    convert_start(&r->converter, r->original, r->dest);
    refill(r, PRIME);
    if (atomic_load(&r->terminal) < 0 || !atomic_load(&r->wr)) {
        *error = r->error ? r->error : "no audio frames";
        return 0;
    }
    if (!atomic_load(&r->terminal)) {
        if (pthread_create(&r->thread, NULL, produce, r)) {
            *error = "cannot start reader";
            return 0;
        }
        r->live = 1;
    }
    return 1;
}
Reader *reader_open(const char *path, int layer, int divisor, const char **error) {
    Reader *r = calloc(1, sizeof(*r));
    *error = "cannot allocate file reader";
    if (!r)
        return NULL;
    r->layer = layer;
    r->wake = dispatch_semaphore_create(0);
    if (!r->wake)
        goto fail;
    *error = "cannot open file";
    r->file = fopen(path, "rb");
    if (!r->file || !bounds(r, error))
        goto fail;
    r->dest = convert_target(r->original, divisor);
    if (!reader_rewind(r, error))
        goto fail;
    return r;
fail:
    reader_close(r);
    return NULL;
}
void reader_close(Reader *r) {
    if (r) {
        join(r);
        if (r->file)
            fclose(r->file);
        if (r->wake)
            dispatch_release(r->wake);
        free(r);
    }
}
Format reader_format(Reader *r) {
    Format f = r->original;
    f.hz = r->dest;
    return f;
}
const BandFrame *reader_peek(Reader *r, int *terminal) {
    unsigned rd = atomic_load(&r->rd);
    *terminal = atomic_load(&r->terminal);
    if (rd == atomic_load(&r->wr))
        return NULL;
    *terminal = 0;
    return &r->frames[rd % SLOTS];
}
void reader_advance(Reader *r) {
    atomic_fetch_add(&r->rd, 1);
    dispatch_semaphore_signal(r->wake);
}
const char *reader_error(Reader *r) {
    return r->error;
}

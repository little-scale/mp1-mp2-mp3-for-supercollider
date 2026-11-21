#ifndef MP1_LAYER1_H
#define MP1_LAYER1_H

#include <stdint.h>

typedef struct {
    double history[16][64], pcm[32], input[32], step[32];
    int allocation[32];
    uint32_t written;
    int column, slot, head;
} Layer1;

void layer1_init_tables(void);
void layer1_reset(Layer1 *d);
/* Call once per host sample. Read column/slot before calling for sync outputs. */
double layer1_tick(Layer1 *d, double data, double address, double allocation,
                   double scale, int raw, int data_connected);

#endif

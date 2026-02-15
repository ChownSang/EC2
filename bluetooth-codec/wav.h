#ifndef WAV_H
#define WAV_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t num_samples;    /* total samples per channel */
    int16_t *data;           /* interleaved PCM data */
} wav_file_t;

/* Read a 16-bit PCM WAV file. Allocates wav->data internally. */
int wav_read(const char *path, wav_file_t *wav);

/* Write a 16-bit PCM WAV file. */
int wav_write(const char *path, const wav_file_t *wav);

/* Free allocated data */
void wav_free(wav_file_t *wav);

/* Generate a test sine wave WAV (for testing without input file) */
int wav_generate_test(wav_file_t *wav, uint32_t sample_rate, uint16_t channels,
                      double freq_hz, double duration_sec);

#endif /* WAV_H */

#include "wav.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Minimal WAV reader/writer for 16-bit PCM */

int wav_read(const char *path, wav_file_t *wav) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char riff[4];
    uint32_t file_size, fmt_size, data_size;
    uint16_t audio_fmt, channels, block_align, bps;
    uint32_t sample_rate, byte_rate;

    /* RIFF header */
    if (fread(riff, 1, 4, f) != 4 || memcmp(riff, "RIFF", 4) != 0) goto fail;
    if (fread(&file_size, 4, 1, f) != 1) goto fail;
    if (fread(riff, 1, 4, f) != 4 || memcmp(riff, "WAVE", 4) != 0) goto fail;

    /* Find fmt chunk */
    while (1) {
        char chunk_id[4];
        uint32_t chunk_size;
        if (fread(chunk_id, 1, 4, f) != 4) goto fail;
        if (fread(&chunk_size, 4, 1, f) != 1) goto fail;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            fmt_size = chunk_size;
            if (fread(&audio_fmt, 2, 1, f) != 1) goto fail;
            if (fread(&channels, 2, 1, f) != 1) goto fail;
            if (fread(&sample_rate, 4, 1, f) != 1) goto fail;
            if (fread(&byte_rate, 4, 1, f) != 1) goto fail;
            if (fread(&block_align, 2, 1, f) != 1) goto fail;
            if (fread(&bps, 2, 1, f) != 1) goto fail;
            /* Skip extra fmt bytes */
            if (fmt_size > 16) fseek(f, fmt_size - 16, SEEK_CUR);

            if (audio_fmt != 1) { /* PCM = 1 */
                fprintf(stderr, "Error: only PCM WAV supported (got format %d)\n", audio_fmt);
                goto fail;
            }
            if (bps != 16) {
                fprintf(stderr, "Error: only 16-bit WAV supported (got %d-bit)\n", bps);
                goto fail;
            }
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            break;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    uint32_t total_interleaved = data_size / 2;
    uint32_t samples_per_ch = total_interleaved / channels;

    wav->channels = channels;
    wav->sample_rate = sample_rate;
    wav->bits_per_sample = bps;
    wav->num_samples = samples_per_ch;
    wav->data = (int16_t *)malloc(data_size);
    if (!wav->data) goto fail;

    if (fread(wav->data, 1, data_size, f) != data_size) {
        free(wav->data);
        wav->data = NULL;
        goto fail;
    }

    fclose(f);
    return 0;

fail:
    fclose(f);
    return -1;
}

int wav_write(const char *path, const wav_file_t *wav) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    uint32_t data_size = wav->num_samples * wav->channels * 2;
    uint32_t file_size = 36 + data_size;
    uint16_t audio_fmt = 1;
    uint16_t block_align = wav->channels * 2;
    uint32_t byte_rate = wav->sample_rate * block_align;
    uint16_t bps = 16;
    uint32_t fmt_size = 16;

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&audio_fmt, 2, 1, f);
    fwrite(&wav->channels, 2, 1, f);
    fwrite(&wav->sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bps, 2, 1, f);

    /* data chunk */
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
    fwrite(wav->data, 1, data_size, f);

    fclose(f);
    return 0;
}

void wav_free(wav_file_t *wav) {
    if (wav->data) {
        free(wav->data);
        wav->data = NULL;
    }
}

int wav_generate_test(wav_file_t *wav, uint32_t sample_rate, uint16_t channels,
                      double freq_hz, double duration_sec) {
    uint32_t num_samples = (uint32_t)(sample_rate * duration_sec);
    wav->channels = channels;
    wav->sample_rate = sample_rate;
    wav->bits_per_sample = 16;
    wav->num_samples = num_samples;
    wav->data = (int16_t *)malloc(num_samples * channels * sizeof(int16_t));
    if (!wav->data) return -1;

    for (uint32_t i = 0; i < num_samples; i++) {
        double t = (double)i / sample_rate;
        /* Mix of frequencies for a richer test signal */
        double val = 0.5 * sin(2.0 * M_PI * freq_hz * t)
                   + 0.3 * sin(2.0 * M_PI * freq_hz * 2.0 * t)
                   + 0.1 * sin(2.0 * M_PI * freq_hz * 3.0 * t)
                   + 0.05 * sin(2.0 * M_PI * freq_hz * 5.0 * t);
        int16_t sample = (int16_t)(val * 28000.0);
        for (uint16_t c = 0; c < channels; c++) {
            wav->data[i * channels + c] = sample;
        }
    }

    return 0;
}

#ifndef NANOCODEC_H
#define NANOCODEC_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/*
 * NanoCodec (NC1) - Custom Bluetooth Audio Codec
 *
 * Design:
 *   - 8 subbands via cosine-modulated polyphase filter bank
 *   - Adaptive DPCM encoding per subband
 *   - Energy-based bit allocation (2-6 bits/sample)
 *   - Frame-based processing (128 samples/frame mono)
 *
 * Target: ~128-320 kbps at 44100 Hz, ~3ms latency
 */

#define NC_SUBBANDS       8
#define NC_BLOCKS_PER_FRAME 16
#define NC_FRAME_SAMPLES  (NC_SUBBANDS * NC_BLOCKS_PER_FRAME)  /* 128 */
#define NC_PROTO_LEN      (NC_SUBBANDS * 10)                   /* 80-tap prototype filter */
#define NC_MAX_CHANNELS   2

#define NC_MAGIC          0x4E433031  /* "NC01" */
#define NC_VERSION        1

/* Quality presets */
typedef enum {
    NC_QUALITY_LOW  = 0,   /* ~128 kbps mono, avg 3 bits/sample */
    NC_QUALITY_MED  = 1,   /* ~200 kbps mono, avg 4 bits/sample */
    NC_QUALITY_HIGH = 2,   /* ~320 kbps mono, avg 5 bits/sample */
} nc_quality_t;

/* File header (written at start of .nc1 file) */
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  channels;
    uint8_t  quality;
    uint8_t  reserved;
    uint32_t sample_rate;
    uint32_t total_frames;
    uint32_t total_samples;  /* original PCM sample count per channel */
} nc_file_header_t;

/* Frame header in bitstream */
typedef struct {
    uint16_t scale_factors[NC_SUBBANDS];  /* 8.8 fixed-point log2 scale per subband */
    uint8_t  bit_alloc[NC_SUBBANDS];      /* bits per sample per subband (2-6) */
} nc_frame_header_t;

/* Encoder state (per channel) */
typedef struct {
    /* Analysis filter bank state */
    double analysis_buf[NC_PROTO_LEN];
    int    analysis_pos;

    /* DPCM predictor state per subband */
    double predictor[NC_SUBBANDS];
    double step_size[NC_SUBBANDS];
} nc_channel_state_t;

/* Encoder context */
typedef struct {
    uint32_t          sample_rate;
    uint8_t           channels;
    nc_quality_t      quality;
    uint32_t          frames_encoded;
    uint32_t          total_samples;
    nc_channel_state_t ch_state[NC_MAX_CHANNELS];
    double            proto_filter[NC_PROTO_LEN];  /* prototype lowpass filter */
} nc_encoder_t;

/* Decoder context */
typedef struct {
    uint32_t          sample_rate;
    uint8_t           channels;
    nc_quality_t      quality;
    uint32_t          frames_decoded;
    nc_channel_state_t ch_state[NC_MAX_CHANNELS];
    double            proto_filter[NC_PROTO_LEN];
} nc_decoder_t;

/* Bitstream writer/reader */
typedef struct {
    uint8_t *data;
    size_t   capacity;
    size_t   byte_pos;
    int      bit_pos;   /* 0-7, bits remaining in current byte */
} nc_bitstream_t;

/* ---- Public API ---- */

/* Encoder */
int  nc_encoder_init(nc_encoder_t *enc, uint32_t sample_rate, uint8_t channels, nc_quality_t quality);
int  nc_encode_frame(nc_encoder_t *enc, const int16_t *pcm, size_t num_samples, nc_bitstream_t *bs);
void nc_encoder_reset(nc_encoder_t *enc);

/* Decoder */
int  nc_decoder_init(nc_decoder_t *dec, uint32_t sample_rate, uint8_t channels, nc_quality_t quality);
int  nc_decode_frame(nc_decoder_t *dec, nc_bitstream_t *bs, int16_t *pcm, size_t max_samples);
void nc_decoder_reset(nc_decoder_t *dec);

/* Bitstream helpers */
void nc_bs_init_write(nc_bitstream_t *bs, uint8_t *buf, size_t capacity);
void nc_bs_init_read(nc_bitstream_t *bs, const uint8_t *buf, size_t size);
void nc_bs_write_bits(nc_bitstream_t *bs, uint32_t val, int nbits);
uint32_t nc_bs_read_bits(nc_bitstream_t *bs, int nbits);
size_t nc_bs_bytes_written(const nc_bitstream_t *bs);

/* File I/O helpers */
int  nc_write_file_header(FILE *f, const nc_encoder_t *enc);
int  nc_read_file_header(FILE *f, nc_file_header_t *hdr);

/* Utility */
size_t nc_max_frame_bytes(nc_quality_t quality, uint8_t channels);
int    nc_base_bits_per_sample(nc_quality_t quality);

#endif /* NANOCODEC_H */

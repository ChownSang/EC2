#include "nanocodec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ================================================================
 * NanoCodec (NC1) - Custom Bluetooth Audio Codec
 *
 * Signal flow:
 *   PCM -> Analysis Filter Bank (8 subbands)
 *       -> Bit Allocation (energy-based)
 *       -> Adaptive DPCM Quantization
 *       -> Bitstream Packing
 *
 * Decoder reverses the process:
 *   Bitstream -> Unpack -> Dequantize -> Synthesis Filter Bank -> PCM
 * ================================================================ */

/* ---- DCT-IV Based Subband Filter Bank ----
 * DCT-IV is its own inverse (orthogonal and symmetric), guaranteeing
 * perfect reconstruction before quantization.
 *
 * Forward:  X[k] = sqrt(2/N) * sum_{n=0}^{N-1} x[n] * cos(pi/N * (n+0.5)(k+0.5))
 * Inverse:  x[n] = sqrt(2/N) * sum_{k=0}^{N-1} X[k] * cos(pi/N * (n+0.5)(k+0.5))
 */

static void dct4_forward(const double *input, double *output, int N) {
    double scale = sqrt(2.0 / N);
    for (int k = 0; k < N; k++) {
        double sum = 0.0;
        for (int n = 0; n < N; n++) {
            sum += input[n] * cos(M_PI / N * (n + 0.5) * (k + 0.5));
        }
        output[k] = sum * scale;
    }
}

static void dct4_inverse(const double *input, double *output, int N) {
    /* DCT-IV is its own inverse (with same scaling) */
    dct4_forward(input, output, N);
}

/* No prototype filter needed for DCT-IV approach */
static void design_prototype_filter(double *h, int len, int num_bands) {
    (void)num_bands;
    for (int i = 0; i < len; i++) h[i] = 1.0;
}

/* Analysis: time domain -> subband domain */
static void analysis_filter_bank(nc_channel_state_t *st, const double *proto,
                                  const double *input, double subband[NC_SUBBANDS]) {
    (void)st; (void)proto;
    dct4_forward(input, subband, NC_SUBBANDS);
}

/* Synthesis: subband domain -> time domain */
static void synthesis_filter_bank(nc_channel_state_t *st, const double *proto,
                                   const double subband[NC_SUBBANDS], double *output) {
    (void)st; (void)proto;
    dct4_inverse(subband, output, NC_SUBBANDS);
}

/* ---- Bit allocation ----
 * Distribute bits across subbands based on energy.
 * Higher energy subbands get more bits (2-6 range).
 */
static int base_bits_table[] = { 3, 4, 5 };  /* LOW, MED, HIGH */

int nc_base_bits_per_sample(nc_quality_t quality) {
    if (quality > NC_QUALITY_HIGH) quality = NC_QUALITY_MED;
    return base_bits_table[quality];
}

static void compute_bit_allocation(const double subband_energy[NC_SUBBANDS],
                                    nc_quality_t quality,
                                    uint8_t bit_alloc[NC_SUBBANDS]) {
    int base = nc_base_bits_per_sample(quality);
    int total_budget = base * NC_SUBBANDS;

    /* Compute log-energy for each subband */
    double log_energy[NC_SUBBANDS];
    double avg_log = 0.0;
    for (int k = 0; k < NC_SUBBANDS; k++) {
        log_energy[k] = log2(subband_energy[k] + 1e-10);
        avg_log += log_energy[k];
    }
    avg_log /= NC_SUBBANDS;

    /* Allocate bits proportional to deviation from average */
    int allocated[NC_SUBBANDS];
    int sum = 0;
    for (int k = 0; k < NC_SUBBANDS; k++) {
        allocated[k] = base + (int)round((log_energy[k] - avg_log) * 0.8);
        if (allocated[k] < 2) allocated[k] = 2;
        if (allocated[k] > 6) allocated[k] = 6;
        sum += allocated[k];
    }

    /* Adjust to meet budget */
    while (sum > total_budget) {
        int max_k = 0;
        for (int k = 1; k < NC_SUBBANDS; k++)
            if (allocated[k] > allocated[max_k]) max_k = k;
        if (allocated[max_k] <= 2) break;
        allocated[max_k]--;
        sum--;
    }
    while (sum < total_budget) {
        int min_k = 0;
        for (int k = 1; k < NC_SUBBANDS; k++)
            if (allocated[k] < allocated[min_k]) min_k = k;
        if (allocated[min_k] >= 6) break;
        allocated[min_k]++;
        sum++;
    }

    for (int k = 0; k < NC_SUBBANDS; k++)
        bit_alloc[k] = (uint8_t)allocated[k];
}

/* ---- Scale factors ----
 * Store peak as 16-bit fixed-point: 8.8 format in log2 domain.
 * This gives 1/256 precision in log2 (~0.024 dB steps), eliminating
 * scale factor quantization as a bottleneck.
 */
static void compute_scale_factors(const double sb_data[NC_BLOCKS_PER_FRAME][NC_SUBBANDS],
                                   uint16_t scale_factors[NC_SUBBANDS]) {
    for (int k = 0; k < NC_SUBBANDS; k++) {
        double peak = 0.0;
        for (int b = 0; b < NC_BLOCKS_PER_FRAME; b++) {
            double a = fabs(sb_data[b][k]);
            if (a > peak) peak = a;
        }
        /* Round up slightly to ensure peak is covered */
        peak *= 1.01;
        /* 8.8 fixed-point log2, offset by 128 */
        int sf = (int)round((log2(peak + 1e-10) + 20.0) * 256.0);
        if (sf < 0) sf = 0;
        if (sf > 65535) sf = 65535;
        scale_factors[k] = (uint16_t)sf;
    }
}

static double scale_factor_to_value(uint16_t sf) {
    return pow(2.0, (double)sf / 256.0 - 20.0);
}

/* ---- Uniform mid-tread quantization ----
 * Direct quantization of DCT coefficients (no DPCM needed for transform domain).
 * Scale factor normalizes the range, then uniform quantize to nbits.
 */
static int quantize_sample(double val, double scale, int nbits, double *predictor) {
    (void)predictor;
    int levels = (1 << nbits) - 1;
    int half = levels / 2;

    /* Normalize to [-1, 1] range using scale factor */
    double normalized = (scale > 1e-10) ? (val / scale) : 0.0;

    /* Quantize to [-half, half] */
    int q = (int)round(normalized * half);
    if (q < -half) q = -half;
    if (q > half) q = half;

    /* Convert to unsigned for bitstream */
    return q + half;
}

static double dequantize_sample(int code, double scale, int nbits, double *predictor) {
    (void)predictor;
    int levels = (1 << nbits) - 1;
    int half = levels / 2;

    int q = code - half;
    double val = (double)q / half * scale;
    return val;
}

/* ---- Bitstream operations ---- */

void nc_bs_init_write(nc_bitstream_t *bs, uint8_t *buf, size_t capacity) {
    bs->data = buf;
    bs->capacity = capacity;
    bs->byte_pos = 0;
    bs->bit_pos = 0;
    memset(buf, 0, capacity);
}

void nc_bs_init_read(nc_bitstream_t *bs, const uint8_t *buf, size_t size) {
    bs->data = (uint8_t *)buf;
    bs->capacity = size;
    bs->byte_pos = 0;
    bs->bit_pos = 0;
}

void nc_bs_write_bits(nc_bitstream_t *bs, uint32_t val, int nbits) {
    for (int i = nbits - 1; i >= 0; i--) {
        if (bs->byte_pos >= bs->capacity) return;
        if (val & (1u << i))
            bs->data[bs->byte_pos] |= (0x80 >> bs->bit_pos);
        bs->bit_pos++;
        if (bs->bit_pos == 8) {
            bs->bit_pos = 0;
            bs->byte_pos++;
        }
    }
}

uint32_t nc_bs_read_bits(nc_bitstream_t *bs, int nbits) {
    uint32_t val = 0;
    for (int i = 0; i < nbits; i++) {
        if (bs->byte_pos >= bs->capacity) return val;
        val <<= 1;
        if (bs->data[bs->byte_pos] & (0x80 >> bs->bit_pos))
            val |= 1;
        bs->bit_pos++;
        if (bs->bit_pos == 8) {
            bs->bit_pos = 0;
            bs->byte_pos++;
        }
    }
    return val;
}

size_t nc_bs_bytes_written(const nc_bitstream_t *bs) {
    return bs->byte_pos + (bs->bit_pos > 0 ? 1 : 0);
}

/* ---- Encoder ---- */

int nc_encoder_init(nc_encoder_t *enc, uint32_t sample_rate, uint8_t channels, nc_quality_t quality) {
    memset(enc, 0, sizeof(*enc));
    enc->sample_rate = sample_rate;
    enc->channels = channels;
    enc->quality = quality;
    enc->frames_encoded = 0;
    enc->total_samples = 0;

    /* Initialize predictor step sizes */
    for (int c = 0; c < channels; c++) {
        for (int k = 0; k < NC_SUBBANDS; k++) {
            enc->ch_state[c].predictor[k] = 0.0;
            enc->ch_state[c].step_size[k] = 1.0;
        }
        enc->ch_state[c].analysis_pos = 0;
        memset(enc->ch_state[c].analysis_buf, 0, sizeof(enc->ch_state[c].analysis_buf));
    }

    design_prototype_filter(enc->proto_filter, NC_PROTO_LEN, NC_SUBBANDS);
    return 0;
}

int nc_encode_frame(nc_encoder_t *enc, const int16_t *pcm, size_t num_samples,
                     nc_bitstream_t *bs) {
    uint8_t channels = enc->channels;
    double sb_data[NC_MAX_CHANNELS][NC_BLOCKS_PER_FRAME][NC_SUBBANDS];

    /* Step 1: Analysis filter bank */
    for (int ch = 0; ch < channels; ch++) {
        for (int blk = 0; blk < NC_BLOCKS_PER_FRAME; blk++) {
            double input[NC_SUBBANDS];
            for (int i = 0; i < NC_SUBBANDS; i++) {
                int idx = blk * NC_SUBBANDS + i;
                if (channels > 1) {
                    /* Interleaved stereo */
                    int pcm_idx = idx * channels + ch;
                    input[i] = (pcm_idx < (int)num_samples * channels)
                        ? (double)pcm[pcm_idx] : 0.0;
                } else {
                    input[i] = (idx < (int)num_samples)
                        ? (double)pcm[idx] : 0.0;
                }
            }
            analysis_filter_bank(&enc->ch_state[ch], enc->proto_filter,
                                  input, sb_data[ch][blk]);
        }
    }

    /* Step 2: Compute energy and bit allocation (per channel) */
    for (int ch = 0; ch < channels; ch++) {
        double energy[NC_SUBBANDS] = {0};
        uint16_t scale_factors[NC_SUBBANDS];
        uint8_t bit_alloc[NC_SUBBANDS];

        for (int k = 0; k < NC_SUBBANDS; k++) {
            for (int b = 0; b < NC_BLOCKS_PER_FRAME; b++) {
                energy[k] += sb_data[ch][b][k] * sb_data[ch][b][k];
            }
            energy[k] /= NC_BLOCKS_PER_FRAME;
        }

        compute_scale_factors(sb_data[ch], scale_factors);
        compute_bit_allocation(energy, enc->quality, bit_alloc);

        /* Step 3: Write frame header (16-bit scale factors + 3-bit allocs) */
        for (int k = 0; k < NC_SUBBANDS; k++)
            nc_bs_write_bits(bs, scale_factors[k], 16);
        for (int k = 0; k < NC_SUBBANDS; k++)
            nc_bs_write_bits(bs, bit_alloc[k], 3);

        /* Step 4: DPCM encode and write subband samples */
        for (int b = 0; b < NC_BLOCKS_PER_FRAME; b++) {
            for (int k = 0; k < NC_SUBBANDS; k++) {
                double scale = scale_factor_to_value(scale_factors[k]);
                int code = quantize_sample(sb_data[ch][b][k], scale,
                                            bit_alloc[k],
                                            &enc->ch_state[ch].predictor[k]);
                nc_bs_write_bits(bs, (uint32_t)code, bit_alloc[k]);
            }
        }
    }

    enc->frames_encoded++;
    enc->total_samples += NC_FRAME_SAMPLES;
    return 0;
}

void nc_encoder_reset(nc_encoder_t *enc) {
    for (int c = 0; c < enc->channels; c++) {
        memset(&enc->ch_state[c], 0, sizeof(nc_channel_state_t));
        for (int k = 0; k < NC_SUBBANDS; k++) {
            enc->ch_state[c].step_size[k] = 1.0;
        }
    }
    enc->frames_encoded = 0;
    enc->total_samples = 0;
}

/* ---- Decoder ---- */

int nc_decoder_init(nc_decoder_t *dec, uint32_t sample_rate, uint8_t channels, nc_quality_t quality) {
    memset(dec, 0, sizeof(*dec));
    dec->sample_rate = sample_rate;
    dec->channels = channels;
    dec->quality = quality;
    dec->frames_decoded = 0;

    for (int c = 0; c < channels; c++) {
        for (int k = 0; k < NC_SUBBANDS; k++) {
            dec->ch_state[c].predictor[k] = 0.0;
            dec->ch_state[c].step_size[k] = 1.0;
        }
        memset(dec->ch_state[c].analysis_buf, 0, sizeof(dec->ch_state[c].analysis_buf));
    }

    design_prototype_filter(dec->proto_filter, NC_PROTO_LEN, NC_SUBBANDS);
    return 0;
}

int nc_decode_frame(nc_decoder_t *dec, nc_bitstream_t *bs, int16_t *pcm, size_t max_samples) {
    uint8_t channels = dec->channels;

    for (int ch = 0; ch < channels; ch++) {
        uint16_t scale_factors[NC_SUBBANDS];
        uint8_t bit_alloc[NC_SUBBANDS];

        /* Read frame header (16-bit scale factors + 3-bit allocs) */
        for (int k = 0; k < NC_SUBBANDS; k++)
            scale_factors[k] = (uint16_t)nc_bs_read_bits(bs, 16);
        for (int k = 0; k < NC_SUBBANDS; k++)
            bit_alloc[k] = (uint8_t)nc_bs_read_bits(bs, 3);

        /* Decode subband samples */
        for (int b = 0; b < NC_BLOCKS_PER_FRAME; b++) {
            double subband[NC_SUBBANDS];
            for (int k = 0; k < NC_SUBBANDS; k++) {
                int code = (int)nc_bs_read_bits(bs, bit_alloc[k]);
                double scale = scale_factor_to_value(scale_factors[k]);
                subband[k] = dequantize_sample(code, scale, bit_alloc[k],
                                                &dec->ch_state[ch].predictor[k]);
            }

            /* Synthesis filter bank -> PCM */
            double output[NC_SUBBANDS];
            synthesis_filter_bank(&dec->ch_state[ch], dec->proto_filter,
                                   subband, output);

            for (int i = 0; i < NC_SUBBANDS; i++) {
                int idx = b * NC_SUBBANDS + i;
                if (channels > 1) {
                    int pcm_idx = idx * channels + ch;
                    if (pcm_idx < (int)max_samples * channels) {
                        double val = output[i];
                        if (val > 32767.0) val = 32767.0;
                        if (val < -32768.0) val = -32768.0;
                        pcm[pcm_idx] = (int16_t)val;
                    }
                } else {
                    if (idx < (int)max_samples) {
                        double val = output[i];
                        if (val > 32767.0) val = 32767.0;
                        if (val < -32768.0) val = -32768.0;
                        pcm[idx] = (int16_t)val;
                    }
                }
            }
        }
    }

    dec->frames_decoded++;
    return 0;
}

void nc_decoder_reset(nc_decoder_t *dec) {
    for (int c = 0; c < dec->channels; c++) {
        memset(&dec->ch_state[c], 0, sizeof(nc_channel_state_t));
        for (int k = 0; k < NC_SUBBANDS; k++) {
            dec->ch_state[c].step_size[k] = 1.0;
        }
    }
    dec->frames_decoded = 0;
}

/* ---- File I/O ---- */

int nc_write_file_header(FILE *f, const nc_encoder_t *enc) {
    nc_file_header_t hdr;
    hdr.magic = NC_MAGIC;
    hdr.version = NC_VERSION;
    hdr.channels = enc->channels;
    hdr.quality = (uint8_t)enc->quality;
    hdr.reserved = 0;
    hdr.sample_rate = enc->sample_rate;
    hdr.total_frames = enc->frames_encoded;
    hdr.total_samples = enc->total_samples;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) return -1;
    return 0;
}

int nc_read_file_header(FILE *f, nc_file_header_t *hdr) {
    if (fread(hdr, sizeof(*hdr), 1, f) != 1) return -1;
    if (hdr->magic != NC_MAGIC) {
        fprintf(stderr, "Error: not a valid NC1 file (bad magic)\n");
        return -1;
    }
    if (hdr->version != NC_VERSION) {
        fprintf(stderr, "Error: unsupported NC1 version %d\n", hdr->version);
        return -1;
    }
    return 0;
}

size_t nc_max_frame_bytes(nc_quality_t quality, uint8_t channels) {
    int avg_bits = nc_base_bits_per_sample(quality);
    /* Per channel: 8 scale_factors (16 bits each) + 8 bit_alloc (3 bits each)
     *            + 16 blocks * 8 subbands * avg_bits */
    size_t bits_per_ch = NC_SUBBANDS * 16 + NC_SUBBANDS * 3
                       + NC_BLOCKS_PER_FRAME * NC_SUBBANDS * avg_bits;
    return (bits_per_ch * channels + 7) / 8 + 4;  /* +4 for safety margin */
}

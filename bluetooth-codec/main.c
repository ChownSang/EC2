#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nanocodec.h"
#include "wav.h"

static void print_usage(const char *prog) {
    printf("NanoCodec (NC1) - Custom Bluetooth Audio Codec\n\n");
    printf("Usage:\n");
    printf("  %s encode <input.wav> <output.nc1> [quality]\n", prog);
    printf("  %s decode <input.nc1> <output.wav>\n", prog);
    printf("  %s test   [quality]                        (encode+decode test tone)\n", prog);
    printf("  %s info   <file.nc1>                       (show file info)\n\n", prog);
    printf("Quality: low (128kbps), med (200kbps, default), high (320kbps)\n");
}

static nc_quality_t parse_quality(const char *str) {
    if (!str) return NC_QUALITY_MED;
    if (strcmp(str, "low") == 0) return NC_QUALITY_LOW;
    if (strcmp(str, "high") == 0) return NC_QUALITY_HIGH;
    return NC_QUALITY_MED;
}

static const char *quality_name(nc_quality_t q) {
    switch (q) {
        case NC_QUALITY_LOW:  return "low (~128kbps)";
        case NC_QUALITY_MED:  return "med (~200kbps)";
        case NC_QUALITY_HIGH: return "high (~320kbps)";
        default: return "unknown";
    }
}

static int do_encode(const char *input_path, const char *output_path, nc_quality_t quality) {
    wav_file_t wav;
    if (wav_read(input_path, &wav) != 0) {
        fprintf(stderr, "Error: cannot read WAV file '%s'\n", input_path);
        return 1;
    }

    printf("Input: %s\n", input_path);
    printf("  Sample rate: %d Hz\n", wav.sample_rate);
    printf("  Channels:    %d\n", wav.channels);
    printf("  Samples:     %d (%.2f sec)\n", wav.num_samples,
           (double)wav.num_samples / wav.sample_rate);
    printf("  Quality:     %s\n", quality_name(quality));

    nc_encoder_t enc;
    nc_encoder_init(&enc, wav.sample_rate, (uint8_t)wav.channels, quality);

    /* Open output */
    FILE *fout = fopen(output_path, "wb");
    if (!fout) {
        fprintf(stderr, "Error: cannot create '%s'\n", output_path);
        wav_free(&wav);
        return 1;
    }

    /* Reserve space for header (write at end) */
    nc_file_header_t dummy_hdr = {0};
    fwrite(&dummy_hdr, sizeof(dummy_hdr), 1, fout);

    /* Encode frame by frame */
    size_t max_frame_buf = nc_max_frame_bytes(quality, (uint8_t)wav.channels);
    uint8_t *frame_buf = (uint8_t *)malloc(max_frame_buf);
    int total_pcm = wav.num_samples;
    int offset = 0;
    int frames = 0;

    while (offset < total_pcm) {
        int remaining = total_pcm - offset;
        int frame_samples = (remaining < NC_FRAME_SAMPLES) ? remaining : NC_FRAME_SAMPLES;

        nc_bitstream_t bs;
        nc_bs_init_write(&bs, frame_buf, max_frame_buf);

        const int16_t *pcm_ptr;
        int16_t zero_buf[NC_FRAME_SAMPLES * NC_MAX_CHANNELS];
        if (wav.channels > 1) {
            pcm_ptr = wav.data + offset * wav.channels;
        } else {
            pcm_ptr = wav.data + offset;
        }

        /* Zero-pad last frame if needed */
        if (frame_samples < NC_FRAME_SAMPLES) {
            memset(zero_buf, 0, sizeof(zero_buf));
            memcpy(zero_buf, pcm_ptr, frame_samples * wav.channels * sizeof(int16_t));
            pcm_ptr = zero_buf;
        }

        nc_encode_frame(&enc, pcm_ptr, NC_FRAME_SAMPLES, &bs);

        /* Write frame size + frame data */
        uint16_t frame_bytes = (uint16_t)nc_bs_bytes_written(&bs);
        fwrite(&frame_bytes, 2, 1, fout);
        fwrite(frame_buf, 1, frame_bytes, fout);

        offset += NC_FRAME_SAMPLES;
        frames++;
    }

    /* Go back and write real header */
    enc.total_samples = (uint32_t)total_pcm;
    fseek(fout, 0, SEEK_SET);
    nc_write_file_header(fout, &enc);

    /* Get output file size */
    fseek(fout, 0, SEEK_END);
    long output_size = ftell(fout);
    fclose(fout);
    free(frame_buf);

    double input_size = (double)total_pcm * wav.channels * 2;
    double ratio = input_size / output_size;
    double bitrate = (double)output_size * 8.0 * wav.sample_rate / total_pcm / 1000.0;

    printf("\nEncoded: %s\n", output_path);
    printf("  Frames:      %d\n", frames);
    printf("  File size:   %ld bytes\n", output_size);
    printf("  Compression: %.1fx\n", ratio);
    printf("  Bitrate:     %.0f kbps\n", bitrate);

    wav_free(&wav);
    return 0;
}

static int do_decode(const char *input_path, const char *output_path) {
    FILE *fin = fopen(input_path, "rb");
    if (!fin) {
        fprintf(stderr, "Error: cannot open '%s'\n", input_path);
        return 1;
    }

    nc_file_header_t hdr;
    if (nc_read_file_header(fin, &hdr) != 0) {
        fclose(fin);
        return 1;
    }

    printf("Input: %s\n", input_path);
    printf("  Sample rate: %d Hz\n", hdr.sample_rate);
    printf("  Channels:    %d\n", hdr.channels);
    printf("  Quality:     %s\n", quality_name((nc_quality_t)hdr.quality));
    printf("  Frames:      %d\n", hdr.total_frames);
    printf("  Samples:     %d (%.2f sec)\n", hdr.total_samples,
           (double)hdr.total_samples / hdr.sample_rate);

    nc_decoder_t dec;
    nc_decoder_init(&dec, hdr.sample_rate, hdr.channels, (nc_quality_t)hdr.quality);

    /* Allocate output PCM */
    uint32_t total_alloc = hdr.total_frames * NC_FRAME_SAMPLES;
    int16_t *pcm_out = (int16_t *)calloc(total_alloc * hdr.channels, sizeof(int16_t));
    if (!pcm_out) {
        fprintf(stderr, "Error: out of memory\n");
        fclose(fin);
        return 1;
    }

    size_t max_frame_buf = nc_max_frame_bytes((nc_quality_t)hdr.quality, hdr.channels);
    uint8_t *frame_buf = (uint8_t *)malloc(max_frame_buf);
    int pcm_offset = 0;

    for (uint32_t f = 0; f < hdr.total_frames; f++) {
        uint16_t frame_bytes;
        if (fread(&frame_bytes, 2, 1, fin) != 1) break;
        if (frame_bytes > max_frame_buf) {
            fprintf(stderr, "Error: corrupt frame %d (size=%d)\n", f, frame_bytes);
            break;
        }
        if (fread(frame_buf, 1, frame_bytes, fin) != frame_bytes) break;

        nc_bitstream_t bs;
        nc_bs_init_read(&bs, frame_buf, frame_bytes);

        int16_t *out_ptr;
        if (hdr.channels > 1) {
            out_ptr = pcm_out + pcm_offset * hdr.channels;
        } else {
            out_ptr = pcm_out + pcm_offset;
        }

        nc_decode_frame(&dec, &bs, out_ptr, NC_FRAME_SAMPLES);
        pcm_offset += NC_FRAME_SAMPLES;
    }

    fclose(fin);
    free(frame_buf);

    /* Write output WAV (trim to original sample count) */
    wav_file_t wav_out;
    wav_out.channels = hdr.channels;
    wav_out.sample_rate = hdr.sample_rate;
    wav_out.bits_per_sample = 16;
    wav_out.num_samples = hdr.total_samples;
    wav_out.data = pcm_out;

    if (wav_write(output_path, &wav_out) != 0) {
        fprintf(stderr, "Error: cannot write '%s'\n", output_path);
        free(pcm_out);
        return 1;
    }

    printf("\nDecoded: %s\n", output_path);
    printf("  Samples:     %d\n", hdr.total_samples);
    free(pcm_out);
    return 0;
}

static int do_test(nc_quality_t quality) {
    printf("=== NanoCodec Self-Test ===\n\n");
    printf("Generating test signal: 440Hz + harmonics, 44100Hz, mono, 2 sec\n\n");

    wav_file_t wav;
    wav_generate_test(&wav, 44100, 1, 440.0, 2.0);

    /* Save original */
    wav_write("test_original.wav", &wav);

    /* Encode */
    nc_encoder_t enc;
    nc_encoder_init(&enc, wav.sample_rate, (uint8_t)wav.channels, quality);

    FILE *fout = fopen("test_encoded.nc1", "wb");
    nc_file_header_t dummy = {0};
    fwrite(&dummy, sizeof(dummy), 1, fout);

    size_t max_fb = nc_max_frame_bytes(quality, 1);
    uint8_t *fb = (uint8_t *)malloc(max_fb);
    int offset = 0;

    while (offset < (int)wav.num_samples) {
        int16_t frame_pcm[NC_FRAME_SAMPLES] = {0};
        int remaining = wav.num_samples - offset;
        int copy = (remaining < NC_FRAME_SAMPLES) ? remaining : NC_FRAME_SAMPLES;
        memcpy(frame_pcm, wav.data + offset, copy * sizeof(int16_t));

        nc_bitstream_t bs;
        nc_bs_init_write(&bs, fb, max_fb);
        nc_encode_frame(&enc, frame_pcm, NC_FRAME_SAMPLES, &bs);

        uint16_t bytes = (uint16_t)nc_bs_bytes_written(&bs);
        fwrite(&bytes, 2, 1, fout);
        fwrite(fb, 1, bytes, fout);
        offset += NC_FRAME_SAMPLES;
    }

    enc.total_samples = wav.num_samples;
    fseek(fout, 0, SEEK_SET);
    nc_write_file_header(fout, &enc);
    fseek(fout, 0, SEEK_END);
    long enc_size = ftell(fout);
    fclose(fout);

    /* Decode */
    do_decode("test_encoded.nc1", "test_output.wav");

    /* Compare: compute SNR */
    wav_file_t wav_dec;
    wav_read("test_output.wav", &wav_dec);

    double signal_power = 0, noise_power = 0;
    uint32_t cmp_samples = wav.num_samples < wav_dec.num_samples
        ? wav.num_samples : wav_dec.num_samples;
    for (uint32_t i = 0; i < cmp_samples; i++) {
        double orig = wav.data[i];
        double decoded = wav_dec.data[i];
        signal_power += orig * orig;
        noise_power += (orig - decoded) * (orig - decoded);
    }

    double snr = 10.0 * log10(signal_power / (noise_power + 1e-10));
    double input_bytes = wav.num_samples * 2.0;
    double ratio = input_bytes / enc_size;
    double bitrate = (double)enc_size * 8.0 * wav.sample_rate / wav.num_samples / 1000.0;

    printf("\n=== Test Results ===\n");
    printf("  Quality:     %s\n", quality_name(quality));
    printf("  Original:    %d bytes\n", wav.num_samples * 2);
    printf("  Encoded:     %ld bytes\n", enc_size);
    printf("  Compression: %.1fx\n", ratio);
    printf("  Bitrate:     %.0f kbps\n", bitrate);
    printf("  SNR:         %.1f dB\n", snr);
    printf("  Latency:     %.1f ms (%.0f samples/frame)\n",
           1000.0 * NC_FRAME_SAMPLES / wav.sample_rate, (double)NC_FRAME_SAMPLES);
    printf("\n  Files: test_original.wav, test_encoded.nc1, test_output.wav\n");

    free(fb);
    wav_free(&wav);
    wav_free(&wav_dec);
    return 0;
}

static int do_info(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", path);
        return 1;
    }

    nc_file_header_t hdr;
    if (nc_read_file_header(f, &hdr) != 0) {
        fclose(f);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fclose(f);

    double duration = (double)hdr.total_samples / hdr.sample_rate;
    double bitrate = (double)file_size * 8.0 * hdr.sample_rate / hdr.total_samples / 1000.0;

    printf("NanoCodec File: %s\n", path);
    printf("  Version:     NC%d\n", hdr.version);
    printf("  Sample rate: %d Hz\n", hdr.sample_rate);
    printf("  Channels:    %d\n", hdr.channels);
    printf("  Quality:     %s\n", quality_name((nc_quality_t)hdr.quality));
    printf("  Frames:      %d\n", hdr.total_frames);
    printf("  Duration:    %.2f sec (%d samples)\n", duration, hdr.total_samples);
    printf("  File size:   %ld bytes\n", file_size);
    printf("  Bitrate:     %.0f kbps\n", bitrate);
    printf("  Latency:     %.1f ms\n", 1000.0 * NC_FRAME_SAMPLES / hdr.sample_rate);

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "encode") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s encode <input.wav> <output.nc1> [quality]\n", argv[0]);
            return 1;
        }
        nc_quality_t q = parse_quality(argc > 4 ? argv[4] : NULL);
        return do_encode(argv[2], argv[3], q);
    }

    if (strcmp(argv[1], "decode") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s decode <input.nc1> <output.wav>\n", argv[0]);
            return 1;
        }
        return do_decode(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "test") == 0) {
        nc_quality_t q = parse_quality(argc > 2 ? argv[2] : NULL);
        return do_test(q);
    }

    if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s info <file.nc1>\n", argv[0]);
            return 1;
        }
        return do_info(argv[2]);
    }

    print_usage(argv[0]);
    return 1;
}

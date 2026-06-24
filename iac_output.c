/*
IAC Exporter v1.0 - Offline converter from .iac to .wav (32-bit Float)
Compile:
    zig cc iac_output.c -O3 -lm -lpthread -o iac_output.exe
Usage:
    iac_output.exe file.iac output.wav
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define IAC_MAGIC "IAC1"
#define MAX_CHANNELS 8
#define BLOCK_SIZE 1024
#define NORMALIZATION_FACTOR 316227.766f // Inverts the 110 dB reduction
#define CHUNK_SECONDS 10 // Tamanho de cada bloco de processamento, em segundos

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint64_t total_samples;
} IACHeader;

typedef struct {
    FILE *file;
    size_t total_bytes_to_read;
    size_t bytes_read_so_far;
    uint8_t buffer[16384];
    size_t byte_count;
    size_t byte_pos;
    uint8_t current_byte;
    int bits_remaining;
    uint64_t *seek_table;         // posições absolutas (no arquivo) de cada chunk
    uint64_t seek_table_count;
    uint64_t bitstream_start_pos; // posição absoluta no arquivo onde o bitstream comprimido começa
} BitStream;

void init_bitstream(BitStream *bs, FILE *f, uint64_t channel_offset, size_t channel_total_size) {
    fseek(f, channel_offset, SEEK_SET);

    uint64_t num_chunks = 0;
    fread(&num_chunks, 8, 1, f);

    bs->seek_table = (uint64_t*)malloc((num_chunks > 0 ? num_chunks : 1) * sizeof(uint64_t));
    if (num_chunks > 0) fread(bs->seek_table, 8, num_chunks, f);
    bs->seek_table_count = num_chunks;

    uint64_t bitstream_start = (uint64_t)ftell(f); // já posicionado certo, após a seek table

    bs->file = f;
    bs->bitstream_start_pos = bitstream_start;
    bs->total_bytes_to_read = channel_offset + channel_total_size - bitstream_start;
    bs->bytes_read_so_far = 0;
    bs->byte_count = 0;
    bs->byte_pos = 0;
    bs->current_byte = 0;
    bs->bits_remaining = 0;
}

uint32_t read_bits(BitStream *bs, int num_bits) {
    uint32_t result = 0;
    for (int i = 0; i < num_bits; i++) {
        if (bs->bits_remaining == 0) {
            if (bs->byte_pos >= bs->byte_count) {
                size_t remaining_in_file = bs->total_bytes_to_read - bs->bytes_read_so_far;
                if (remaining_in_file == 0) return 0;
                size_t to_read = sizeof(bs->buffer);
                if (to_read > remaining_in_file) to_read = remaining_in_file;
                
                size_t read_bytes = fread(bs->buffer, 1, to_read, bs->file);
                if (read_bytes == 0) return 0;
                bs->byte_count = read_bytes;
                bs->byte_pos = 0;
                bs->bytes_read_so_far += read_bytes;
            }
            bs->current_byte = bs->buffer[bs->byte_pos++];
            bs->bits_remaining = 8;
        }
        int bit = (bs->current_byte >> 7) & 1;
        bs->current_byte <<= 1;
        bs->bits_remaining--;
        result = (result << 1) | bit;
    }
    return result;
}

void align_bitstream_to_byte(BitStream *bs) {
    bs->bits_remaining = 0;
}

// Kept identical to the original codec to not break decoding
int32_t decode_next_sample(BitStream *bs, int32_t *last_sample, int *current_k, int *samples_left_in_block, uint64_t sample_idx, uint32_t sample_rate) {
    size_t blocks_per_chunk = (5 * sample_rate) / BLOCK_SIZE;
    if (blocks_per_chunk == 0) blocks_per_chunk = 1;

    size_t b_idx_check = sample_idx / BLOCK_SIZE;
    if (sample_idx % BLOCK_SIZE == 0 && (b_idx_check % blocks_per_chunk == 0)) {
        align_bitstream_to_byte(bs);
    }

    if (*samples_left_in_block == 0) {
        *current_k = read_bits(bs, 4);
        *samples_left_in_block = BLOCK_SIZE;
    }

    size_t b_idx = sample_idx / BLOCK_SIZE;
    size_t sample_in_block = BLOCK_SIZE - *samples_left_in_block;

    if (sample_in_block == 0 && (b_idx % blocks_per_chunk == 0)) {
        int32_t raw = (int32_t)read_bits(bs, 15);
        if (raw & 0x4000) raw -= 0x8000;
        *last_sample = raw;
        (*samples_left_in_block)--;
        return *last_sample;
    }

    uint32_t q = 0;
    while (read_bits(bs, 1) == 1) q++;

    uint32_t r = read_bits(bs, *current_k);
    uint32_t u_val = (q << (*current_k)) | r;
    int32_t delta = (int32_t)(u_val >> 1) ^ -(int32_t)(u_val & 1);

    *last_sample += delta;
    (*samples_left_in_block)--;

    return *last_sample;
}

// NEW FUNCTION: Writes the standard RIFF WAVE header for 32-bit IEEE Float
void write_wav_header(FILE *f, uint32_t sample_rate, uint16_t channels, uint64_t total_samples) {
    uint32_t subchunk2_size = total_samples * channels * sizeof(int32_t); // Alterado para int32_t
    uint32_t chunk_size = 36 + subchunk2_size;
    uint32_t byte_rate = sample_rate * channels * sizeof(int32_t);       // Alterado para int32_t
    uint16_t block_align = channels * sizeof(int32_t);                   // Alterado para int32_t
    uint16_t bits_per_sample = 32;
    uint16_t audio_format = 1; // MODIFICADO: 1 = PCM Int (s32), 3 = IEEE Float

    fwrite("RIFF", 1, 4, f);
    fwrite(&chunk_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    
    fwrite("fmt ", 1, 4, f);
    uint32_t subchunk1_size = 16;
    fwrite(&subchunk1_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);
    
    fwrite("data", 1, 4, f);
    fwrite(&subchunk2_size, 4, 1, f);
}

// Each channel is fully independent (own FILE*, own BitStream, own delta
// state), exactly like the encoder treats them when compressing -- so each
// one can be decoded on its own thread with zero cross-channel dependency.
// Aqui cada thread decodifica só um BLOCO (start_idx .. start_idx+num_frames),
// não o canal inteiro -- o estado (last_sample/k/samples_left) é mantido
// entre as chamadas de iac_output_main e continua de onde parou no bloco anterior.
typedef struct {
    BitStream *bs;
    int32_t *last_sample;
    int *current_k;
    int *samples_left_in_block;
    uint64_t start_idx;     
    uint64_t num_frames;    
    uint32_t sample_rate;
    int32_t *out_buffer;      // MODIFICADO: Alterado para int32_t*
} ChannelDecodeJob;

void *decode_channel_thread(void *arg) {
    ChannelDecodeJob *job = (ChannelDecodeJob*)arg;

    for (uint64_t n = 0; n < job->num_frames; n++) {
        uint64_t i = job->start_idx + n; 
        int32_t sample = decode_next_sample(job->bs,
                                            job->last_sample,
                                            job->current_k,
                                            job->samples_left_in_block,
                                            i,
                                            job->sample_rate);

        // Aplica o ganho inverso do encoder
        float final_val = (float)sample * NORMALIZATION_FACTOR;

        // Limita os extremos (clamping) para não estourar o range de um s32
        if (final_val > 2147483647.0f) final_val = 2147483647.0f;
        if (final_val < -2147483648.0f) final_val = -2147483648.0f;

        job->out_buffer[n] = (int32_t)final_val; // Salva diretamente como s32
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <file.iac> <output.wav>\n", argv[0]);
        return 1;
    }

    printf("\n=== IAC Exporter v0.1.0 ===\n\n");

    FILE *f_in = fopen(argv[1], "rb");
    if (!f_in) {
        printf("Error opening input file: %s\n", argv[1]);
        return 1;
    }

    char magic[4];
    fread(magic, 1, 4, f_in);
    if (memcmp(magic, IAC_MAGIC, 4) != 0) {
        printf("Error: File is not a valid IAC file\n");
        fclose(f_in);
        return 1;
    }

    IACHeader header;
    uint32_t version;
    fread(&version, 4, 1, f_in);
    fread(&header.sample_rate, 4, 1, f_in);
    fread(&header.channels, 2, 1, f_in);
    fread(&header.bits_per_sample, 2, 1, f_in);
    fread(&header.total_samples, 8, 1, f_in);

    if (header.channels > MAX_CHANNELS) {
        printf("Error: File exceeds the limit of %d supported channels.\n", MAX_CHANNELS);
        fclose(f_in);
        return 1;
    }

    printf("Converting: %d Hz, %d channels, %llu total samples per channel...\n",
           header.sample_rate, header.channels, (unsigned long long)header.total_samples);

    // Ignores extra metadata and goes to the channel table
    fseek(f_in, 32, SEEK_CUR);

    uint64_t offsets[MAX_CHANNELS], sizes[MAX_CHANNELS];
    for (int i = 0; i < header.channels; i++) {
        fread(&offsets[i], 8, 1, f_in);
        fread(&sizes[i], 8, 1, f_in);
    }

    // Initialization of individual files and states per channel
    FILE *channel_files[MAX_CHANNELS];
    BitStream channel_streams[MAX_CHANNELS];
    int32_t channel_last_samples[MAX_CHANNELS] = {0};
    int channel_current_k[MAX_CHANNELS] = {0};
    int channel_samples_left_in_block[MAX_CHANNELS] = {0};

    for (int i = 0; i < header.channels; i++) {
        channel_files[i] = fopen(argv[1], "rb");
        init_bitstream(&channel_streams[i], channel_files[i], offsets[i], sizes[i]);
    }

    // Opens the output WAV file
    FILE *f_out = fopen(argv[2], "wb");
    if (!f_out) {
        printf("Error creating output file: %s\n", argv[2]);
        for (int i = 0; i < header.channels; i++) fclose(channel_files[i]);
        fclose(f_in);
        return 1;
    }

    // Writes the 32-bit float .wav header
    write_wav_header(f_out, header.sample_rate, header.channels, header.total_samples);

    // Tamanho do bloco em frames (CHUNK_SECONDS de áudio por vez). A memória
    // usada agora é proporcional ao bloco, não ao arquivo inteiro.
    uint64_t chunk_frames = (uint64_t)CHUNK_SECONDS * header.sample_rate;
    if (chunk_frames == 0) chunk_frames = 1;
    if (chunk_frames > header.total_samples) chunk_frames = header.total_samples;
    if (chunk_frames == 0) chunk_frames = 1; // arquivo vazio, evita divisão/alocação de tamanho 0

    printf("Decoding in chunks of %ds (%llu frames each), %d channels in parallel...\n",
           CHUNK_SECONDS, (unsigned long long)chunk_frames, header.channels);

    // Buffer de cada canal -- agora só do tamanho de UM bloco
    int32_t *channel_buffers[MAX_CHANNELS]; // MODIFICADO: de float para int32_t
    for (int c = 0; c < header.channels; c++) {
        channel_buffers[c] = (int32_t*)malloc(chunk_frames * sizeof(int32_t)); // Alterado para int32_t
        if (!channel_buffers[c]) {
            printf("Error: not enough RAM to allocate chunk buffers.\n");
            for (int k = 0; k < c; k++) free(channel_buffers[k]);
            fclose(f_out);
            for (int i = 0; i < header.channels; i++) fclose(channel_files[i]);
            fclose(f_in);
            return 1;
        }
    }

    // Buffer temporário para intercalar os frames de áudio (L/R/L/R...)
    int32_t *frame_buffer = (int32_t*)malloc(header.channels * sizeof(int32_t)); // MODIFICADO: de float para int32_t;

    pthread_t threads[MAX_CHANNELS];
    ChannelDecodeJob jobs[MAX_CHANNELS];

    // PROCESSAMENTO EM BLOCOS: decodifica um bloco (paralelo por canal),
    // intercala e escreve, decodifica o próximo bloco, e assim por diante.
    for (uint64_t start = 0; start < header.total_samples; start += chunk_frames) {
        uint64_t this_chunk = header.total_samples - start;
        if (this_chunk > chunk_frames) this_chunk = chunk_frames;

        // Dispara uma thread por canal pra decodificar este bloco
        for (int c = 0; c < header.channels; c++) {
            jobs[c].bs = &channel_streams[c];
            jobs[c].last_sample = &channel_last_samples[c];
            jobs[c].current_k = &channel_current_k[c];
            jobs[c].samples_left_in_block = &channel_samples_left_in_block[c];
            jobs[c].start_idx = start;
            jobs[c].num_frames = this_chunk;
            jobs[c].sample_rate = header.sample_rate;
            jobs[c].out_buffer = channel_buffers[c];
            pthread_create(&threads[c], NULL, decode_channel_thread, &jobs[c]);
        }
        for (int c = 0; c < header.channels; c++) {
            pthread_join(threads[c], NULL);
        }

        // Intercala e escreve este bloco (leitura de RAM, é rápido)
        for (uint64_t n = 0; n < this_chunk; n++) {
            for (int c = 0; c < header.channels; c++) {
                frame_buffer[c] = channel_buffers[c][n];
            }
            // MODIFICADO: tamanho do elemento alterado para sizeof(int32_t)
            fwrite(frame_buffer, sizeof(int32_t), header.channels, f_out); 
        }

        printf("\rProgress: %.1f%%", ((double)(start + this_chunk) / header.total_samples) * 100.0);
        fflush(stdout);
    }

    printf("\rProgress: 100.0%%\nExport completed successfully!\nGenerated file: %s\n", argv[2]);

    // Cleanup
    free(frame_buffer);
    for (int c = 0; c < header.channels; c++) free(channel_buffers[c]);
    fclose(f_out);
    for (int i = 0; i < header.channels; i++) fclose(channel_files[i]);
    fclose(f_in);

    return 0;
}
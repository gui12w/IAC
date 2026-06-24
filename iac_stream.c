/*
    IAC Streamer v1.2 - Transmissor nativo para pipelines do Windows com sincronia absoluta
    Otimização: Fast-Forward Paralelo Multicore para áudio pesado (5.1 / 7.1)
    Compilar:
        zig cc iac_stream.c -O3 -pthread -lm -o iac_stream.exe
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <windows.h> // Necessário para as Pipelines do Windows
#include <pthread.h> // Infraestrutura Multi-core para aceleração de Seek

#define IAC_MAGIC "IAC1"
#define MAX_CHANNELS 8
#define BLOCK_SIZE 1024
#define NORMALIZATION_FACTOR 316227.766f

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint64_t total_samples;
} IACHeader;

typedef struct {
    FILE *file;
    uint64_t total_bytes_to_read; // Alterado de size_t para uint64_t
    uint64_t bytes_read_so_far;   // Alterado de size_t para uint64_t
    uint8_t buffer[16384];
    size_t byte_count;
    size_t byte_pos;
    uint8_t current_byte;
    int bits_remaining;
    uint64_t *seek_table;       
    uint64_t seek_table_count;  
    uint64_t bitstream_start_pos; 
} BitStream;

// Estrutura de argumentos para aceleração do avanço rápido
typedef struct {
    BitStream *bs;
    int32_t *last_sample;
    int *current_k;
    int *samples_left_in_block;
    uint64_t target_sample;
    uint32_t sample_rate;
} FastForwardArg;

void init_bitstream(BitStream *bs, FILE *f, uint64_t channel_offset, uint64_t channel_total_size) { // Alterado para uint64_t
    _fseeki64(f, channel_offset, SEEK_SET); // Alterado para _fseeki64

    uint64_t num_chunks;
    fread(&num_chunks, 8, 1, f);

    bs->seek_table = (uint64_t*)malloc(num_chunks * sizeof(uint64_t));
    fread(bs->seek_table, 8, num_chunks, f);
    bs->seek_table_count = num_chunks;

    uint64_t bitstream_start = _ftelli64(f); // Alterado para _ftelli64

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
                uint64_t remaining_in_file = bs->total_bytes_to_read - bs->bytes_read_so_far;
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

void seek_bitstream_to_chunk(BitStream *bs, uint64_t file_byte_pos) {
    _fseeki64(bs->file, file_byte_pos, SEEK_SET); // Alterado para _fseeki64
    bs->byte_pos = 0;
    bs->byte_count = 0;
    bs->bits_remaining = 0;
    bs->current_byte = 0;
    bs->bytes_read_so_far = file_byte_pos - bs->bitstream_start_pos;
}

// Thread operária focada exclusivamente em devorar o bitstream em velocidade máxima
void *channel_fast_forward_worker(void *arg) {
    FastForwardArg *ffa = (FastForwardArg*)arg;

    size_t blocks_per_chunk = (5 * ffa->sample_rate) / BLOCK_SIZE;
    if (blocks_per_chunk == 0) blocks_per_chunk = 1;
    size_t samples_per_chunk = blocks_per_chunk * BLOCK_SIZE;

    uint64_t chunk_idx = ffa->target_sample / samples_per_chunk;
    if (chunk_idx >= ffa->bs->seek_table_count) chunk_idx = ffa->bs->seek_table_count - 1;
    uint64_t chunk_start_sample = chunk_idx * samples_per_chunk;

    seek_bitstream_to_chunk(ffa->bs, ffa->bs->seek_table[chunk_idx]);

    *ffa->current_k = 0;
    *ffa->samples_left_in_block = 0;
    *ffa->last_sample = 0;

    // decodifica só o resíduo dentro do chunk, não o arquivo inteiro
    for (uint64_t i = chunk_start_sample; i < ffa->target_sample; i++) {
        decode_next_sample(ffa->bs, ffa->last_sample, ffa->current_k,
                            ffa->samples_left_in_block, i, ffa->sample_rate);
    }
    return NULL;
}

void write_wav_header_to_pipe(HANDLE hPipe, uint32_t sample_rate, uint16_t channels, uint64_t total_samples) {
    uint64_t full_subchunk2_size = total_samples * channels * sizeof(int32_t);
    
    // Se estourar o limite de 32-bits (4GB), define como tamanho dinâmico/infinito para streaming (0xFFFFFFFF)
    uint32_t subchunk2_size = (full_subchunk2_size > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)full_subchunk2_size;
    uint32_t chunk_size = (full_subchunk2_size > 0xFFFFFFFF) ? 0xFFFFFFFF : (36 + subchunk2_size);
    
    uint32_t byte_rate = sample_rate * channels * sizeof(int32_t);       
    uint16_t block_align = channels * sizeof(int32_t);                   
    uint16_t bits_per_sample = 32;
    uint16_t audio_format = 1; 
    DWORD written;

    WriteFile(hPipe, "RIFF", 4, &written, NULL);
    WriteFile(hPipe, &chunk_size, 4, &written, NULL);
    WriteFile(hPipe, "WAVE", 4, &written, NULL);
    WriteFile(hPipe, "fmt ", 4, &written, NULL);
    uint32_t subchunk1_size = 16;
    WriteFile(hPipe, &subchunk1_size, 4, &written, NULL);
    WriteFile(hPipe, &audio_format, 2, &written, NULL);
    WriteFile(hPipe, &channels, 2, &written, NULL);
    WriteFile(hPipe, &sample_rate, 4, &written, NULL);
    WriteFile(hPipe, &byte_rate, 4, &written, NULL);
    WriteFile(hPipe, &block_align, 2, &written, NULL);
    WriteFile(hPipe, &bits_per_sample, 2, &written, NULL);
    WriteFile(hPipe, "data", 4, &written, NULL);
    WriteFile(hPipe, &subchunk2_size, 4, &written, NULL);
}

int main(int argc, char **argv) {
    if (argc < 3) return 1;

    const char *iac_path = argv[1];
    double start_time_seconds = atof(argv[2]);

    FILE *f_in = fopen(iac_path, "rb");
    if (!f_in) return 1;

    char magic[4];
    fread(magic, 1, 4, f_in);
    if (memcmp(magic, IAC_MAGIC, 4) != 0) { fclose(f_in); return 1; }

    IACHeader header;
    uint32_t version;
    fread(&version, 4, 1, f_in);
    fread(&header.sample_rate, 4, 1, f_in);
    fread(&header.channels, 2, 1, f_in);
    fread(&header.bits_per_sample, 2, 1, f_in);
    fread(&header.total_samples, 8, 1, f_in);

    _fseeki64(f_in, 32, SEEK_CUR);

    uint64_t offsets[MAX_CHANNELS], sizes[MAX_CHANNELS];
    for (int i = 0; i < header.channels; i++) {
        fread(&offsets[i], 8, 1, f_in);
        fread(&sizes[i], 8, 1, f_in);
    }

    FILE *channel_files[MAX_CHANNELS];
    BitStream channel_streams[MAX_CHANNELS];
    int32_t channel_last_samples[MAX_CHANNELS] = {0};
    int channel_current_k[MAX_CHANNELS] = {0};
    int channel_samples_left_in_block[MAX_CHANNELS] = {0};

    for (int i = 0; i < header.channels; i++) {
        channel_files[i] = fopen(iac_path, "rb");
        init_bitstream(&channel_streams[i], channel_files[i], offsets[i], sizes[i]);
    }

    // ====================================================================
    // PASSO 1: Cria e conecta a Pipeline Virtual
    // ====================================================================
    HANDLE hPipe = CreateNamedPipeA(
        "\\\\.\\pipe\\iac_stream",
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1, 65536, 65536, 0, NULL
    );
    if (hPipe == INVALID_HANDLE_VALUE) { for (int i = 0; i < header.channels; i++) fclose(channel_files[i]); fclose(f_in); return 1; }

    ConnectNamedPipe(hPipe, NULL);

    // ====================================================================
    // PASSO 2: Envia o Cabeçalho WAV COMPLETO e injeta o silêncio de padding
    // ====================================================================
    uint64_t start_sample = (uint64_t)(start_time_seconds * header.sample_rate);
    if (start_sample > header.total_samples) start_sample = header.total_samples;

    write_wav_header_to_pipe(hPipe, header.sample_rate, header.channels, header.total_samples);

    if (start_sample > 0) {
        int32_t *silence_buffer = (int32_t*)calloc(header.channels * 1024, sizeof(int32_t)); // Alterado para int32_t
        if (silence_buffer) {
            uint64_t silence_written = 0;
            while (silence_written < start_sample) {
                uint64_t to_write = start_sample - silence_written;
                if (to_write > 1024) to_write = 1024;
                DWORD written;
                if (!WriteFile(hPipe, silence_buffer, to_write * header.channels * sizeof(int32_t), &written, NULL)) { // Alterado para int32_t
                    break;
                }
                silence_written += to_write;
            }
            free(silence_buffer);
        }
    }

    // ====================================================================
    // PASSO 3: OTIMIZAÇÃO: Avanço rápido Paralelo Multi-thread (8 Cores para 7.1)
    // ====================================================================
    if (start_sample > 0) {
        pthread_t ff_threads[MAX_CHANNELS];
        FastForwardArg ff_args[MAX_CHANNELS];

        // Dispara uma thread por canal para ler o bitstream em paralelo
        for (int c = 0; c < header.channels; c++) {
            ff_args[c].bs = &channel_streams[c];
            ff_args[c].last_sample = &channel_last_samples[c];
            ff_args[c].current_k = &channel_current_k[c];
            ff_args[c].samples_left_in_block = &channel_samples_left_in_block[c];
            ff_args[c].target_sample = start_sample;
            ff_args[c].sample_rate = header.sample_rate;

            pthread_create(&ff_threads[c], NULL, channel_fast_forward_worker, &ff_args[c]);
        }

        // Aguarda a sincronização síncrona de todas as canais na barreira
        for (int c = 0; c < header.channels; c++) {
            pthread_join(ff_threads[c], NULL);
        }
    }

    // ====================================================================
    // PASSO 4: Transmissão contínua a partir do ponto do seek
    // ====================================================================
    int32_t *frame_buffer = (int32_t*)malloc(header.channels * sizeof(int32_t)); // Alterado para int32_t

    for (uint64_t i = start_sample; i < header.total_samples; i++) {
        for (int c = 0; c < header.channels; c++) {
            int32_t sample = decode_next_sample(&channel_streams[c], &channel_last_samples[c], 
                                                &channel_current_k[c], &channel_samples_left_in_block[c], i, header.sample_rate);

            // Aplica o ganho inverso da redução do encoder
            float final_val = (float)sample * NORMALIZATION_FACTOR;

            // Clampa os limites para não estourar o range de um inteiro de 32 bits assinado
            if (final_val > 2147483647.0f) final_val = 2147483647.0f;
            if (final_val < -2147483648.0f) final_val = -2147483648.0f;

            frame_buffer[c] = (int32_t)final_val; // Salva diretamente como s32
        }

        DWORD written;
        if (!WriteFile(hPipe, frame_buffer, header.channels * sizeof(int32_t), &written, NULL)) { // Alterado para int32_t
            break; 
        }
    }

    free(frame_buffer);
    CloseHandle(hPipe);
    for (int i = 0; i < header.channels; i++) fclose(channel_files[i]);
    fclose(f_in);
    return 0;
}
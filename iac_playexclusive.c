/*
IAC Player v1.3 - Imprecision Audio Codec Player (Multithreaded Channel-Parallel WASAPI Exclusive Edition)
- Núcleo Organizador (Master Thread) gerencia o fluxo em chunks de 5 segundos.
- Núcleos Trabalhadores (Worker Threads) processam a descompressão Rice de cada canal em paralelo.
- Sincronização limpa via POSIX Mutexes e Condition Variables.
- Backend de áudio via Miniaudio configurado em Modo WASAPI Exclusivo.

Compilar:
    zig cc iac_playexclusive.c -std=gnu99 -pthread -O3 -lm -o iacplay_exclusive.exe
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <pthread.h> // Suporte total a Multithread POSIX

#if defined(_WIN32)
    #include <windows.h>
    #define THREAD_SLEEP_MS(ms) Sleep(ms)
#else
    #include <unistd.h>
    #define THREAD_SLEEP_MS(ms) usleep((ms) * 1000)
#endif

#if defined(_WIN32)
    #include <conio.h>
#else
    #if defined(__APPLE__)
        #include <unistd.h>
    #endif
    #include <termios.h>
    int getch(void) {
        struct termios oldattr, newattr;
        int ch;
        tcgetattr(STDIN_FILENO, &oldattr);
        newattr = oldattr;
        newattr.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newattr);
        ch = getchar();
        tcsetattr(STDIN_FILENO, TCSANOW, &oldattr);
        return ch;
    }
#endif

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define IAC_MAGIC "IAC1"
#define MAX_CHANNELS 8
#define BLOCK_SIZE 1024
#define NORMALIZATION_FACTOR 316227.766f // Inverte a redução de 110 dB
#define DECLICK_FADE_FRAMES 64           // ~1.3ms @48kHz, suficiente pra não estalar no fade-in
#define RESUME_MARGIN_SEC 0.10           // colchão mínimo (100ms) pra saber que voltou ter dado
#define PAST_BUFFER_SECONDS 5            // quanto do "passado" fica retido (não sobrescrito) no ring buffer
#define FUTURE_BUFFER_SECONDS 10         // quanto a thread tenta manter decodificado pra frente (presente+futuro)

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint64_t total_samples;
} IACHeader;

// Checkpoint da posição exata no bitstream no INÍCIO de cada chunk de 5s.
typedef struct {
    uint64_t byte_offset;
    uint8_t saved_byte;
    int saved_bits_remaining;
    int valid;
} ChunkCheckpoint;

// ============================================================================
// BIT STREAM READER
// ============================================================================
typedef struct {
    FILE *file;                     // Ponteiro exclusivo deste canal no disco
    size_t total_bytes_to_read;     // Tamanho total da stream deste canal
    size_t bytes_read_so_far;       // Bytes já lidos do disco
    uint8_t buffer[16384];          // Pequeno cache local de 16KB para evitar fseeks/freads minúsculos
    size_t byte_count;              // Bytes válidos atualmente no buffer
    size_t byte_pos;                // Posição atual de leitura no buffer
    uint8_t current_byte;
    int bits_remaining;
    uint64_t *seek_table;           // NOVO: offsets absolutos (no arquivo) do início de cada chunk de 5s
    uint64_t seek_table_count;      // NOVO: quantidade de entradas da seek_table
    uint64_t bitstream_start_pos;   // NOVO: posição absoluta no arquivo onde o bitstream (pós seek_table) começa
} BitStream;

void init_bitstream(BitStream *bs, FILE *f, uint64_t channel_offset, size_t channel_total_size) {
    fseek(f, channel_offset, SEEK_SET);

    uint64_t num_chunks;
    fread(&num_chunks, 8, 1, f);

    bs->seek_table = (uint64_t*)malloc(num_chunks * sizeof(uint64_t));
    fread(bs->seek_table, 8, num_chunks, f);
    bs->seek_table_count = num_chunks;

    uint64_t bitstream_start = ftell(f); // já posicionado certo

    bs->file = f;
    bs->bitstream_start_pos = bitstream_start;
    bs->total_bytes_to_read = channel_offset + channel_total_size - bitstream_start;
    bs->bytes_read_so_far = 0;
    bs->byte_count = 0;
    bs->byte_pos = 0;
    bs->current_byte = 0;
    bs->bits_remaining = 0;
}

void seek_bitstream_to_chunk(BitStream *bs, uint64_t file_byte_pos) {
    fseek(bs->file, file_byte_pos, SEEK_SET);
    bs->byte_pos = 0;
    bs->byte_count = 0;
    bs->bits_remaining = 0;
    bs->current_byte = 0;
    bs->bytes_read_so_far = file_byte_pos - bs->bitstream_start_pos;
}

void align_bitstream_to_byte(BitStream *bs) {
    bs->bits_remaining = 0;
}

size_t iac_chunk_frames(uint32_t sample_rate) {
    size_t blocks_per_chunk = (5 * sample_rate) / BLOCK_SIZE;
    if (blocks_per_chunk == 0) blocks_per_chunk = 1;
    return blocks_per_chunk * BLOCK_SIZE;
}

uint32_t read_bits(BitStream *bs, int num_bits) {
    uint32_t result = 0;
    for (int i = 0; i < num_bits; i++) {
        if (bs->bits_remaining == 0) {
            if (bs->byte_pos >= bs->byte_count) {
                size_t remaining_in_file = bs->total_bytes_to_read - bs->bytes_read_so_far;
                if (remaining_in_file == 0) {
                    return 0; 
                }
                size_t to_read = sizeof(bs->buffer);
                if (to_read > remaining_in_file) to_read = remaining_in_file;
                
                size_t read_bytes = fread(bs->buffer, 1, to_read, bs->file);
                if (read_bytes == 0) {
                    return 0; 
                }
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

// ============================================================================
// DECODER RICE DINÂMICO + DELTA
// ============================================================================
int32_t decode_next_sample(BitStream *bs, int32_t *last_sample, int *current_k, int *samples_left_in_block, uint64_t sample_idx, uint32_t sample_rate) {
    size_t blocks_per_chunk = (5 * sample_rate) / BLOCK_SIZE;
    if (blocks_per_chunk == 0) blocks_per_chunk = 1;

    // NOVO: o encoder faz padding de byte no início de cada chunk de 5s (é isso que
    // permite os offsets da seek_table serem byte-exatos). Sem isso aqui, depois de um
    // seek os bits ficariam desalinhados e a decodificação sairia lixo.
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
    while (read_bits(bs, 1) == 1) {
        q++;
    }

    uint32_t r = read_bits(bs, *current_k);
    uint32_t u_val = (q << (*current_k)) | r;
    int32_t delta = (int32_t)(u_val >> 1) ^ -(int32_t)(u_val & 1);

    *last_sample += delta;
    (*samples_left_in_block)--;

    return *last_sample;
}

// Forward declaration para a estrutura de argumentos da thread do canal
struct iacplay_desc;
typedef struct {
    struct iacplay_desc *ip;
    int channel_idx;
} ChannelWorkerArg;

// ============================================================================
// ESTRUTURA DO PLAYER (MUDANÇA PARA MULTITHREAD EXCLUSIVO)
// ============================================================================
typedef struct iacplay_desc {
    FILE *file;
    FILE *channel_files[MAX_CHANNELS];     
    uint64_t channel_offsets[MAX_CHANNELS]; 
    uint64_t compressed_sizes[MAX_CHANNELS];
    char file_path[512];                    

    IACHeader header;
    volatile uint64_t playback_cursor;      
    volatile uint64_t write_cursor;         
    volatile int is_paused;
    volatile int running;

    volatile int is_starved;
    int fade_phase;              
    int fade_counter;

    float *ring_buffer[MAX_CHANNELS];
    size_t ring_buffer_capacity;            
    size_t future_target_frames;            

    BitStream channel_streams[MAX_CHANNELS];
    int32_t channel_last_samples[MAX_CHANNELS];
    int channel_current_k[MAX_CHANNELS];
    int channel_samples_left_in_block[MAX_CHANNELS];
    uint64_t channel_sample_counters[MAX_CHANNELS];

    ChunkCheckpoint *checkpoints[MAX_CHANNELS];
    size_t chunk_frames;     
    size_t num_chunks;       

    // --- INFRAESTRUTURA DE MULTITHREADING POSIX ---
    pthread_t decode_thread;                        // Thread Mestra (O Organizador)
    pthread_t worker_threads[MAX_CHANNELS];         // Threads Trabalhadoras (Uma por canal)
    pthread_mutex_t master_mutex;
    pthread_mutex_t worker_mutexes[MAX_CHANNELS];   // Mutexes de controle local por canal
    pthread_cond_t worker_cond_start[MAX_CHANNELS]; // Sinalizado pelo Organizador para iniciar trabalho
    pthread_cond_t worker_cond_done[MAX_CHANNELS];  // Sinalizado pelo Trabalhador quando o chunk de 5s acaba
    volatile int worker_status[MAX_CHANNELS];       // Estado: 0 = Ocioso/Pronto, 1 = Processando
    uint64_t worker_target_frame[MAX_CHANNELS];     // Ponto de início virtual que o canal deve renderizar
    ChannelWorkerArg worker_args[MAX_CHANNELS];     // Argumentos empacotados passados às threads
} iacplay_desc;

// ============================================================================
// WORKER THREAD: PROCESSA UM CANAL EXCLUSIVO POR 5 SEGUNDOS (CHUNK)
// ============================================================================
void *channel_worker_func(void *arg) {
    ChannelWorkerArg *wa = (ChannelWorkerArg*)arg;
    iacplay_desc *ip = wa->ip;
    int c = wa->channel_idx;
    size_t buf_cap = ip->ring_buffer_capacity;

    while (ip->running) {
        pthread_mutex_lock(&ip->worker_mutexes[c]);
        while (ip->worker_status[c] == 0 && ip->running) {
            pthread_cond_wait(&ip->worker_cond_start[c], &ip->worker_mutexes[c]);
        }
        if (!ip->running) {
            pthread_mutex_unlock(&ip->worker_mutexes[c]);
            break;
        }
        pthread_mutex_unlock(&ip->worker_mutexes[c]);

        uint64_t start_frame = ip->worker_target_frame[c];
        
        for (uint64_t f = 0; f < ip->chunk_frames; f++) {
            uint64_t current_virtual_frame = start_frame + f;
            uint64_t file_sample_idx = current_virtual_frame % ip->header.total_samples;

            if (file_sample_idx == 0 && current_virtual_frame > 0) {
                fseek(ip->channel_files[c], ip->channel_offsets[c], SEEK_SET);
                init_bitstream(&ip->channel_streams[c], ip->channel_files[c], ip->channel_offsets[c], ip->compressed_sizes[c]);
                ip->channel_last_samples[c] = 0;
                ip->channel_current_k[c] = 0;
                ip->channel_samples_left_in_block[c] = 0;
                ip->channel_sample_counters[c] = 0;
            }

            if (file_sample_idx % ip->chunk_frames == 0) {
                size_t chunk_idx = file_sample_idx / ip->chunk_frames;
                if (chunk_idx < ip->num_chunks) {
                    BitStream *bs = &ip->channel_streams[c];
                    size_t consumed = bs->bytes_read_so_far - (bs->byte_count - bs->byte_pos);
                    ip->checkpoints[c][chunk_idx].byte_offset = consumed;
                    ip->checkpoints[c][chunk_idx].saved_byte = bs->current_byte;
                    ip->checkpoints[c][chunk_idx].saved_bits_remaining = bs->bits_remaining;
                    ip->checkpoints[c][chunk_idx].valid = 1;
                }
            }

            int32_t sample = decode_next_sample(&ip->channel_streams[c], 
                                                &ip->channel_last_samples[c], 
                                                &ip->channel_current_k[c], 
                                                &ip->channel_samples_left_in_block[c],
                                                file_sample_idx,
                                                ip->header.sample_rate);

            float normalized = (float)sample * NORMALIZATION_FACTOR;
            float audio_sample = normalized / 2147483648.0f;

            size_t target_ring_idx = current_virtual_frame % buf_cap;
            ip->ring_buffer[c][target_ring_idx] = audio_sample;
        }

        pthread_mutex_lock(&ip->worker_mutexes[c]);
        ip->worker_status[c] = 0;
        pthread_cond_signal(&ip->worker_cond_done[c]);
        pthread_mutex_unlock(&ip->worker_mutexes[c]);
    }
    return NULL;
}

// ============================================================================
// MASTER THREAD (NÚCLEO ORGANIZADOR): COORDENA OS WORKERS EM CHUNKS
// ============================================================================
void *decode_thread_func(void *arg) {
    iacplay_desc *ip = (iacplay_desc*)arg;
    size_t future_target = ip->future_target_frames; 

    while (ip->running) {
        if (ip->is_paused) {
            THREAD_SLEEP_MS(10);
            continue;
        }

        pthread_mutex_lock(&ip->master_mutex);

        uint64_t r_cur = ip->playback_cursor;
        uint64_t w_cur = ip->write_cursor;
        uint64_t filled_frames = w_cur - r_cur;

        if (filled_frames + ip->chunk_frames <= future_target) {
            // 1. Disparar canais em paralelo
            for (int c = 0; c < ip->header.channels; c++) {
                pthread_mutex_lock(&ip->worker_mutexes[c]);
                ip->worker_target_frame[c] = w_cur;
                ip->worker_status[c] = 1; 
                pthread_cond_signal(&ip->worker_cond_start[c]);
                pthread_mutex_unlock(&ip->worker_mutexes[c]);
            }

            // 2. Barreira de sincronização
            for (int c = 0; c < ip->header.channels; c++) {
                pthread_mutex_lock(&ip->worker_mutexes[c]);
                while (ip->worker_status[c] == 1 && ip->running) {
                    pthread_cond_wait(&ip->worker_cond_done[c], &ip->worker_mutexes[c]);
                }
                pthread_mutex_unlock(&ip->worker_mutexes[c]);
            }

            if (!ip->running) {
                pthread_mutex_unlock(&ip->master_mutex);
                break;
            }

            // 3. Commit de avanço
            ip->write_cursor += ip->chunk_frames;
        }
        
        pthread_mutex_unlock(&ip->master_mutex);

        if (filled_frames + ip->chunk_frames > future_target) {
            THREAD_SLEEP_MS(10);
        } else {
            THREAD_SLEEP_MS(1);
        }
    }
    return NULL;
}

// ============================================================================
// OPERAÇÕES DE FLUXO DE TEMPO E SEEK
// ============================================================================
double calculate_time(iacplay_desc *ip) {
    uint64_t current_frame = ip->playback_cursor % ip->header.total_samples;
    return (double)current_frame / (double)ip->header.sample_rate;
}

double calculate_duration(iacplay_desc *ip) {
    return (double)ip->header.total_samples / (double)ip->header.sample_rate;
}

void update_timer(iacplay_desc *ip) {
    double current = calculate_time(ip);
    double duration = calculate_duration(ip);
    printf("\r%.2f / %.2f seg (Buffer: %04.1fs)", current, duration, (double)(ip->write_cursor - ip->playback_cursor) / ip->header.sample_rate);
    fflush(stdout);
}

void iacplay_seek_absolute(iacplay_desc *ip, double time_seconds) {
    if (time_seconds < 0) time_seconds = 0;
    uint64_t target_frame = (uint64_t)(time_seconds * ip->header.sample_rate);
    if (target_frame > ip->header.total_samples) {
        target_frame = ip->header.total_samples;
    }

    uint64_t w_cur = ip->write_cursor;
    uint64_t valid_low = (w_cur >= ip->ring_buffer_capacity) ? (w_cur - ip->ring_buffer_capacity) : 0;
    if (target_frame >= valid_low && target_frame < w_cur) {
        ip->playback_cursor = target_frame;
        return;
    }

    int originally_paused = ip->is_paused;
    ip->is_paused = 1; // Diz para o Organizador não pegar novos chunks na próxima iteração

    // O TRUNFO ANTI-BUG: Bloqueia a thread principal aqui até que o Organizador 
    // termine qualquer operação de decodificação paralela em andamento.
    pthread_mutex_lock(&ip->master_mutex);

    size_t chunk_idx = (size_t)(target_frame / ip->chunk_frames);
    uint64_t chunk_start_frame = (uint64_t)chunk_idx * ip->chunk_frames;

    // NOVO: salto direto via seek_table do arquivo — não depende mais de o player
    // já ter passado por aquele trecho antes (checkpoint runtime), funciona em
    // qualquer ponto do arquivo, mesmo logo após abrir.
    for (int c = 0; c < ip->header.channels; c++) {
        BitStream *bs = &ip->channel_streams[c];

        uint64_t cidx = chunk_idx;
        if (bs->seek_table_count > 0 && cidx >= bs->seek_table_count) {
            cidx = bs->seek_table_count - 1;
        }

        seek_bitstream_to_chunk(bs, bs->seek_table[cidx]);

        ip->channel_last_samples[c] = 0;
        ip->channel_current_k[c] = 0;
        ip->channel_samples_left_in_block[c] = 0;
        ip->channel_sample_counters[c] = 0;
    }

    // Loop de catch-up: agora só percorre o resíduo dentro do próprio chunk de 5s
    // (no máximo ~5s de áudio), em vez de potencialmente todo o arquivo desde o início.
    for (uint64_t i = chunk_start_frame; i < target_frame; i++) {
        for (int c = 0; c < ip->header.channels; c++) {
            decode_next_sample(&ip->channel_streams[c], 
                               &ip->channel_last_samples[c], 
                               &ip->channel_current_k[c], 
                               &ip->channel_samples_left_in_block[c],
                               i, 
                               ip->header.sample_rate);
        }
    }

    ip->playback_cursor = target_frame;
    ip->write_cursor = target_frame;
    ip->is_starved = 1;   
    ip->fade_phase = 0;

    // Libera o mutex para o organizador poder voltar ao trabalho
    pthread_mutex_unlock(&ip->master_mutex);
    ip->is_paused = originally_paused;
}

// ============================================================================
// CALLBACK MINIAUDIO
// ============================================================================
void audio_cb(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    iacplay_desc *ip = (iacplay_desc*)pDevice->pUserData;
    float *buffer = (float*)pOutput;
    int num_frames = (int)frameCount;
    int num_channels = (int)pDevice->playback.channels;

    if (ip->is_paused || !ip->running) {
        memset(buffer, 0, num_frames * num_channels * sizeof(float));
        return;
    }

    uint64_t resume_threshold = (uint64_t)(ip->header.sample_rate * RESUME_MARGIN_SEC);
    if (resume_threshold < 1) resume_threshold = 1;

    for (int f = 0; f < num_frames; f++) {
        uint64_t r_cur = ip->playback_cursor;
        uint64_t w_cur = ip->write_cursor;
        uint64_t filled = (w_cur > r_cur) ? (w_cur - r_cur) : 0;

        if (ip->is_starved) {
            if (filled >= resume_threshold) {
                ip->is_starved = 0;
                ip->fade_phase = 3;   
                ip->fade_counter = 0;
            } else {
                for (int c = 0; c < num_channels; c++) buffer[f * num_channels + c] = 0.0f;
                continue; 
            }
        }

        if (filled == 0) {
            ip->is_starved = 1;
            for (int c = 0; c < num_channels; c++) buffer[f * num_channels + c] = 0.0f;
            continue;
        }

        size_t target_idx = r_cur % ip->ring_buffer_capacity;
        for (int c = 0; c < num_channels; c++) {
            float s = ip->ring_buffer[c][target_idx];

            if (ip->fade_phase == 3) {
                float t = (float)ip->fade_counter / (float)DECLICK_FADE_FRAMES;
                if (t > 1.0f) t = 1.0f;
                s *= t; 
            }
            buffer[f * num_channels + c] = s;
        }

        if (ip->fade_phase == 3) {
            ip->fade_counter++;
            if (ip->fade_counter >= DECLICK_FADE_FRAMES) ip->fade_phase = 0;
        }

        ip->playback_cursor++;
    }
    update_timer(ip);
}

void toggle_pause(iacplay_desc *ip) {
    ip->is_paused = !ip->is_paused;
    printf(ip->is_paused ? "\nPAUSED\n" : "\nPLAYING\n");
}

// ============================================================================
// CARREGAMENTO DO ARQUIVO .IAC
// ============================================================================
iacplay_desc *iacplay_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    iacplay_desc *ip = (iacplay_desc*)calloc(1, sizeof(iacplay_desc));
    ip->file = f;
    strncpy(ip->file_path, path, sizeof(ip->file_path) - 1);

    char magic[4];
    fread(magic, 1, 4, f);
    if (memcmp(magic, IAC_MAGIC, 4) != 0) {
        printf("Error: Invalid IAC file\n");
        free(ip);
        fclose(f);
        return NULL;
    }

    uint32_t version;
    fread(&version, 4, 1, f);
    fread(&ip->header.sample_rate, 4, 1, f);
    fread(&ip->header.channels, 2, 1, f);
    fread(&ip->header.bits_per_sample, 2, 1, f);
    fread(&ip->header.total_samples, 8, 1, f);

    printf("IAC Info: %d Hz, %d channels, %d bits, %llu samples per channel\n",
           ip->header.sample_rate, ip->header.channels, 
           ip->header.bits_per_sample, (unsigned long long)ip->header.total_samples);

    fseek(f, 32, SEEK_CUR);

    uint64_t offsets[MAX_CHANNELS], sizes[MAX_CHANNELS];
    for (int i = 0; i < ip->header.channels; i++) {
        fread(&offsets[i], 8, 1, f);
        fread(&sizes[i], 8, 1, f);
        ip->channel_offsets[i] = offsets[i];
        ip->compressed_sizes[i] = sizes[i];
    }

    ip->ring_buffer_capacity = (size_t)(PAST_BUFFER_SECONDS + FUTURE_BUFFER_SECONDS) * ip->header.sample_rate;
    ip->future_target_frames = (size_t)FUTURE_BUFFER_SECONDS * ip->header.sample_rate;

    ip->chunk_frames = iac_chunk_frames(ip->header.sample_rate);
    ip->num_chunks = (ip->header.total_samples + ip->chunk_frames - 1) / ip->chunk_frames;
    if (ip->num_chunks == 0) ip->num_chunks = 1;

    printf("Allocating %.2f MB of Ring Buffer for multithreaded decompression...\n", 
           (double)(ip->ring_buffer_capacity * ip->header.channels * sizeof(float)) / (1024.0 * 1024.0));

    ip->playback_cursor = 0;
    ip->write_cursor = 0;
    ip->is_paused = 0;
    ip->running = 1;
    ip->is_starved = 0;
    ip->fade_phase = 0;
    ip->fade_counter = 0;

    for (int i = 0; i < ip->header.channels; i++) {
        ip->ring_buffer[i] = (float*)malloc(ip->ring_buffer_capacity * sizeof(float));
        if (!ip->ring_buffer[i]) {
            printf("Fatal error: Insufficient RAM for the Ring Buffer.\n");
            exit(1);
        }

        ip->channel_files[i] = fopen(path, "rb");
        fseek(ip->channel_files[i], ip->channel_offsets[i], SEEK_SET);

        init_bitstream(&ip->channel_streams[i], ip->channel_files[i], ip->channel_offsets[i], ip->compressed_sizes[i]);
        ip->channel_last_samples[i] = 0;
        ip->channel_current_k[i] = 0;
        ip->channel_samples_left_in_block[i] = 0;
        ip->channel_sample_counters[i] = 0;

        ip->checkpoints[i] = (ChunkCheckpoint*)calloc(ip->num_chunks, sizeof(ChunkCheckpoint));
        if (!ip->checkpoints[i]) {
            printf("Fatal error: Insufficient RAM for checkpoint table.\n");
            exit(1);
        }

        pthread_mutex_init(&ip->worker_mutexes[i], NULL);
        pthread_cond_init(&ip->worker_cond_start[i], NULL);
        pthread_cond_init(&ip->worker_cond_done[i], NULL);
        ip->worker_status[i] = 0;
        ip->worker_target_frame[i] = 0;

        ip->worker_args[i].ip = ip;
        ip->worker_args[i].channel_idx = i;

        if (pthread_create(&ip->worker_threads[i], NULL, channel_worker_func, &ip->worker_args[i]) != 0) {
            printf("Error starting channel worker thread %d.\n", i);
            exit(1);
        }
    }

    pthread_mutex_init(&ip->master_mutex, NULL);

    if (pthread_create(&ip->decode_thread, NULL, decode_thread_func, ip) != 0) {
        printf("Error starting background master orchestrator thread.\n");
        exit(1);
    }

    return ip;
}

void iacplay_close(iacplay_desc *ip) {
    if (!ip) return;
    ip->running = 0;

    for (int i = 0; i < ip->header.channels; i++) {
        pthread_mutex_lock(&ip->worker_mutexes[i]);
        pthread_cond_signal(&ip->worker_cond_start[i]);
        pthread_mutex_unlock(&ip->worker_mutexes[i]);
    }

    pthread_join(ip->decode_thread, NULL);
    pthread_mutex_destroy(&ip->master_mutex);

    for (int i = 0; i < ip->header.channels; i++) {
        pthread_join(ip->worker_threads[i], NULL);
        
        pthread_mutex_destroy(&ip->worker_mutexes[i]);
        pthread_cond_destroy(&ip->worker_cond_start[i]);
        pthread_cond_destroy(&ip->worker_cond_done[i]);

        if (ip->ring_buffer[i]) free(ip->ring_buffer[i]);
        if (ip->channel_files[i]) fclose(ip->channel_files[i]);
        if (ip->checkpoints[i]) free(ip->checkpoints[i]);
    }
    if (ip->file) fclose(ip->file);
    free(ip);
}

// ============================================================================
// MAIN (WASAPI EXCLUSIVE CORE)
// ============================================================================
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <file.iac>\n", argv[0]);
        return 1;
    }

    printf("\n=== IAC Player WASAPI Exclusive v0.1.0 ===\n");
    printf("Imprecision Audio Codec Player\n\n");

    iacplay_desc *ip = iacplay_open(argv[1]);
    if (!ip) {
        printf("Error opening file.\n");
        return 1;
    }

    ma_device_config deviceConfig;
    ma_device device;

    deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format    = ma_format_f32; 
    deviceConfig.playback.channels  = ip->header.channels;
    deviceConfig.sampleRate         = ip->header.sample_rate;
    deviceConfig.dataCallback       = audio_cb;
    deviceConfig.pUserData          = ip;
    deviceConfig.periodSizeInFrames = 1024;
    
    // ATIVAÇÃO DO MODO EXCLUSIVO
    deviceConfig.playback.shareMode = ma_share_mode_exclusive;

    if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS) {
        printf("Error: Unable to initialize device in WASAPI Exclusive mode.\n");
        printf("Check if %d Hz is natively supported by your hardware.\n", ip->header.sample_rate);
        iacplay_close(ip);
        return 1;
    }

    if (ma_device_start(&device) != MA_SUCCESS) {
        printf("Error starting audio playback on device.\n");
        ma_device_uninit(&device);
        iacplay_close(ip);
        return 1;
    }

    printf("\nPlaying...\n");
    printf("Press:\n");
    printf(" [space] to pause\n");
    printf(" [x] to go back 5s\n");
    printf(" [c] to go forward 5s\n");
    printf(" [q] to exit\n\n");

    int wants_to_quit = 0;
    while (!wants_to_quit) {
        char c = getch();
        switch (c) {
            case ' ':
                toggle_pause(ip);
                break;
            case 'x': {
                double current = calculate_time(ip);
                iacplay_seek_absolute(ip, current - 5.0);
                break;
            }
            case 'c': {
                double current = calculate_time(ip);
                iacplay_seek_absolute(ip, current + 5.0);
                break;
            }
            case 'q':
                wants_to_quit = 1;
                break;
        }
        update_timer(ip);
    }

    ma_device_uninit(&device);
    iacplay_close(ip);
    printf("\n\nBye bye\n");
    return 0;
}
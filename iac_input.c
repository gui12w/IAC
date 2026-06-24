/*
IAC Encoder v1.4 - Imprecision Audio Codec (Seek Table Edition)
- Redução de 110 dB
- Conversão para 14 bits por sample
- Formato binário compacto com Seek Table por canal para busca instantânea O(1)

Compilar:
    zig cc iac_input_ram.c -std=gnu99 -pthread -O3 -lm -o iac_encode.exe
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define popen  _popen
#define pclose _pclose
#endif

#define IAC_MAGIC "IAC1"
#define IAC_VERSION 1
#define DB_REDUCTION 110.0
#define MAX_CHANNELS 8
#define BITS_PER_SAMPLE 15
#define MAX_15BIT_VALUE 16383   // 2^14 - 1 (signed 15-bit)
#define MIN_15BIT_VALUE -16384  // -2^14
#define BLOCK_SIZE 1024         // Tamanho ideal de bloco para análise de entropia

// ============================================================================
// CONTROLE DE PROGRESSO MULTITHREAD
// ============================================================================
pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;
double global_progress[MAX_CHANNELS] = {0};
int global_total_channels = 0;

typedef struct {
    int32_t *samples;
    size_t count;
    size_t capacity;
} Channel;

typedef struct {
    uint8_t *data;
    size_t byte_count;
    size_t capacity;
    uint8_t current_byte;
    int bits_in_current_byte;
} BitBuffer;

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint64_t total_samples;
} IACHeader;

typedef struct {
    Channel *channel;
    BitBuffer *output;
    int channel_id;
    uint32_t sample_rate;
    // --- NOVO: Ponteiros para retornar a tabela gerada pela thread ---
    uint64_t *seek_table;
    size_t seek_table_count;
    size_t seek_table_capacity;
} ThreadData;

// ============================================================================
// BIT BUFFER
// ============================================================================

void init_bit_buffer(BitBuffer *buf) {
    buf->capacity = 1024 * 1024;
    buf->byte_count = 0;
    buf->data = (uint8_t*)malloc(buf->capacity);
    buf->current_byte = 0;
    buf->bits_in_current_byte = 0;
    
    if (!buf->data) {
        fprintf(stderr, "Error allocating bit buffer.\n");
        exit(1);
    }
}

void ensure_bit_capacity(BitBuffer *buf, size_t required_bytes) {
    if (buf->byte_count + required_bytes >= buf->capacity) {
        size_t new_cap = buf->capacity * 2;
        while (buf->byte_count + required_bytes >= new_cap) {
            new_cap *= 2;
        }
        uint8_t *new_ptr = (uint8_t*)realloc(buf->data, new_cap);
        if (!new_ptr) {
            fprintf(stderr, "Error reallocating bit buffer.\n");
            exit(1);
        }
        buf->data = new_ptr;
        buf->capacity = new_cap;
    }
}

void write_bits(BitBuffer *buf, int32_t value, int num_bits) {
    for (int i = num_bits - 1; i >= 0; i--) {
        int bit = (value >> i) & 1;
        buf->current_byte = (buf->current_byte << 1) | bit;
        buf->bits_in_current_byte++;
        
        if (buf->bits_in_current_byte == 8) {
            ensure_bit_capacity(buf, 1);
            buf->data[buf->byte_count++] = buf->current_byte;
            buf->current_byte = 0;
            buf->bits_in_current_byte = 0;
        }
    }
}

// --- NOVO: Força o alinhamento de byte para permitir saltos via fseek ---
void align_bit_buffer_to_byte(BitBuffer *buf) {
    if (buf->bits_in_current_byte > 0) {
        buf->current_byte <<= (8 - buf->bits_in_current_byte);
        ensure_bit_capacity(buf, 1);
        buf->data[buf->byte_count++] = buf->current_byte;
        buf->current_byte = 0;
        buf->bits_in_current_byte = 0;
    }
}

void write_rice(BitBuffer *buf, int32_t value, int k) {
    uint32_t u_val = (uint32_t)((value << 1) ^ (value >> 31));
    uint32_t q = u_val >> k;          
    uint32_t r = u_val & ((1 << k) - 1); 

    for (uint32_t i = 0; i < q; i++) {
        write_bits(buf, 1, 1);
    }
    write_bits(buf, 0, 1);
    write_bits(buf, r, k);
}

void finalize_bit_buffer(BitBuffer *buf) {
    align_bit_buffer_to_byte(buf);
}

// ============================================================================
// CANAL
// ============================================================================

void init_channel(Channel *ch, size_t initial_capacity) {
    ch->capacity = initial_capacity;
    ch->count = 0;
    ch->samples = (int32_t*)malloc(ch->capacity * sizeof(int32_t));
    if (!ch->samples) {
        fprintf(stderr, "Error allocating channel\n");
        exit(1);
    }
}

void ensure_channel_capacity(Channel *ch, size_t required) {
    if (ch->count + required >= ch->capacity) {
        size_t new_cap = ch->capacity * 2;
        while (ch->count + required >= new_cap) {
            new_cap *= 2;
        }
        int32_t *new_ptr = (int32_t*)realloc(ch->samples, new_cap * sizeof(int32_t));
        if (!new_ptr) {
            fprintf(stderr, "Error reallocating channel\n");
            exit(1);
        }
        ch->samples = new_ptr;
        ch->capacity = new_cap;
    }
}

// ============================================================================
// LEITURA DE ÁUDIO
// ============================================================================

FILE *convert_to_wav_pipe(const char *audio_file) {
    const char *ext = strrchr(audio_file, '.');
    const char *formato = ext ? ext + 1 : "áudio";

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -loglevel error -i \"%s\" -f wav -acodec pcm_s32le -rf64 never -",
        audio_file);

    printf("Converting %s to WAV 32-bit via pipe (RAM, No Write to Disk)...\n", formato);

#ifdef _WIN32
    FILE *pipe = popen(cmd, "rb");
#else
    FILE *pipe = popen(cmd, "r");
#endif
    if (!pipe) {
        fprintf(stderr, "Error opening FFmpeg pipe\n");
        return NULL;
    }

#ifdef _WIN32
    _setmode(_fileno(pipe), _O_BINARY);
#endif

    return pipe;
}

static int skip_bytes(FILE *f, uint32_t n) {
    uint8_t buf[4096];
    while (n > 0) {
        size_t to_read = n < sizeof(buf) ? n : sizeof(buf);
        size_t got = fread(buf, 1, to_read, f);
        if (got != to_read) return 0;
        n -= (uint32_t)got;
    }
    return 1;
}

int ler_wav_multicanal(FILE *f, Channel channels[], IACHeader *header) {
    if (!f) return 0;

    uint8_t riff_hdr[12];
    if (fread(riff_hdr, 1, 12, f) != 12) return 0;
    if (memcmp(riff_hdr, "RIFF", 4) != 0 || memcmp(riff_hdr + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "Error: not a valid WAV stream\n");
        return 0;
    }

    uint8_t chunk_id[4];
    uint32_t chunk_size;
    int found_fmt = 0, found_data = 0;
    uint32_t bits = 0;

    while (fread(chunk_id, 1, 4, f) == 4) {
        if (fread(&chunk_size, 4, 1, f) != 1) break;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            uint32_t to_read = chunk_size < 16 ? chunk_size : 16;
            if (fread(fmt, 1, to_read, f) != to_read) return 0;
            if (chunk_size > 16 && !skip_bytes(f, chunk_size - 16)) return 0;
            if (chunk_size % 2 == 1 && !skip_bytes(f, 1)) return 0; 

            memcpy(&header->channels, fmt + 2, 2);
            memcpy(&header->sample_rate, fmt + 4, 4);
            memcpy(&bits, fmt + 14, 2);
            found_fmt = 1;

            printf("WAV Info: %d Hz, %d channels, %d bits\n",
                   header->sample_rate, header->channels, bits);
        }
        else if (memcmp(chunk_id, "data", 4) == 0) {
            printf("Chunk 'data' found (%u bytes)\n", chunk_size);
            found_data = 1;
            break; 
        }
        else {
            if (!skip_bytes(f, chunk_size)) break;
            if (chunk_size % 2 == 1 && !skip_bytes(f, 1)) break; 
        }
    }

    if (!found_fmt || !found_data) {
        fprintf(stderr, "Error: missing 'fmt ' or 'data' chunk in WAV stream\n");
        return 0;
    }

    for (int i = 0; i < header->channels; i++) {
        init_channel(&channels[i], 512 * 1024);
    }

    float fator_f = (float)pow(10.0, -DB_REDUCTION / 20.0);

    if (bits == 32) {
        int32_t buffer[8 * MAX_CHANNELS];
        size_t num_read;
        
        while ((num_read = fread(buffer, sizeof(int32_t), 8 * header->channels, f)) > 0) {
            for (size_t i = 0; i < num_read; i++) {
                int ch = i % header->channels;
                int32_t s32 = buffer[i];
                int32_t reduced = (int32_t)(s32 * fator_f);
                
                if (reduced > MAX_15BIT_VALUE) reduced = MAX_15BIT_VALUE;
                if (reduced < MIN_15BIT_VALUE) reduced = MIN_15BIT_VALUE;
                
                ensure_channel_capacity(&channels[ch], 1);
                channels[ch].samples[channels[ch].count++] = reduced;
            }
        }
    }
    else if (bits == 16) {
        int32_t buffer[8 * MAX_CHANNELS];
        size_t num_read;
        
        while ((num_read = fread(buffer, sizeof(int32_t), 8 * header->channels, f)) > 0) {
            for (size_t i = 0; i < num_read; i++) {
                int ch = i % header->channels;
                int32_t s16 = buffer[i];
                int32_t s32 = ((int32_t)s16) << 16;
                int32_t reduced = (int32_t)(s32 * fator_f);
                
                if (reduced > MAX_15BIT_VALUE) reduced = MAX_15BIT_VALUE;
                if (reduced < MIN_15BIT_VALUE) reduced = MIN_15BIT_VALUE;
                
                ensure_channel_capacity(&channels[ch], 1);
                channels[ch].samples[channels[ch].count++] = reduced;
            }
        }
    } else {
        fprintf(stderr, "Error: Only 16-bit and 32-bit supported. File is %d-bit.\n", bits);
        return 0;
    }

    header->total_samples = channels[0].count;
    for (int i = 0; i < header->channels; i++) {
        channels[i].samples = (int32_t*)realloc(channels[i].samples, channels[i].count * sizeof(int32_t));
        channels[i].capacity = channels[i].count;
    }
    printf("Reading completed: %zu samples per channel\n", channels[0].count);
    return 1;
}

// ============================================================================
// COMPRESSÃO 14-BIT
// ============================================================================

int achar_melhor_k(int32_t *deltas, size_t tamanho) {
    int melhor_k = 0;
    size_t menor_quantidade_bits = (size_t)-1; 

    for (int test_k = 0; test_k < 16; test_k++) {
        size_t bits_totais = 0;
        
        for (size_t i = 0; i < tamanho; i++) {
            uint32_t u_val = (uint32_t)((deltas[i] << 1) ^ (deltas[i] >> 31));
            uint32_t q = u_val >> test_k;
            bits_totais += (q + 1) + test_k; 
        }

        if (bits_totais < menor_quantidade_bits) {
            menor_quantidade_bits = bits_totais;
            melhor_k = test_k;
        }
    }
    return melhor_k;
}

void *compactar_canal_14bit_thread(void *arg) {
    ThreadData *td = (ThreadData*)arg;
    Channel *ch = td->channel;
    BitBuffer *out = td->output;
    
    init_bit_buffer(out);
    
    if (ch->count == 0) {
        td->seek_table = NULL;
        td->seek_table_count = 0;
        finalize_bit_buffer(out);
        return NULL;
    }

    td->seek_table_capacity = (ch->count / (5 * td->sample_rate)) + 10;
    td->seek_table = (uint64_t*)malloc(td->seek_table_capacity * sizeof(uint64_t));
    td->seek_table_count = 0;

    // --- OTIMIZAÇÃO CRÍTICA: Buffer local pequeno em vez de malloc do canal inteiro ---
    int32_t deltas[BLOCK_SIZE]; 

    size_t blocks_per_chunk = (5 * td->sample_rate) / BLOCK_SIZE;
    if (blocks_per_chunk == 0) blocks_per_chunk = 1;

    int32_t last_sample = 0;
    size_t total_samples = ch->count;
    size_t i = 0;
    
    while (i < total_samples) {
        size_t b_idx = i / BLOCK_SIZE;
        size_t tamanho_bloco = BLOCK_SIZE;
        if (i + tamanho_bloco > total_samples) {
            tamanho_bloco = total_samples - i;
        }
        
        // Calcula os deltas apenas para o bloco atual de 1024 samples
        for (size_t j = 0; j < tamanho_bloco; j++) {
            if (j == 0 && (b_idx % blocks_per_chunk == 0)) {
                deltas[j] = 0; 
                last_sample = ch->samples[i + j];
            } else {
                deltas[j] = ch->samples[i + j] - last_sample;
                last_sample = ch->samples[i + j];
            }
        }
        
        int melhor_k;
        if (b_idx % blocks_per_chunk == 0 && tamanho_bloco > 1) {
            melhor_k = achar_melhor_k(&deltas[1], tamanho_bloco - 1);
        } else {
            melhor_k = achar_melhor_k(&deltas[0], tamanho_bloco);
        }
        
        if (b_idx % blocks_per_chunk == 0) {
            align_bit_buffer_to_byte(out);

            if (td->seek_table_count >= td->seek_table_capacity) {
                td->seek_table_capacity *= 2;
                td->seek_table = (uint64_t*)realloc(td->seek_table, td->seek_table_capacity * sizeof(uint64_t));
            }
            td->seek_table[td->seek_table_count++] = out->byte_count;
        }

        write_bits(out, melhor_k, 4);
        
        for (size_t j = 0; j < tamanho_bloco; j++) {
            if (j == 0 && (b_idx % blocks_per_chunk == 0)) {
                write_bits(out, ch->samples[i + j], BITS_PER_SAMPLE);
                continue;
            }
            write_rice(out, deltas[j], melhor_k); // <-- Usa o índice local j do bloco
        }
        
        i += tamanho_bloco;

        global_progress[td->channel_id] = ((double)i / total_samples) * 100.0;

        if ((i / BLOCK_SIZE) % 100 == 0 || i >= total_samples) {
            pthread_mutex_lock(&print_mutex); 
            printf("\rProgress: ");
            for (int c = 0; c < global_total_channels; c++) {
                printf("Ch%d: %5.1f%%", c, global_progress[c]);
                if (c < global_total_channels - 1) printf(" | ");
            }
            fflush(stdout);
            pthread_mutex_unlock(&print_mutex); 
        }
    }
    
    // O free(deltas) antigo sumiu porque agora a memória é local e limpa automaticamente!
    finalize_bit_buffer(out);
    return NULL;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <input> <output.iac>\n", argv[0]);
        return 1;
    }

    const char *input = argv[1];
    const char *output = argv[2];

    printf("\n=== IAC Encoder v0.1.0 ===\n");
    printf("Imprecision Audio Codec - 14 bits per sample\n\n");

    FILE *input_stream = NULL;
    int is_pipe = 0;

    const char *ext = strrchr(input, '.');
    if (ext && strcmp(ext, ".wav") == 0) {
        input_stream = fopen(input, "rb");
        if (!input_stream) {
            perror("Error opening WAV");
            return 1;
        }
    } else {
        input_stream = convert_to_wav_pipe(input);
        if (!input_stream) return 1;
        is_pipe = 1;
    }

    Channel channels[MAX_CHANNELS];
    IACHeader header = {0};
    
    int ok = ler_wav_multicanal(input_stream, channels, &header);

    if (is_pipe) pclose(input_stream);
    else fclose(input_stream);

    if (!ok) return 1;

    printf("\nCompressing %d channels...\n", header.channels);
    global_total_channels = header.channels;
    
    pthread_t threads[MAX_CHANNELS];
    ThreadData thread_data[MAX_CHANNELS];
    BitBuffer outputs[MAX_CHANNELS];
    
    for (int i = 0; i < header.channels; i++) {
        thread_data[i].channel = &channels[i];
        thread_data[i].output = &outputs[i];
        thread_data[i].channel_id = i;
        thread_data[i].sample_rate = header.sample_rate;
        
        pthread_create(&threads[i], NULL, compactar_canal_14bit_thread, &thread_data[i]);
    }
    
    for (int i = 0; i < header.channels; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("\n\nSaving IAC file...\n");
    FILE *fout = fopen(output, "wb");
    if (!fout) {
        perror("Error creating file");
        return 1;
    }

    // Header
    fwrite(IAC_MAGIC, 1, 4, fout);
    uint32_t version = IAC_VERSION;
    fwrite(&version, 4, 1, fout);
    fwrite(&header.sample_rate, 4, 1, fout);
    fwrite(&header.channels, 2, 1, fout);
    
    header.bits_per_sample = BITS_PER_SAMPLE;
    fwrite(&header.bits_per_sample, 2, 1, fout);
    fwrite(&header.total_samples, 8, 1, fout);
    
    // Reservado (32 bytes)
    uint8_t reserved[32] = {0};
    fwrite(reserved, 1, 32, fout);

    // Índice de canais (Salva placeholders)
    long index_pos = ftell(fout);
    uint64_t offsets[MAX_CHANNELS] = {0};
    uint64_t sizes[MAX_CHANNELS] = {0};
    
    for (int i = 0; i < header.channels; i++) {
        fwrite(&offsets[i], 8, 1, fout);
        fwrite(&sizes[i], 8, 1, fout);
    }

    // --- MODIFICADO: ESCREVE A SEEK TABLE JUNTO COM OS DADOS DE CADA CANAL ---
    for (int i = 0; i < header.channels; i++) {
        offsets[i] = ftell(fout); // Ponto absoluto do canal no arquivo

        uint64_t num_chunks = thread_data[i].seek_table_count;
        
        // 1. Escreve a quantidade de chunks da tabela (8 bytes)
        fwrite(&num_chunks, 8, 1, fout);

        // 2. Transforma os offsets relativos do BitBuffer em offsets absolutos do arquivo final
        // O bitstream compactado começará logo após o 'num_chunks' (8B) + o tamanho do array de offsets (num_chunks * 8B)
        uint64_t bitstream_start_file_pos = offsets[i] + 8 + (num_chunks * 8);
        for (size_t c = 0; c < num_chunks; c++) {
            thread_data[i].seek_table[c] += bitstream_start_file_pos;
        }

        // 3. Escreve o array completo da Seek Table
        fwrite(thread_data[i].seek_table, 8, num_chunks, fout);

        // 4. Escreve o BitBuffer de dados brutos compactados
        fwrite(outputs[i].data, 1, outputs[i].byte_count, fout);

        // Calcula o tamanho final completo ocupado pelo bloco deste canal
        sizes[i] = ftell(fout) - offsets[i];
    }

    // Atualiza o índice global de offsets no início do arquivo
    fseek(fout, index_pos, SEEK_SET);
    for (int i = 0; i < header.channels; i++) {
        fwrite(&offsets[i], 8, 1, fout);
        fwrite(&sizes[i], 8, 1, fout);
    }

    fclose(fout);

    // Estatísticas
    size_t total_compressed = 0;
    for (int i = 0; i < header.channels; i++) {
        total_compressed += outputs[i].byte_count;
        printf("  Channel %d: %zu bytes (Pure compressed data)\n", i, outputs[i].byte_count);
    }
    
    size_t original_size = header.total_samples * header.channels * 4; 
    float compression_ratio = (float)original_size / (float)total_compressed;
    
    printf("\nOriginal size: %zu bytes (32-bit)\n", original_size);
    printf("Compressed data size: %zu bytes (14-bit)\n", total_compressed);
    printf("Compression ratio: %.2fx\n", compression_ratio);

    // Cleanup
    for (int i = 0; i < header.channels; i++) {
        free(channels[i].samples);
        free(outputs[i].data);
        if (thread_data[i].seek_table) {
            free(thread_data[i].seek_table); // Libera a seek table alocada na thread
        }
    }

    printf("\nEncoding complete!\n");
    return 0;
}
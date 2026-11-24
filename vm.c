/*
 * vm.c - Gerenciador de Memoria Virtual
 *
 * Baseado no projeto "Designing a Virtual Memory Manager"
 * (Silberschatz et al., Operating System Concepts, 10a ed., P-51),
 * com as seguintes modificacoes:
 *   - memoria fisica com apenas 128 frames (paginas de 256 bytes);
 *   - substituicao de pagina: fifo ou lru;
 *   - substituicao da TLB (16 entradas): fifo ou lru;
 *   - busca na TLB feita por uma thread para cada entrada;
 *   - saida gravada em correct.txt e impressa na saida padrao (stdout).
 *
 * Uso: ./vm <arquivo_de_enderecos> <fifo|lru> <fifo|lru>
 *        argv[2] = algoritmo de substituicao de pagina
 *        argv[3] = algoritmo de substituicao da TLB
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------------- */
/* Parametros do sistema                                                  */
/* ---------------------------------------------------------------------- */
#define PAGE_SIZE        256     /* bytes por pagina / frame             */
#define NUM_PAGES        256     /* entradas da tabela de paginas        */
#define NUM_FRAMES       128     /* frames na memoria fisica             */
#define TLB_SIZE         16      /* entradas da TLB                      */
#define BACKING_SIZE     (NUM_PAGES * PAGE_SIZE)   /* 65536 bytes        */

#define BACKING_STORE    "BACKING_STORE.bin"
#define OUTPUT_FILE      "correct.txt"

#define OFFSET_MASK      0x00FF
#define ADDRESS_MASK     0xFFFF

typedef enum { ALG_FIFO, ALG_LRU } algorithm_t;

/* ---------------------------------------------------------------------- */
/* Estruturas                                                             */
/* ---------------------------------------------------------------------- */
typedef struct {
    int      valid;
    int      frame;
    uint64_t last_use;     /* instante do ultimo acesso (LRU de pagina)  */
} page_entry_t;

typedef struct {
    int      valid;
    int      page;
    int      frame;
    uint64_t last_use;     /* instante do ultimo acesso (LRU da TLB)     */
} tlb_entry_t;

/* Memoria fisica e estruturas de traducao */
static signed char  physical_memory[NUM_FRAMES][PAGE_SIZE];
static page_entry_t page_table[NUM_PAGES];
static int          frame_owner[NUM_FRAMES];   /* pagina em cada frame, -1 = livre */
static tlb_entry_t  tlb[TLB_SIZE];

/* Controle dos algoritmos */
static int      frames_used   = 0;   /* frames ja ocupados (preenche 0..127)   */
static int      fifo_frame    = 0;   /* proximo frame vitima no FIFO de pagina */
static int      tlb_fifo_next = 0;   /* proxima entrada vitima no FIFO da TLB  */
static uint64_t clock_tick    = 0;   /* relogio logico para o LRU              */

/* Estatisticas */
static int translated = 0;
static int page_faults = 0;
static int tlb_hits = 0;

/* ---------------------------------------------------------------------- */
/* Busca paralela na TLB: uma thread por entrada                          */
/* ---------------------------------------------------------------------- */
typedef struct {
    int index;     /* entrada da TLB que esta thread verifica */
    int page;      /* pagina procurada                        */
} tlb_search_arg_t;

static pthread_mutex_t tlb_result_lock = PTHREAD_MUTEX_INITIALIZER;
static int tlb_found_index;   /* -1 = nao encontrado */

static void *tlb_search_worker(void *arg)
{
    tlb_search_arg_t *a = (tlb_search_arg_t *)arg;
    const tlb_entry_t *e = &tlb[a->index];

    if (e->valid && e->page == a->page) {
        pthread_mutex_lock(&tlb_result_lock);
        tlb_found_index = a->index;
        pthread_mutex_unlock(&tlb_result_lock);
    }
    return NULL;
}

/* Retorna o indice da TLB que contem a pagina, ou -1 (TLB miss). */
static int tlb_lookup(int page)
{
    pthread_t        threads[TLB_SIZE];
    tlb_search_arg_t args[TLB_SIZE];
    int              created[TLB_SIZE];
    int              i;

    tlb_found_index = -1;

    for (i = 0; i < TLB_SIZE; i++) {
        args[i].index = i;
        args[i].page  = page;
        created[i] = (pthread_create(&threads[i], NULL, tlb_search_worker, &args[i]) == 0);
        if (!created[i]) {
            /* Falha ao criar a thread: verifica a entrada diretamente,
               para que a traducao continue correta. */
            tlb_search_worker(&args[i]);
        }
    }
    for (i = 0; i < TLB_SIZE; i++) {
        if (created[i])
            pthread_join(threads[i], NULL);
    }
    return tlb_found_index;
}

/* Escolhe a entrada da TLB que recebera um novo mapeamento. */
static int tlb_choose_victim(algorithm_t alg)
{
    int i, victim;

    if (alg == ALG_FIFO) {
        /* FIFO: fila circular, a entrada mais antiga sai primeiro. */
        victim = tlb_fifo_next;
        tlb_fifo_next = (tlb_fifo_next + 1) % TLB_SIZE;
        return victim;
    }

    /* LRU: usa uma entrada livre, se houver; senao, a menos usada recentemente. */
    for (i = 0; i < TLB_SIZE; i++)
        if (!tlb[i].valid)
            return i;

    victim = 0;
    for (i = 1; i < TLB_SIZE; i++)
        if (tlb[i].last_use < tlb[victim].last_use)
            victim = i;
    return victim;
}

static int tlb_insert(int page, int frame, algorithm_t alg)
{
    int idx = tlb_choose_victim(alg);
    tlb[idx].valid    = 1;
    tlb[idx].page     = page;
    tlb[idx].frame    = frame;
    tlb[idx].last_use = clock_tick;
    return idx;
}

/* Remove da TLB o mapeamento de uma pagina que saiu da memoria. */
static void tlb_invalidate(int page)
{
    int i;
    for (i = 0; i < TLB_SIZE; i++)
        if (tlb[i].valid && tlb[i].page == page)
            tlb[i].valid = 0;
}

/* ---------------------------------------------------------------------- */
/* Substituicao de pagina                                                 */
/* ---------------------------------------------------------------------- */
static int choose_frame(algorithm_t alg)
{
    int f, victim;

    /* Enquanto houver frames livres, preenche de 0 a 127. */
    if (frames_used < NUM_FRAMES)
        return frames_used++;

    if (alg == ALG_FIFO) {
        /* Como os frames sao preenchidos em ordem, a pagina mais antiga
           esta sempre no proximo frame da fila circular. */
        victim = fifo_frame;
        fifo_frame = (fifo_frame + 1) % NUM_FRAMES;
    } else {
        /* LRU: frame cuja pagina foi acessada ha mais tempo. */
        victim = 0;
        for (f = 1; f < NUM_FRAMES; f++)
            if (page_table[frame_owner[f]].last_use <
                page_table[frame_owner[victim]].last_use)
                victim = f;
    }

    /* Desaloca a pagina vitima. */
    page_table[frame_owner[victim]].valid = 0;
    tlb_invalidate(frame_owner[victim]);
    frame_owner[victim] = -1;
    return victim;
}

/* Trata uma falta de pagina: carrega a pagina do backing store. */
static int handle_page_fault(int page, FILE *backing, algorithm_t alg)
{
    int frame = choose_frame(alg);

    if (fseek(backing, (long)page * PAGE_SIZE, SEEK_SET) != 0 ||
        fread(physical_memory[frame], 1, PAGE_SIZE, backing) != PAGE_SIZE) {
        fprintf(stderr, "Erro: falha ao ler a pagina %d de %s.\n", page, BACKING_STORE);
        return -1;
    }

    page_table[page].valid = 1;
    page_table[page].frame = frame;
    frame_owner[frame] = page;
    page_faults++;
    return frame;
}

/* ---------------------------------------------------------------------- */
/* Validacao de entradas                                                  */
/* ---------------------------------------------------------------------- */
static int parse_algorithm(const char *s, algorithm_t *out)
{
    if (strcmp(s, "fifo") == 0) { *out = ALG_FIFO; return 0; }
    if (strcmp(s, "lru")  == 0) { *out = ALG_LRU;  return 0; }
    return -1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Uso: %s <arquivo_de_enderecos> <fifo|lru> <fifo|lru>\n"
            "  2o argumento: algoritmo de substituicao de pagina (fifo ou lru)\n"
            "  3o argumento: algoritmo de substituicao da TLB (fifo ou lru)\n"
            "Exemplo: %s addresses.txt lru fifo\n",
            prog, prog);
}

static int is_blank(const char *s)
{
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n')
            return 0;
    return 1;
}

/*
 * Le e valida todo o arquivo de enderecos antes de iniciar a simulacao.
 * Formato aceito: um inteiro decimal nao negativo por linha, no intervalo
 * 0..65535 (enderecos logicos de 16 bits). Espacos em volta do numero e
 * linhas em branco no final do arquivo sao tolerados.
 */
static int load_addresses(const char *path, int **out, int *count)
{
    FILE  *fp;
    char   line[256];
    int   *vec = NULL, n = 0, cap = 0;
    int    lineno = 0, seen_blank = 0;
    struct stat st;

    if (stat(path, &st) != 0) {
        fprintf(stderr, "Erro: arquivo de enderecos '%s' nao encontrado: %s\n",
                path, strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "Erro: '%s' nao e um arquivo regular.\n", path);
        return -1;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "Erro: nao foi possivel abrir o arquivo de enderecos '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof line, fp) != NULL) {
        char *p, *end;
        long  v;

        lineno++;
        if (strchr(line, '\n') == NULL && !feof(fp)) {
            fprintf(stderr, "Erro: linha %d de '%s' e longa demais.\n", lineno, path);
            goto fail;
        }
        if (is_blank(line)) { seen_blank = 1; continue; }
        if (seen_blank) {
            fprintf(stderr, "Erro: linha em branco no meio de '%s' (antes da linha %d).\n",
                    path, lineno);
            goto fail;
        }

        p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p < '0' || *p > '9') {
            fprintf(stderr, "Erro: formato invalido na linha %d de '%s': esperado um "
                    "inteiro nao negativo.\n", lineno, path);
            goto fail;
        }
        errno = 0;
        v = strtol(p, &end, 10);
        if (!is_blank(end)) {
            fprintf(stderr, "Erro: formato invalido na linha %d de '%s': esperado apenas "
                    "um inteiro por linha.\n", lineno, path);
            goto fail;
        }
        if (errno == ERANGE || v < 0 || v > ADDRESS_MASK) {
            fprintf(stderr, "Erro: endereco fora do intervalo 0..65535 na linha %d de '%s'.\n",
                    lineno, path);
            goto fail;
        }

        if (n == cap) {
            int *tmp;
            cap = cap ? cap * 2 : 1024;
            tmp = realloc(vec, (size_t)cap * sizeof *vec);
            if (tmp == NULL) {
                fprintf(stderr, "Erro: memoria insuficiente.\n");
                goto fail;
            }
            vec = tmp;
        }
        vec[n++] = (int)v;
    }

    if (ferror(fp)) {
        fprintf(stderr, "Erro: falha de leitura em '%s'.\n", path);
        goto fail;
    }
    if (n == 0) {
        fprintf(stderr, "Erro: o arquivo de enderecos '%s' esta vazio.\n", path);
        goto fail;
    }

    fclose(fp);
    *out = vec;
    *count = n;
    return 0;

fail:
    fclose(fp);
    free(vec);
    return -1;
}

static FILE *open_backing_store(void)
{
    FILE *fp = fopen(BACKING_STORE, "rb");
    long  size;

    if (fp == NULL) {
        fprintf(stderr, "Erro: nao foi possivel abrir '%s': %s\n",
                BACKING_STORE, strerror(errno));
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) != BACKING_SIZE) {
        fprintf(stderr, "Erro: '%s' deve ter exatamente %d bytes.\n",
                BACKING_STORE, BACKING_SIZE);
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    return fp;
}

/* Escreve a mesma linha no arquivo de saida e na saida padrao. */
static void emit(FILE *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------------- */
/* Programa principal                                                     */
/* ---------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    algorithm_t page_alg, tlb_alg;
    int        *addresses = NULL;
    int         count = 0, i;
    FILE       *backing, *out;

    /* (1) numero e tipo dos argumentos */
    if (argc != 4) {
        fprintf(stderr, "Erro: numero de argumentos invalido (esperado 3, recebido %d).\n",
                argc - 1);
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (parse_algorithm(argv[2], &page_alg) != 0) {
        fprintf(stderr, "Erro: algoritmo de substituicao de pagina invalido: '%s'.\n", argv[2]);
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (parse_algorithm(argv[3], &tlb_alg) != 0) {
        fprintf(stderr, "Erro: algoritmo de substituicao da TLB invalido: '%s'.\n", argv[3]);
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* (2) arquivo de enderecos: existencia, abertura e formato */
    if (load_addresses(argv[1], &addresses, &count) != 0)
        return EXIT_FAILURE;

    backing = open_backing_store();
    if (backing == NULL) {
        free(addresses);
        return EXIT_FAILURE;
    }

    out = fopen(OUTPUT_FILE, "w");
    if (out == NULL) {
        fprintf(stderr, "Erro: nao foi possivel criar '%s': %s\n", OUTPUT_FILE, strerror(errno));
        fclose(backing);
        free(addresses);
        return EXIT_FAILURE;
    }

    for (i = 0; i < NUM_FRAMES; i++)
        frame_owner[i] = -1;

    for (i = 0; i < count; i++) {
        int logical = addresses[i] & ADDRESS_MASK;
        int page    = (logical >> 8) & 0xFF;
        int offset  = logical & OFFSET_MASK;
        int frame, tlb_index;

        clock_tick++;

        tlb_index = tlb_lookup(page);
        if (tlb_index >= 0) {
            /* TLB hit */
            tlb_hits++;
            frame = tlb[tlb_index].frame;
            tlb[tlb_index].last_use = clock_tick;
        } else {
            /* TLB miss: consulta a tabela de paginas */
            if (page_table[page].valid) {
                frame = page_table[page].frame;
            } else {
                frame = handle_page_fault(page, backing, page_alg);
                if (frame < 0) {
                    fclose(out);
                    fclose(backing);
                    free(addresses);
                    return EXIT_FAILURE;
                }
            }
            tlb_index = tlb_insert(page, frame, tlb_alg);
        }
        page_table[page].last_use = clock_tick;

        emit(out, "Virtual address: %d TLB: %d Physical address: %d Value: %d\n",
                logical, tlb_index, frame * PAGE_SIZE + offset,
                (int)physical_memory[frame][offset]);
        translated++;
    }

    emit(out, "Number of Translated Addresses = %d\n", translated);
    emit(out, "Page Faults = %d\n", page_faults);
    emit(out, "Page Fault Rate = %.3f\n", (double)page_faults / translated);
    emit(out, "TLB Hits = %d\n", tlb_hits);
    emit(out, "TLB Hit Rate = %.3f\n", (double)tlb_hits / translated);

    fflush(stdout);
    if (fclose(out) != 0) {
        fprintf(stderr, "Erro: falha ao gravar '%s'.\n", OUTPUT_FILE);
        fclose(backing);
        free(addresses);
        return EXIT_FAILURE;
    }
    fclose(backing);
    free(addresses);
    return EXIT_SUCCESS;
}

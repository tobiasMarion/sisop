/*
 * ipc.c — Módulo de IPC: fork() + pipes
 *
 * INF01142 — Sistemas Operacionais — 2026/1
 *
 * =========================================================================
 * ESTE É O ÚNICO ARQUIVO QUE DEVE SER MODIFICADO.
 * =========================================================================
 *
 * Implemente todas as funções declaradas em ipc.h.
 * Consulte os comentários em ipc.h para o contrato de cada função.
 *
 * Chamadas de sistema que você precisará:
 *   pipe(2), fork(2), read(2), write(2), close(2),
 *   waitpid(2), select(2) ou poll(2), exit(3), malloc(3), free(3)
 *
 * Referências rápidas:
 *   man 2 pipe      man 2 fork     man 2 waitpid
 *   man 2 select    man 2 read     man 2 write
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <errno.h>

#include "mandelbrot.h"
#include "ipc.h"

/* =========================================================================
 * Definição interna do Pool
 *
 * Sugestão de estrutura — pode-se alterar caso necessário.
 * ========================================================================= */
typedef struct {
    pid_t pid;      /* PID do filho, ou -1 se a entrada está livre */
    int   read_fd;  /* descritor de leitura do pipe de retorno      */
} PoolEntry;

struct Pool {
    int        max;     /* capacidade máxima do pool  */
    int        active;  /* número de filhos ativos    */
    PoolEntry *entries; /* array de entradas          */
};

/* =========================================================================
 * pool_create
 * ========================================================================= */
Pool *pool_create(int max_children)
{
    Pool *pool = malloc(sizeof(Pool));
    if (!pool) return NULL;

    pool->max     = max_children;
    pool->active  = 0;
    pool->entries = malloc(sizeof(PoolEntry) * max_children);
    if (!pool->entries) { free(pool); return NULL; }

    for (int i = 0; i < max_children; i++) {
        pool->entries[i].pid     = -1;
        pool->entries[i].read_fd = -1;
    }
    return pool;
}

/* =========================================================================
 * pool_destroy
 * ========================================================================= */
void pool_destroy(Pool *pool)
{
    if (!pool) return;
    for (int i = 0; i < pool->max; i++) {
        if (pool->entries[i].read_fd != -1)
            close(pool->entries[i].read_fd);
    }
    free(pool->entries);
    free(pool);
}

/* =========================================================================
 * pool_active
 * ========================================================================= */
int pool_active(const Pool *pool)
{
    return pool->active;
}

/* =========================================================================
 * launch_worker
 *
 * TODO: implemente esta função.
 * ========================================================================= */
void launch_worker(Pool *pool, const RenderParams *params, const Tile *t) 
{
    (void)pool;
    (void)params;
    (void)t;


    int fd[2];
    if (pipe(fd) == -1) { perror("pipe"); return; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(fd[0]); close(fd[1]); return; }
    
    if (pid == 0) {
        // filho
        close(fd[0]);
        worker_main(params, t, fd[1]); // worker_main não retorna
    }
    
    // pai
    close(fd[1]);

    // registrar pid e fd[0] em uma entrada livre do pool
    // incrementar pool->active
    // Encontrar uma entrada livre no pool 
    for (int i = 0; i < pool->max; i++) {
        if (pool->entries[i].pid == -1) {
            pool->entries[i].pid = pid;
            pool->entries[i].read_fd = fd[0];
            pool->active++;
            break;
        }
    }
    
}

/* =========================================================================
 * worker_main
 *
 * TODO: implemente esta função.
 * ========================================================================= */
void worker_main(const RenderParams *params, const Tile *tile, int write_fd)
{
    (void)params;
    (void)tile;
    (void)write_fd;

    int n_pixels = tile->w * tile->h;
    unsigned char *buf = malloc(n_pixels);
    if (!buf) { perror("malloc"); exit(1); }

    compute_tile(params, tile, buf);

    // Escrever cabeçalho: ox, oy, w, h
    int header[4] = { tile->ox, tile->oy, tile->w, tile->h };
    if (write(write_fd, header, sizeof(header)) == -1) {
        perror("write header");
        free(buf);
        exit(1);
    }

    // Escrever pixels
    // Lembre: use um loop para garantir que todos os bytes foram escritos!
    int total_sent = 0;
    while (total_sent < n_pixels) {
        ssize_t sent = write(write_fd, buf + total_sent, n_pixels - total_sent);
        if (sent <= 0) break; 
        total_sent += sent;
    }

    close(write_fd);
    free(buf);
    exit(0);
}

/* =========================================================================
 * pool_collect_ready
 *
 * TODO: implemente esta função.
 * ========================================================================= */
int pool_collect_ready(Pool *pool, TileResult *result)
{
    (void)result;

    if (pool->active == 0) return 0;

    // Dica de estrutura com select():
    fd_set rfds;
    FD_ZERO(&rfds);
    int maxfd = -1;

    for (int i = 0; i < pool->max; i++) {
        if (pool->entries[i].pid != -1) {
            FD_SET(pool->entries[i].read_fd, &rfds);
            if (pool->entries[i].read_fd > maxfd)
                maxfd = pool->entries[i].read_fd;
        }
    }

    struct timeval tv = {0, 0}; // timeout zero = não bloqueia
    int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (ready <= 0) return 0;


    // Para cada entrada com dados:
    for (int i = 0; i < pool->max; i++) {
        int fd = pool->entries[i].read_fd;
        if (pool->entries[i].pid != -1 && FD_ISSET(fd, &rfds)) {
            
            // Ler cabeçalho (4 ints: ox, oy, w, h)
            int header[4];
            if (read(fd, header, sizeof(header)) < (ssize_t)sizeof(header)) {
                return 0; // Leitura incompleta ou erro
            }

            result->tile.ox = header[0];
            result->tile.oy = header[1];
            result->tile.w  = header[2];
            result->tile.h  = header[3];

            // Alocar result->pixels com malloc(w * h)
            int n_pixels = result->tile.w * result->tile.h;
            result->pixels = malloc(n_pixels);
            if (!result->pixels) return 0;

            // Ler os pixels em loop
            int total_read = 0;
            while (total_read < n_pixels) {
                ssize_t bytes = read(fd, result->pixels + total_read, n_pixels - total_read);
                if (bytes <= 0) break;
                total_read += bytes;
            }

            close(pool->entries[i].read_fd);
            pool->entries[i].pid = -1;
            pool->entries[i].read_fd = -1;
            pool->active--;

            return 1; // Sucesso
        }
    }


    return 0;
}

/* =========================================================================
 * pool_reap
 *
 * TODO: implemente esta função.
 * ========================================================================= */
void pool_reap(Pool *pool)
{

    int status;
    pid_t pid;

    // Enquanto existirem filhos que terminaram (sem bloquear):
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        // encontrar a entrada com esse pid no pool
        for (int i = 0; i < pool->max; i++) {
            if (pool->entries[i].pid == pid) {

                // fechar o read_fd correspondente
                close(pool->entries[i].read_fd);

                // marcar a entrada como livre (pid = -1, read_fd = -1)
                pool->entries[i].pid = -1;
                pool->entries[i].read_fd = -1;

                pool->active--;
                break;
            }
        }
    }

}

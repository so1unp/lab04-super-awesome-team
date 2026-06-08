#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include "shm.h"
#include "ipc.h"

/*
 * Crea la memoria compartida POSIX con el Mapa e inicializa su mutex.
 * Retorna puntero al Mapa mapeado, o NULL en error.
 */
Mapa *shm_crear(const Config *cfg)
{
    int fd;
    Mapa *mapa;
    pthread_mutexattr_t attr;

    /* Crear (o recrear) la shm */
    fd = shm_open(SHM_MAPA_NAME, O_CREAT | O_EXCL | O_RDWR, 0660);
    if (fd == -1) {
        /* Si ya existia de una ejecucion anterior, la eliminamos y reintentamos */
        shm_unlink(SHM_MAPA_NAME);
        fd = shm_open(SHM_MAPA_NAME, O_CREAT | O_EXCL | O_RDWR, 0660);
        if (fd == -1) {
            perror("shm_open");
            return NULL;
        }
    }

    /* Ajustar el tamaño */
    if (ftruncate(fd, sizeof(Mapa)) == -1) {
        perror("ftruncate");
        close(fd);
        shm_unlink(SHM_MAPA_NAME);
        return NULL;
    }

    /* Mapear */
    mapa = mmap(NULL, sizeof(Mapa), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mapa == MAP_FAILED) {
        perror("mmap");
        shm_unlink(SHM_MAPA_NAME);
        return NULL;
    }

    /* Inicializar todo en cero */
    memset(mapa, 0, sizeof(Mapa));

    /* Inicializar el mapa con celdas vacías */
    for (int i = 0; i < MAPA_FILAS; i++) {
        for (int j = 0; j < MAPA_COLS; j++) {
            mapa->celdas[i][j].tipo = CELDA_VACIA;
            mapa->celdas[i][j].idx  = -1;
        }
    }

    mapa->juego_activo    = 1;
    mapa->num_naves       = 0;
    mapa->num_estaciones  = 0;
    mapa->num_asteroides  = 0;

    /* Silenciar warning: cfg reservado para uso futuro (tamaño dinamico) */
    (void)cfg;

    /* Inicializar mutex como PROCESS_SHARED */
    if (pthread_mutexattr_init(&attr) != 0) {
        perror("pthread_mutexattr_init");
        munmap(mapa, sizeof(Mapa));
        shm_unlink(SHM_MAPA_NAME);
        return NULL;
    }
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
        perror("pthread_mutexattr_setpshared");
        pthread_mutexattr_destroy(&attr);
        munmap(mapa, sizeof(Mapa));
        shm_unlink(SHM_MAPA_NAME);
        return NULL;
    }
    if (pthread_mutex_init(&mapa->mutex, &attr) != 0) {
        perror("pthread_mutex_init");
        pthread_mutexattr_destroy(&attr);
        munmap(mapa, sizeof(Mapa));
        shm_unlink(SHM_MAPA_NAME);
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);

    return mapa;
}

/*
 * Desmapea y elimina la shm. Debe llamarse solo desde el servidor al cerrar.
 */
void shm_destruir(Mapa *mapa)
{
    if (mapa == NULL)
        return;
    pthread_mutex_destroy(&mapa->mutex);
    munmap(mapa, sizeof(Mapa));
    shm_unlink(SHM_MAPA_NAME);
}

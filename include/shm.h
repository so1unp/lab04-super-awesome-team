#ifndef SHM_H
#define SHM_H

#include "mapa.h"
#include "config.h"

/*
 * Crea la memoria compartida POSIX, inicializa el Mapa y su mutex
 * (PTHREAD_PROCESS_SHARED). Solo debe llamarla el servidor al arrancar.
 * Retorna puntero al Mapa mapeado, o NULL en error.
 */
Mapa *shm_crear(const Config *cfg);

/*
 * Destruye el mutex, desmapea y hace unlink de la shm.
 * Solo debe llamarla el servidor al cerrar.
 */
void shm_destruir(Mapa *mapa);

#endif /* SHM_H */

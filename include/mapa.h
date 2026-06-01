#ifndef MAPA_H
#define MAPA_H

#include <pthread.h>
#include "config.h"
#include "tipos.h"
#include "nave.h"
#include "estacion.h"
#include "asteroide.h"

/* ─── Celda del mapa ───────────────────────────────────────────────────── */
typedef struct {
    TipoCelda tipo;
    int idx;  /* indice en la tabla correspondiente (nave/estacion/asteroide), -1 si vacia */
} Celda;

/*
 * Estructura principal del mapa en memoria compartida.
 * Toda la informacion del estado del juego reside aqui.
 * El servidor es el unico escritor; los clientes solo leen para visualizacion
 * (salvo sus propios campos, que actualizan con el mutex correspondiente).
 */
typedef struct {
    Celda     celdas[MAPA_FILAS][MAPA_COLS];
    Nave      naves[MAX_NAVES];
    Estacion  estaciones[MAX_ESTACIONES];
    Asteroide asteroides[MAX_ASTEROIDES];
    int       num_naves;
    int       num_estaciones;
    int       num_asteroides;
    int       juego_activo;  /* 0 = todas las estaciones sin combustible -> fin */

    /*
     * Mutex de proceso compartido (PTHREAD_PROCESS_SHARED).
     * El servidor lo inicializa al crear la shm.
     * Debe tomarse antes de modificar celdas, naves, estaciones o asteroides.
     */
    pthread_mutex_t mutex;
} Mapa;

#endif /* MAPA_H */

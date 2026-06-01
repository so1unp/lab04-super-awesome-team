#ifndef MAPA_H
#define MAPA_H

#include <pthread.h>
#include "config.h"

/* ─── Tipos de celda ───────────────────────────────────────────────────── */
typedef enum {
    CELDA_VACIA     = '.',
    CELDA_NAVE      = '@',
    CELDA_ESTACION  = 'E',
    CELDA_ASTEROIDE = '*',
    CELDA_NAVE_MUERTA = 'X'   /* nave desactivada (saqueble) */
} TipoCelda;

/* ─── Estados de objetos ───────────────────────────────────────────────── */
typedef enum {
    ESTADO_ACTIVO      = 0,
    ESTADO_DESACTIVADO = 1
} Estado;

/* ─── Tipos de cliente ─────────────────────────────────────────────────── */
typedef enum {
    CLIENTE_NAVE    = 0,
    CLIENTE_ESTACION = 1
} TipoCliente;

/* ─── Asteroide ────────────────────────────────────────────────────────── */
typedef struct {
    int fila;
    int col;
    int deuterio;
    int mutexio;
    int semaforita;
    int kernelio;
    Estado estado;  /* ESTADO_DESACTIVADO cuando queda sin recursos */
} Asteroide;

/* ─── Nave ─────────────────────────────────────────────────────────────── */
typedef struct {
    int fila;
    int col;
    int combustible;
    int oxigeno;
    /* inventario de recursos recolectados */
    int deuterio;
    int mutexio;
    int semaforita;
    int kernelio;
    Estado estado;
    int pid;
    int id;  /* indice en la tabla de naves del servidor */
} Nave;

/* ─── Estacion ─────────────────────────────────────────────────────────── */
typedef struct {
    int fila;
    int col;
    int combustible;
    /* PIDs de las naves actualmente en el hangar (0 = libre) */
    int hangar[3];
    Estado estado;
    int pid;
    int id;  /* indice en la tabla de estaciones del servidor */
} Estacion;

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

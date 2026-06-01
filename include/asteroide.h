#ifndef ASTEROIDE_H
#define ASTEROIDE_H

#include "tipos.h"

/*
 * Representa un asteroide en el mapa.
 * Vive en Mapa.asteroides[] dentro de la memoria compartida.
 * El servidor actualiza sus recursos al extraer una nave.
 */
typedef struct {
    int fila;
    int col;
    int deuterio;
    int mutexio;
    int semaforita;
    int kernelio;
    Estado estado;  /* ESTADO_DESACTIVADO cuando queda sin recursos */
} Asteroide;

#endif /* ASTEROIDE_H */

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
    /*
     * Movimiento (asteroides moviles). El servidor avanza la posicion flotante
     * y la redondea a (fila, col). El angulo da la direccion; el giro lo cambia
     * un poquito cada tick (0 = recto, chico = pendular, grande = circular).
     */
    float pos_fila;
    float pos_col;
    float angulo;     /* radianes */
    float velocidad;  /* celdas por tick */
    float giro;       /* delta de angulo por tick */
} Asteroide;

#endif /* ASTEROIDE_H */

#ifndef ESTACION_H
#define ESTACION_H

#include "tipos.h"

/*
 * Representa el estado de una estacion espacial.
 * Vive en Mapa.estaciones[] dentro de la memoria compartida.
 * Solo la propia estacion escribe sus campos; el servidor los inicializa.
 */
typedef struct {
    int fila;
    int col;
    int combustible;
    /* PIDs de las naves actualmente en el hangar (0 = libre) */
    int hangar[3];
    Estado estado;
    int pid;
    int id;  /* indice en Mapa.estaciones[] */
} Estacion;

#endif /* ESTACION_H */

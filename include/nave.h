#include <stdbool.h>

#ifndef NAVE_H
#define NAVE_H

#include "tipos.h"
#include "config.h"

/*
 * Representa el estado de una nave espacial.
 * Vive en Mapa.naves[] dentro de la memoria compartida.
 * Solo la propia nave escribe sus campos; el servidor los inicializa.
 */
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
    int id;  /* indice en Mapa.naves[] */
    int direccion;
    int dinero; /* creditos: gana vendiendo minerales, gasta comprando combustible/oxigeno */
    bool escudo;
} Nave;

#endif /* NAVE_H */

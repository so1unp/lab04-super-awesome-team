#ifndef ASTEROIDES_H
#define ASTEROIDES_H

#include <stdio.h>
#include "mapa.h"
#include "config.h"

/*
 * Puebla el mapa con cfg->num_asteroides asteroides en posiciones
 * aleatorias libres. Cada asteroide recibe recursos aleatorios (0..100).
 * Debe llamarse después de shm_crear() y antes de aceptar clientes.
 */
void asteroides_inicializar(Mapa *mapa, const Config *cfg);

#endif /* ASTEROIDES_H */

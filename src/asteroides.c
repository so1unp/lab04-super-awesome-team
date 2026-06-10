#include <stdlib.h>
#include <time.h>
#include "asteroides.h"

/* Maximo de recursos por mineral en un asteroide */
#define MAX_RECURSO 100

/*
 * Inicializa N asteroides en posiciones aleatorias libres del mapa.
 * Cada asteroide recibe cantidades aleatorias (0..MAX_RECURSO) de cada mineral.
 * Las posiciones son distintas entre si y no coinciden con celdas ocupadas.
 */
void asteroides_inicializar(Mapa *mapa, const Config *cfg)
{
    int colocados = 0;
    int intentos  = 0;
    /* Evitar loop infinito si el mapa estuviese muy lleno */
    int max_intentos = cfg->num_asteroides * 100;

    srand((unsigned int)time(NULL));

    while (colocados < cfg->num_asteroides && intentos < max_intentos) {
        int fila = rand() % MAPA_FILAS;
        int col  = rand() % MAPA_COLS;
        intentos++;

        if (mapa->celdas[fila][col].tipo != CELDA_VACIA)
            continue;

        int idx = mapa->num_asteroides;

        mapa->asteroides[idx].fila       = fila;
        mapa->asteroides[idx].col        = col;
        mapa->asteroides[idx].deuterio   = rand() % (MAX_RECURSO + 1);
        mapa->asteroides[idx].mutexio    = rand() % (MAX_RECURSO + 1);
        mapa->asteroides[idx].semaforita = rand() % (MAX_RECURSO + 1);
        mapa->asteroides[idx].kernelio   = rand() % (MAX_RECURSO + 1);
        mapa->asteroides[idx].estado     = ESTADO_ACTIVO;

        mapa->celdas[fila][col].tipo = CELDA_ASTEROIDE;
        mapa->celdas[fila][col].idx  = idx;

        mapa->num_asteroides++;
        colocados++;
    }

    if (colocados < cfg->num_asteroides) {
        /* No es un error fatal, simplemente no habia espacio suficiente */
        fprintf(stderr, "asteroides: solo se colocaron %d de %d asteroides\n",
                colocados, cfg->num_asteroides);
    }
}

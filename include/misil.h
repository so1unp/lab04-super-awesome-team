#ifndef MISIL_H
#define MISIL_H

/*
 * Proyectil disparado por una nave (task: combate / disparo).
 * Vive en Mapa.misiles[] dentro de la memoria compartida. El servidor lo avanza
 * en su hilo de simulacion y resuelve las colisiones:
 *   - asteroide impactado: se destruye,
 *   - nave (distinta del dueno) impactada: queda desactivada,
 *   - estacion o borde del mapa: el misil se apaga.
 * No ocupa una celda del mapa: el radar lo dibuja en su posicion (fila, col).
 */
typedef struct {
    int activo;    /* 0 = libre, 1 = en vuelo */
    int fila;
    int col;
    int df;        /* desplazamiento por paso en fila (-1, 0, 1) */
    int dc;        /* desplazamiento por paso en columna (-1, 0, 1) */
    int id_dueno;  /* indice de la nave que disparo (no se auto-impacta) */
} Misil;

#endif /* MISIL_H */

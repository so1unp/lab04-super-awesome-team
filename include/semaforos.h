#ifndef SEMAFOROS_H
#define SEMAFOROS_H

#include <stddef.h>
#include <semaphore.h>

/*
 * Infraestructura de semaforos binarios POSIX por celda del mapa (task #29).
 *
 * Hay un semaforo nombrado por cada celda del mapa, nombrado siguiendo el
 * formato SEM_CELDA_FMT definido en ipc.h ("/cosmikernel_cell_RRR_CCC").
 * Cada semaforo arranca en 1 (celda libre); cuando una nave ocupa la celda
 * se decrementa a 0 (sem_wait/sem_trywait); al abandonarla se incrementa a 1
 * (sem_post).
 *
 * Convencion: el SERVIDOR los crea al iniciar y los destruye al cerrar.
 * Los CLIENTES (nave/estacion) solo los abren y cierran via sem_open/sem_close
 * en cada movimiento, usando los helpers de abajo.
 */

/*
 * Crea los MAPA_FILAS * MAPA_COLS semaforos nombrados, todos con valor 1.
 * Si algun semaforo de un nombre igual ya existia (por una corrida previa
 * que no limpio bien) se hace sem_unlink y se reintenta.
 *
 * Retorna 0 en exito, -1 si alguno falla (los creados quedan parcialmente).
 * Llamar tipicamente desde el servidor, despues de shm_crear().
 *
 * Nota: en el servidor se cierran inmediatamente los descriptores; el
 * semaforo persiste en el kernel hasta sem_unlink.
 */
int semaforos_crear(void);

/*
 * Hace sem_unlink de los MAPA_FILAS * MAPA_COLS semaforos.
 * Es idempotente: si alguno ya no existe, se ignora.
 * Llamar desde el servidor al cerrar (o desde la nave en modo standalone).
 */
void semaforos_destruir(void);

/*
 * Abre el semaforo nombrado de la celda (fila, col) ya creado por el servidor.
 * Devuelve un puntero a sem_t o NULL si falla.
 * El llamador debe cerrar el handle con semaforo_celda_cerrar() (no destruye
 * el semaforo, solo libera el file descriptor en este proceso).
 */
sem_t *semaforo_celda_abrir(int fila, int col);

/* Cierra el file descriptor del semaforo (NO destruye el semaforo). */
int semaforo_celda_cerrar(sem_t *sem);

/*
 * Helper: escribe en `out` el nombre POSIX del semaforo para (fila, col).
 * `len` debe ser al menos SEM_CELDA_NAME_LEN.
 */
void semaforo_celda_nombre(char *out, size_t len, int fila, int col);

#endif /* SEMAFOROS_H */

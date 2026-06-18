/*
 * src/semaforos.c - Infraestructura de semaforos binarios por celda (task #29).
 *
 * Ver include/semaforos.h para la documentacion de la API.
 */

#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "config.h"
#include "ipc.h"
#include "semaforos.h" 

// Definir nombre
void semaforo_celda_nombre(char *out, size_t len, int fila, int col)
{
    snprintf(out, len, SEM_CELDA_FMT, fila, col);
}

sem_t *semaforo_celda_abrir(int fila, int col)
{
    char nombre[SEM_CELDA_NAME_LEN];
    sem_t *sem;

    semaforo_celda_nombre(nombre, sizeof(nombre), fila, col);
    sem = sem_open(nombre, 0);   /* sin O_CREAT: debe existir */
    if (sem == SEM_FAILED)
        return NULL;
    return sem;
}

int semaforo_celda_cerrar(sem_t *sem)
{
    return sem_close(sem);
}

int semaforos_crear(void)
{
    char nombre[SEM_CELDA_NAME_LEN];
    sem_t *sem;

    for (int f = 0; f < MAPA_FILAS; f++)
    {
        for (int c = 0; c < MAPA_COLS; c++)
        {
            semaforo_celda_nombre(nombre, sizeof(nombre), f, c);

            /* Por si quedo colgado de una ejecucion anterior. */
            sem_unlink(nombre);

            /* Valor inicial 1 = celda libre. O_EXCL para detectar conflictos. */
            sem = sem_open(nombre, O_CREAT | O_EXCL, 0666, 1);
            if (sem == SEM_FAILED)
            {
                fprintf(stderr, "semaforos_crear: fallo en celda (%d,%d) [%s]\n",
                        f, c, nombre);
                return -1;
            }
            /* En el servidor no necesitamos mantener el handle abierto: los
             * clientes lo abriran por nombre cuando se muevan. */
            sem_close(sem);
        }
    }
    return 0;
}

void semaforos_destruir(void)
{
    char nombre[SEM_CELDA_NAME_LEN];

    for (int f = 0; f < MAPA_FILAS; f++)
    {
        for (int c = 0; c < MAPA_COLS; c++)
        {
            semaforo_celda_nombre(nombre, sizeof(nombre), f, c);
            sem_unlink(nombre);   /* ignora ENOENT */
        }
    }
}

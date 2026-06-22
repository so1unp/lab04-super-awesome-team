#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include "config.h"
#include "shm.h"
#include "ipc.h"
#include "asteroides.h"
#include "semaforos.h"
#include "registro.h"
#include "apagado.h"

/* Tick base del hilo de simulacion (ms). Los misiles avanzan cada tick; los
 * asteroides se mueven cada intervalo_asteroides_ms (acumulando ticks). */
#define SIM_TICK_MS 50

/* Bandera de parada del hilo de simulacion (la levanta el main al apagar). */
static volatile sig_atomic_t g_sim_stop = 0;

typedef struct
{
    Mapa *mapa;
    int intervalo_asteroides_ms;
} SimArgs;

/*
 * Mueve cada asteroide activo segun su angulo y velocidad, girando un poco
 * (giro) para el efecto pendular/circular. Toroidal en los bordes; si la celda
 * destino esta ocupada, gira un poco y no avanza (para no pisar naves/etc).
 * Debe llamarse con el mutex del mapa tomado.
 */
static void mover_asteroides(Mapa *mapa)
{
    for (int i = 0; i < MAX_ASTEROIDES; i++)
    {
        Asteroide *a = &mapa->asteroides[i];
        if (a->estado != ESTADO_ACTIVO)
            continue;

        a->angulo += a->giro;

        float nf = a->pos_fila + a->velocidad * sinf(a->angulo);
        float nc = a->pos_col + a->velocidad * cosf(a->angulo);

        /* Wraparound toroidal. */
        if (nf < 0.0f) nf += (float)MAPA_FILAS;
        if (nf >= (float)MAPA_FILAS) nf -= (float)MAPA_FILAS;
        if (nc < 0.0f) nc += (float)MAPA_COLS;
        if (nc >= (float)MAPA_COLS) nc -= (float)MAPA_COLS;

        int cf = (int)nf;
        int cc = (int)nc;
        if (cf < 0) cf = 0;
        if (cf >= MAPA_FILAS) cf = MAPA_FILAS - 1;
        if (cc < 0) cc = 0;
        if (cc >= MAPA_COLS) cc = MAPA_COLS - 1;

        if (cf == a->fila && cc == a->col)
        {
            /* Mismo casillero: solo actualizar la posicion sub-celda. */
            a->pos_fila = nf;
            a->pos_col = nc;
            continue;
        }

        if (mapa->celdas[cf][cc].tipo == CELDA_VACIA)
        {
            /* Liberar la celda actual (si todavia es este asteroide). */
            if (mapa->celdas[a->fila][a->col].tipo == CELDA_ASTEROIDE &&
                mapa->celdas[a->fila][a->col].idx == i)
            {
                mapa->celdas[a->fila][a->col].tipo = CELDA_VACIA;
                mapa->celdas[a->fila][a->col].idx = -1;
            }
            mapa->celdas[cf][cc].tipo = CELDA_ASTEROIDE;
            mapa->celdas[cf][cc].idx = i;
            a->fila = cf;
            a->col = cc;
            a->pos_fila = nf;
            a->pos_col = nc;
        }
        else
        {
            /* Celda ocupada (nave/estacion/otro asteroide): no avanzamos, pero
             * giramos un poco para buscar otra ruta (evita quedar trabado). */
            a->angulo += 0.5f;
        }
    }
}

/*
 * Avanza cada misil en vuelo y resuelve colisiones:
 *   - asteroide: se destruye y el misil se apaga,
 *   - nave (distinta del dueno): queda desactivada (nave muerta) y el misil se apaga,
 *   - estacion o borde del mapa: el misil se apaga.
 * Debe llamarse con el mutex del mapa tomado.
 */
static void avanzar_misiles(Mapa *mapa)
{
    for (int i = 0; i < MAX_MISILES; i++)
    {
        Misil *m = &mapa->misiles[i];
        if (!m->activo)
            continue;

        int nf = m->fila + m->df;
        int nc = m->col + m->dc;

        if (nf < 0 || nf >= MAPA_FILAS || nc < 0 || nc >= MAPA_COLS)
        {
            m->activo = 0; /* salio del mapa */
            continue;
        }

        TipoCelda t = mapa->celdas[nf][nc].tipo;
        int idx = mapa->celdas[nf][nc].idx;

        if (t == CELDA_ASTEROIDE)
        {
            int valor = 10; /* botin minimo */
            if (idx >= 0 && idx < MAX_ASTEROIDES)
            {
                /* El botin vale lo que tenia el asteroide (suma de minerales). */
                valor = mapa->asteroides[idx].deuterio + mapa->asteroides[idx].mutexio +
                        mapa->asteroides[idx].semaforita + mapa->asteroides[idx].kernelio;
                if (valor < 10)
                    valor = 10;
                mapa->asteroides[idx].estado = ESTADO_DESACTIVADO;
                mapa->asteroides[idx].deuterio = 0;
                mapa->asteroides[idx].mutexio = 0;
                mapa->asteroides[idx].semaforita = 0;
                mapa->asteroides[idx].kernelio = 0;
                if (mapa->num_asteroides > 0)
                    mapa->num_asteroides--;
            }
            /* El asteroide destruido deja un botin de $ (guardamos el monto en idx). */
            mapa->celdas[nf][nc].tipo = CELDA_BOTIN;
            mapa->celdas[nf][nc].idx = valor;
            m->activo = 0;
        }
        else if (t == CELDA_NAVE && idx != m->id_dueno)
        {
            if (idx >= 0 && idx < MAX_NAVES)
            {
                mapa->naves[idx].estado = ESTADO_DESACTIVADO;
                mapa->celdas[nf][nc].tipo = CELDA_NAVE_MUERTA;
                mapa->celdas[nf][nc].idx = idx;
            }
            m->activo = 0;
        }
        else if (t == CELDA_ESTACION)
        {
            m->activo = 0; /* la estacion es indestructible */
        }
        else
        {
            /* Celda vacia, nave muerta o la propia nave del dueno: avanza. */
            m->fila = nf;
            m->col = nc;
        }
    }
}

static void *simulacion_thread(void *arg)
{
    SimArgs *args = (SimArgs *)arg;
    Mapa *mapa = args->mapa;
    int acum_ms = 0;

    while (!g_sim_stop)
    {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = SIM_TICK_MS * 1000000L};
        nanosleep(&ts, NULL);
        if (g_sim_stop)
            break;

        pthread_mutex_lock(&mapa->mutex);
        avanzar_misiles(mapa);
        acum_ms += SIM_TICK_MS;
        if (acum_ms >= args->intervalo_asteroides_ms)
        {
            acum_ms = 0;
            mover_asteroides(mapa);
        }
        pthread_mutex_unlock(&mapa->mutex);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    const char *config_path = (argc > 1) ? argv[1] : CONFIG_PATH;
    Config cfg;
    Mapa *mapa;

    if (config_load(config_path, &cfg) == -1)
        fprintf(stderr, "servidor: arrancando con valores por defecto\n");

    config_print(&cfg);

    mapa = shm_crear(&cfg);
    if (mapa == NULL)
    {
        fprintf(stderr, "servidor: error al crear la memoria compartida\n");
        exit(EXIT_FAILURE);
    }

    printf("servidor: shm '%s' creada (%zu bytes)\n", SHM_MAPA_NAME, sizeof(Mapa));

    asteroides_inicializar(mapa, &cfg);
    printf("servidor: %d asteroides colocados en el mapa\n", mapa->num_asteroides);

    /* Task #29: semaforos binarios por celda del mapa. */
    if (semaforos_crear() == -1)
    {
        fprintf(stderr, "servidor: error creando semaforos de celda\n");
        shm_destruir(mapa);
        exit(EXIT_FAILURE);
    }
    printf("servidor: %d semaforos de celda creados\n", MAPA_FILAS * MAPA_COLS);

    /* Hilo de simulacion: mueve asteroides y avanza misiles. */
    pthread_t th_sim;
    SimArgs sim_args = {.mapa = mapa, .intervalo_asteroides_ms = cfg.intervalo_asteroides_ms};
    int sim_ok = (pthread_create(&th_sim, NULL, simulacion_thread, &sim_args) == 0);
    if (!sim_ok)
        fprintf(stderr, "servidor: no se pudo crear el hilo de simulacion\n");

    printf("servidor: esperando registros (Ctrl+C para salir)\n");
    (void)registro_servidor_loop(mapa, &cfg);

    /* Parar el hilo de simulacion antes de apagar. */
    g_sim_stop = 1;
    if (sim_ok)
        pthread_join(th_sim, NULL);

    apagado_guardar_y_notificar(mapa, &cfg);

    semaforos_destruir();

    shm_destruir(mapa);
    exit(EXIT_SUCCESS);
}

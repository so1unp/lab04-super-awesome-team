#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include "apagado.h"
#include "ipc.h"

#define ESTADO_FILE "estado_mapa.txt"

static void guardar_estado(const Mapa *mapa, FILE *f)
{
    time_t ahora = time(NULL);
    fprintf(f, "# CosmiKernel - estado guardado: %s", ctime(&ahora));
    fprintf(f, "juego_activo=%d\n\n", mapa->juego_activo);

    fprintf(f, "# Asteroides (%d)\n", mapa->num_asteroides);
    for (int i = 0; i < mapa->num_asteroides; i++) {
        const Asteroide *a = &mapa->asteroides[i];
        fprintf(f, "asteroide fila=%d col=%d deuterio=%d mutexio=%d semaforita=%d kernelio=%d estado=%d\n",
                a->fila, a->col, a->deuterio, a->mutexio, a->semaforita, a->kernelio, (int)a->estado);
    }

    fprintf(f, "\n# Naves (%d)\n", mapa->num_naves);
    for (int i = 0; i < MAX_NAVES; i++) {
        const Nave *n = &mapa->naves[i];
        if (n->pid == 0)
            continue;
        fprintf(f, "nave id=%d pid=%d fila=%d col=%d combustible=%d oxigeno=%d "
                   "deuterio=%d mutexio=%d semaforita=%d kernelio=%d estado=%d\n",
                n->id, n->pid, n->fila, n->col, n->combustible, n->oxigeno,
                n->deuterio, n->mutexio, n->semaforita, n->kernelio, (int)n->estado);
    }

    fprintf(f, "\n# Estaciones (%d)\n", mapa->num_estaciones);
    for (int i = 0; i < MAX_ESTACIONES; i++) {
        const Estacion *e = &mapa->estaciones[i];
        if (e->pid == 0)
            continue;
        fprintf(f, "estacion id=%d pid=%d fila=%d col=%d combustible=%d estado=%d\n",
                e->id, e->pid, e->fila, e->col, e->combustible, (int)e->estado);
    }
}

static void notificar_clientes(const Mapa *mapa, const Config *cfg)
{
    /* Enviar SIGUSR1 a todas las naves activas */
    for (int i = 0; i < MAX_NAVES; i++) {
        if (mapa->naves[i].pid != 0)
            kill(mapa->naves[i].pid, SIGUSR1);
    }

    /* Enviar SIGUSR1 a todas las estaciones activas */
    int max_est = cfg->num_estaciones < MAX_ESTACIONES ? cfg->num_estaciones : MAX_ESTACIONES;
    for (int i = 0; i < max_est; i++) {
        if (mapa->estaciones[i].pid != 0)
            kill(mapa->estaciones[i].pid, SIGUSR1);
    }
}

void apagado_guardar_y_notificar(const Mapa *mapa, const Config *cfg)
{
    FILE *f;

    /* Notificar primero para que los clientes sepan cuanto antes */
    notificar_clientes(mapa, cfg);

    /* Guardar estado en archivo */
    f = fopen(ESTADO_FILE, "w");
    if (f == NULL) {
        perror("apagado: no se pudo abrir " ESTADO_FILE);
        return;
    }

    guardar_estado(mapa, f);
    fclose(f);

    printf("servidor: estado guardado en '%s'\n", ESTADO_FILE);
}

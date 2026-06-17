#include <stdio.h>
#include <stdlib.h>
#include "config.h"
#include "shm.h"
#include "ipc.h"
#include "asteroides.h"
#include "registro.h"

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

    printf("servidor: esperando registros (Ctrl+C para salir)\n");
    (void)registro_servidor_loop(mapa, &cfg);

    shm_destruir(mapa);
    exit(EXIT_SUCCESS);
}
#include <stdio.h>
#include <stdlib.h>
#include "config.h"

int main(int argc, char *argv[])
{
    const char *config_path = (argc > 1) ? argv[1] : CONFIG_PATH;
    Config cfg;

    if (config_load(config_path, &cfg) == -1)
        fprintf(stderr, "servidor: arrancando con valores por defecto\n");

    config_print(&cfg);

    // Agregar código aquí.

    exit(EXIT_SUCCESS);
}
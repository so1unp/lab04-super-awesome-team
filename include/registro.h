#ifndef REGISTRO_H
#define REGISTRO_H

#include "mapa.h"
#include "config.h"

/*
 * Inicia el servicio de registro/desregistro de clientes usando cola POSIX.
 * Bloquea en un loop procesando mensajes hasta recibir SIGINT/SIGTERM.
 * Retorna 0 en cierre normal, -1 en error de inicializacion.
 */
int registro_servidor_loop(Mapa *mapa, const Config *cfg);

#endif /* REGISTRO_H */

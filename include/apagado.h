#ifndef APAGADO_H
#define APAGADO_H

#include "mapa.h"
#include "config.h"

/*
 * Guarda el estado del mapa en disco (estado_mapa.txt) y envía SIGUSR1
 * a todos los clientes activos (naves y estaciones) para notificar la
 * desconexión del servidor.
 */
void apagado_guardar_y_notificar(const Mapa *mapa, const Config *cfg);

#endif /* APAGADO_H */

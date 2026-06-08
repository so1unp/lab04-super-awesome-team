#ifndef IPC_H
#define IPC_H

#include <sys/types.h>
#include "tipos.h"

/* ─── Nombres de recursos IPC (creados por el servidor) ───────────────── */

/* Memoria compartida POSIX que contiene el struct Mapa completo */
#define SHM_MAPA_NAME "/cosmikernel_mapa"

/*
 * Semáforo binario por celda del mapa.
 * Formato: /cosmikernel_cell_RRR_CCC  (3 digitos fila, 3 digitos columna)
 * Creados por el servidor al iniciar; destruidos al cerrar.
 */
#define SEM_CELDA_FMT "/cosmikernel_cell_%03d_%03d"
#define SEM_CELDA_NAME_LEN 32

/*
 * Cola de mensajes de la estacion (una por estacion).
 * Las naves envian aqui sus transacciones cuando estan en el hangar.
 * Formato: /cosmikernel_estacion_ID
 */
#define MQ_ESTACION_FMT "/cosmikernel_estacion_%d"
#define MQ_ESTACION_NAME_LEN 32

/*
 * Cola de mensajes de cada nave para recibir alertas del cuadrante.
 * Formato: /cosmikernel_nave_PID
 */
#define MQ_NAVE_FMT "/cosmikernel_nave_%d"
#define MQ_NAVE_NAME_LEN 32

/* Cola de registro: las naves y estaciones se registran al servidor aqui */
#define MQ_REGISTRO_NAME "/cosmikernel_registro"

/* ─── Mensajes IPC ─────────────────────────────────────────────────────── */

/*
 * Mensaje de registro: enviado por naves y estaciones al servidor
 * a través de MQ_REGISTRO_NAME al conectarse.
 */
typedef struct
{
    TipoCliente tipo; /* CLIENTE_NAVE o CLIENTE_ESTACION */
    pid_t pid;
} MsgRegistro;

/*
 * Respuesta del servidor al mensaje de registro.
 * Indica la posicion inicial asignada y el id en la tabla correspondiente.
 * Retorna -1 en fila/col si no hay lugar disponible.
 */
typedef struct
{
    int id; /* indice en naves[] o estaciones[] dentro del Mapa */
    int fila;
    int col;
    int error; /* 0 = ok, != 0 = rechazo */
} MsgRegistroResp;

/*
 * Mensaje de transaccion: enviado por una nave a la cola de la estacion.
 * Solo valido cuando la nave esta en el hangar (verificado por la estacion).
 */
typedef struct
{
    TipoOperacion operacion;
    int cantidad;
    pid_t pid_nave;
    int id_nave; /* indice en naves[] del Mapa */
} MsgTransaccion;

/*
 * Respuesta de la estacion a la transaccion.
 * Enviada de vuelta a la cola MQ_NAVE_FMT de la nave.
 */
typedef struct
{
    TipoOperacion operacion;
    int cantidad_efectiva; /* puede ser menor si la estacion no tiene suficiente */
    int precio_total;
    int error; /* 0 = ok, != 0 = fallo (hangar lleno, sin stock, etc.) */
} MsgTransaccionResp;

/*
 * Alerta de combustible bajo: enviada por la estacion a la cola de cada nave
 * registrada cuando su combustible baja del umbral configurado.
 */
typedef struct
{
    int id_estacion;
    pid_t pid_estacion;
    int combustible_actual;
} MsgAlertaCombustible;

#endif /* IPC_H */

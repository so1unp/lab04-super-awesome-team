#ifndef TIPOS_H
#define TIPOS_H

/* ─── Tipos de celda ───────────────────────────────────────────────────── */
typedef enum {
    CELDA_VACIA       = '.',
    CELDA_NAVE        = '@',
    CELDA_ESTACION    = 'E',
    CELDA_ASTEROIDE   = '*',
    CELDA_NAVE_MUERTA = 'X'   /* nave desactivada (saqueble) */
} TipoCelda;

/* ─── Estado de un objeto en el juego ─────────────────────────────────── */
typedef enum {
    ESTADO_ACTIVO      = 0,
    ESTADO_DESACTIVADO = 1
} Estado;

/* ─── Tipo de cliente que se conecta al servidor ──────────────────────── */
typedef enum {
    CLIENTE_NAVE     = 0,
    CLIENTE_ESTACION = 1
} TipoCliente;

/* TipoOperacion se define en ipc.h (es parte del protocolo de mensajes) */

#endif /* TIPOS_H */

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

/* ─── Tipo de operacion en una transaccion nave ↔ estacion ───────────── */
typedef enum {
    OP_VENDER_DEUTERIO     = 0,
    OP_VENDER_MUTEXIO      = 1,
    OP_VENDER_SEMAFORITA   = 2,
    OP_VENDER_KERNELIO     = 3,
    OP_COMPRAR_COMBUSTIBLE = 4,
    OP_COMPRAR_OXIGENO     = 5
} TipoOperacion;

#endif /* TIPOS_H */

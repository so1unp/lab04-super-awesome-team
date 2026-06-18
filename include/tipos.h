#ifndef TIPOS_H
#define TIPOS_H

/* ─── Tipos de celda ───────────────────────────────────────────────────── */
/*
 * El valor de cada constante es el caracter con que se representa la celda
 * al dibujar el mapa. Es la representacion comun para todos los procesos
 * (servidor, naves, estaciones): cualquiera que dibuje el mapa usa estos
 * mismos simbolos.
 */
typedef enum {
    CELDA_VACIA       = ' ',  /* espacio profundo */
    CELDA_NAVE        = '^',  /* nave espacial */
    CELDA_ESTACION    = '#',  /* estacion espacial */
    CELDA_ASTEROIDE   = '@',  /* asteroide */
    CELDA_NAVE_MUERTA = 'X'   /* nave desactivada (saqueable) */
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

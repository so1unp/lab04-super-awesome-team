#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_PATH "config.txt"

/* Valores por defecto */
#define DEFAULT_NUM_ESTACIONES         2
#define DEFAULT_NUM_ASTEROIDES         5
#define DEFAULT_PRECIO_DEUTERIO        10
#define DEFAULT_PRECIO_MUTEXIO         20
#define DEFAULT_PRECIO_SEMAFORITA      15
#define DEFAULT_PRECIO_KERNELIO        25
#define DEFAULT_PRECIO_COMBUSTIBLE     12
#define DEFAULT_PRECIO_OXIGENO         8
#define DEFAULT_UMBRAL_COMBUSTIBLE     30
#define DEFAULT_INTERVALO_OXIGENO      1   /* segundos entre decrementos de oxigeno en nave */
#define DEFAULT_INTERVALO_COMBUSTIBLE  10  /* segundos entre decrementos de combustible en estacion */
#define DEFAULT_RADAR_REFRESH_MS       100 /* milisegundos entre refrescos del radar (ncurses) */

/* Limites del juego */
#define MAX_ESTACIONES  3
#define MAX_NAVES       8
#define MAX_ASTEROIDES  20
#define MAPA_FILAS      24
#define MAPA_COLS       80

/* Recursos iniciales */
#define NAVE_COMBUSTIBLE_INICIAL    100
#define NAVE_OXIGENO_INICIAL        100
#define ESTACION_COMBUSTIBLE_INICIAL 200

typedef struct {
    int num_estaciones;
    int num_asteroides;
    int precio_deuterio;
    int precio_mutexio;
    int precio_semaforita;
    int precio_kernelio;
    int precio_combustible;
    int precio_oxigeno;
    int umbral_combustible_estacion;
    int intervalo_oxigeno_nave;        /* segundos */
    int intervalo_combustible_estacion; /* segundos */
    int radar_refresh_ms;              /* milisegundos entre refrescos del radar */
} Config;

/*
 * Carga la configuracion desde el archivo en 'path'.
 * Si un campo no esta presente usa el valor por defecto.
 * Retorna 0 en exito, -1 si no se puede abrir el archivo.
 */
int config_load(const char *path, Config *cfg);

/*
 * Imprime la configuracion actual por stdout (para debug).
 */
void config_print(const Config *cfg);

#endif /* CONFIG_H */

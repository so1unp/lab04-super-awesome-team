#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"

/* Carga los valores por defecto en cfg. */
static void config_defaults(Config *cfg)
{
    cfg->num_estaciones = DEFAULT_NUM_ESTACIONES;
    cfg->num_asteroides = DEFAULT_NUM_ASTEROIDES;
    cfg->precio_deuterio = DEFAULT_PRECIO_DEUTERIO;
    cfg->precio_mutexio = DEFAULT_PRECIO_MUTEXIO;
    cfg->precio_semaforita = DEFAULT_PRECIO_SEMAFORITA;
    cfg->precio_kernelio = DEFAULT_PRECIO_KERNELIO;
    cfg->precio_combustible = DEFAULT_PRECIO_COMBUSTIBLE;
    cfg->precio_oxigeno = DEFAULT_PRECIO_OXIGENO;
    cfg->umbral_combustible_estacion = DEFAULT_UMBRAL_COMBUSTIBLE;
    cfg->intervalo_oxigeno_nave = DEFAULT_INTERVALO_OXIGENO;
    cfg->intervalo_combustible_estacion = DEFAULT_INTERVALO_COMBUSTIBLE;
    cfg->radar_refresh_ms = DEFAULT_RADAR_REFRESH_MS;
    cfg->intervalo_asteroides_ms = DEFAULT_INTERVALO_ASTEROIDES_MS;
}

/*
 * Parsea una linea "clave = valor".
 * Ignora lineas en blanco y comentarios (inicio con '#').
 * Retorna 1 si se reconocio la clave, 0 si no.
 */
static int config_parse_line(const char *line, Config *cfg)
{
    char key[64];
    int val;

    /* ignorar comentarios y lineas vacias */
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
        return 0;

    if (sscanf(line, " %63[^= ] = %d", key, &val) != 2)
        return 0;

    if (strcmp(key, "num_estaciones") == 0)
        cfg->num_estaciones = val;
    else if (strcmp(key, "num_asteroides") == 0)
        cfg->num_asteroides = val;
    else if (strcmp(key, "precio_deuterio") == 0)
        cfg->precio_deuterio = val;
    else if (strcmp(key, "precio_mutexio") == 0)
        cfg->precio_mutexio = val;
    else if (strcmp(key, "precio_semaforita") == 0)
        cfg->precio_semaforita = val;
    else if (strcmp(key, "precio_kernelio") == 0)
        cfg->precio_kernelio = val;
    else if (strcmp(key, "precio_combustible") == 0)
        cfg->precio_combustible = val;
    else if (strcmp(key, "precio_oxigeno") == 0)
        cfg->precio_oxigeno = val;
    else if (strcmp(key, "umbral_combustible_estacion") == 0)
        cfg->umbral_combustible_estacion = val;
    else if (strcmp(key, "intervalo_oxigeno_nave") == 0)
        cfg->intervalo_oxigeno_nave = val;
    else if (strcmp(key, "intervalo_combustible_estacion") == 0)
        cfg->intervalo_combustible_estacion = val;
    else if (strcmp(key, "radar_refresh_ms") == 0)
        cfg->radar_refresh_ms = val;
    else if (strcmp(key, "intervalo_asteroides_ms") == 0)
        cfg->intervalo_asteroides_ms = val;
    else
        return 0;

    return 1;
}

/*
 * Valida que los valores de cfg esten dentro de rangos razonables.
 * Imprime advertencia y corrige si algun valor es invalido.
 */
static void config_validate(Config *cfg)
{
    if (cfg->num_estaciones < 1 || cfg->num_estaciones > MAX_ESTACIONES)
    {
        fprintf(stderr, "config: num_estaciones invalido (%d), usando %d\n",
                cfg->num_estaciones, DEFAULT_NUM_ESTACIONES);
        cfg->num_estaciones = DEFAULT_NUM_ESTACIONES;
    }
    if (cfg->num_asteroides < 1 || cfg->num_asteroides > MAX_ASTEROIDES)
    {
        fprintf(stderr, "config: num_asteroides invalido (%d), usando %d\n",
                cfg->num_asteroides, DEFAULT_NUM_ASTEROIDES);
        cfg->num_asteroides = DEFAULT_NUM_ASTEROIDES;
    }
    if (cfg->precio_deuterio <= 0)
        cfg->precio_deuterio = DEFAULT_PRECIO_DEUTERIO;
    if (cfg->precio_mutexio <= 0)
        cfg->precio_mutexio = DEFAULT_PRECIO_MUTEXIO;
    if (cfg->precio_semaforita <= 0)
        cfg->precio_semaforita = DEFAULT_PRECIO_SEMAFORITA;
    if (cfg->precio_kernelio <= 0)
        cfg->precio_kernelio = DEFAULT_PRECIO_KERNELIO;
    if (cfg->precio_combustible <= 0)
        cfg->precio_combustible = DEFAULT_PRECIO_COMBUSTIBLE;
    if (cfg->precio_oxigeno <= 0)
        cfg->precio_oxigeno = DEFAULT_PRECIO_OXIGENO;
    if (cfg->umbral_combustible_estacion <= 0)
        cfg->umbral_combustible_estacion = DEFAULT_UMBRAL_COMBUSTIBLE;
    if (cfg->intervalo_oxigeno_nave <= 0)
        cfg->intervalo_oxigeno_nave = DEFAULT_INTERVALO_OXIGENO;
    if (cfg->intervalo_combustible_estacion <= 0)
        cfg->intervalo_combustible_estacion = DEFAULT_INTERVALO_COMBUSTIBLE;
    if (cfg->radar_refresh_ms < 10 || cfg->radar_refresh_ms > 5000)
    {
        fprintf(stderr, "config: radar_refresh_ms invalido (%d), usando %d\n",
                cfg->radar_refresh_ms, DEFAULT_RADAR_REFRESH_MS);
        cfg->radar_refresh_ms = DEFAULT_RADAR_REFRESH_MS;
    }
    if (cfg->intervalo_asteroides_ms < 20 || cfg->intervalo_asteroides_ms > 10000)
    {
        fprintf(stderr, "config: intervalo_asteroides_ms invalido (%d), usando %d\n",
                cfg->intervalo_asteroides_ms, DEFAULT_INTERVALO_ASTEROIDES_MS);
        cfg->intervalo_asteroides_ms = DEFAULT_INTERVALO_ASTEROIDES_MS;
    }
}

int config_load(const char *path, Config *cfg)
{
    FILE *f;
    char line[128];

    config_defaults(cfg);

    f = fopen(path, "r");
    if (f == NULL)
    {
        fprintf(stderr, "config: no se pudo abrir '%s', usando valores por defecto\n", path);
        return -1;
    }

    while (fgets(line, sizeof(line), f) != NULL)
        config_parse_line(line, cfg);

    fclose(f);
    config_validate(cfg);
    return 0;
}

void config_print(const Config *cfg)
{
    printf("=== Configuracion ===\n");
    printf("  num_estaciones              = %d\n", cfg->num_estaciones);
    printf("  num_asteroides              = %d\n", cfg->num_asteroides);
    printf("  precio_deuterio             = %d\n", cfg->precio_deuterio);
    printf("  precio_mutexio              = %d\n", cfg->precio_mutexio);
    printf("  precio_semaforita           = %d\n", cfg->precio_semaforita);
    printf("  precio_kernelio             = %d\n", cfg->precio_kernelio);
    printf("  precio_combustible          = %d\n", cfg->precio_combustible);
    printf("  precio_oxigeno              = %d\n", cfg->precio_oxigeno);
    printf("  umbral_combustible_estacion = %d\n", cfg->umbral_combustible_estacion);
    printf("  intervalo_oxigeno_nave      = %d s\n", cfg->intervalo_oxigeno_nave);
    printf("  intervalo_combustible_est   = %d s\n", cfg->intervalo_combustible_estacion);
    printf("  radar_refresh_ms            = %d ms\n", cfg->radar_refresh_ms);
    printf("  intervalo_asteroides_ms     = %d ms\n", cfg->intervalo_asteroides_ms);
    printf("=====================\n");
}

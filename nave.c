/*
 * nave.c - Proceso cliente "nave espacial".
 *
 * Implementa los hilos:
 *   - radar (task #27): lee la SHM y dibuja mapa + panel de estado con ncurses.
 *   - soporte vital (task #20): decrementa periodicamente el oxigeno de la nave
 *     en la SHM (mapa->naves[id].oxigeno).
 *
 * Flujo del main:
 *   1. Carga config.txt (radar_refresh_ms, intervalo_oxigeno_nave).
 *   2. Abre la SHM /cosmikernel_mapa creada por el servidor. El servidor
 *      debe estar corriendo; si la SHM no existe, la nave aborta con error.
 *   3. Se registra contra el servidor via MQ_REGISTRO_NAME para obtener su
 *      id en Mapa.naves[]. Si el registro falla, aborta con error.
 *   4. Inicializa ncurses, instala handler de SIGINT.
 *   5. Lanza los hilos radar y soporte_vital.
 *   6. Espera a que se levante la bandera de salida; limpia recursos.
 *
 * Compilar (POSIX, Linux): make
 */

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <ncurses.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"
#include "mapa.h"
#include "semaforos.h"

/* ─── Estado global del proceso nave ──────────────────────────────────── */

/* Bandera de salida (se setea desde el handler de SIGINT). */
static volatile sig_atomic_t g_salir = 0;

/* Argumentos pasados al hilo radar. */
typedef struct
{
    Mapa *mapa;
    int id_nave;
    int refresh_ms;
} RadarArgs;

/* Argumentos pasados al hilo de soporte vital. */
typedef struct
{
    Mapa *mapa;
    int id_nave;
    int intervalo_seg;
} VitalArgs;

/* Argumentos pasados al hilo de propulsion. */
typedef struct
{
    Mapa *mapa;
    int id_nave;
} PropulsionArgs;

static const int df[4] = {-1, 0, 1, 0};
static const int dc[4] = {0, 1, 0, -1};

/* ─── Handler de SIGINT ───────────────────────────────────────────────── */

static void manejar_sigint(int sig)
{
    (void)sig;
    g_salir = 1;
}

/* ─── Abrir SHM del mapa ──────────────────────────────────────────────── */

/*
 * Abre la SHM creada por el servidor en /cosmikernel_mapa.
 * El servidor es el unico que crea la SHM; si no existe, esta funcion
 * falla (la nave no puede operar sin servidor).
 *
 * Devuelve el puntero al Mapa mapeado o NULL si falla.
 */
static Mapa *abrir_shm_mapa(void)
{
    int fd;
    Mapa *mapa;

    fd = shm_open(SHM_MAPA_NAME, O_RDWR, 0666);
    if (fd == -1)
    {
        if (errno == ENOENT)
            fprintf(stderr, "nave: no se encontro la SHM '%s'. "
                            "El servidor no esta corriendo.\n",
                    SHM_MAPA_NAME);
        else
            perror("shm_open");
        return NULL;
    }

    mapa = mmap(NULL, sizeof(Mapa), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mapa == MAP_FAILED)
    {
        perror("mmap");
        return NULL;
    }

    return mapa;
}

/* ─── Registro con el servidor ────────────────────────────────────────── */

/*
 * Envia MsgRegistro a /cosmikernel_registro y espera la respuesta en
 * la cola privada /cosmikernel_nave_<pid>.
 * Devuelve el id asignado por el servidor, o -1 si el registro fallo.
 */
static int registrar_nave(void)
{
    mqd_t mq_registro, mq_propia;
    char nombre_propia[MQ_NAVE_NAME_LEN];
    MsgRegistro msg;
    MsgRegistroResp resp;

    memset(&msg, 0, sizeof(msg));
    msg.op = REG_OP_REGISTRAR;
    msg.tipo = CLIENTE_NAVE;
    msg.pid = getpid();
    msg.id = -1;
    snprintf(nombre_propia, sizeof(nombre_propia), MQ_NAVE_FMT, (int)getpid());
    snprintf(msg.cola_respuesta, sizeof(msg.cola_respuesta), "%s", nombre_propia);
    struct mq_attr attr;
    int id = -1;

    /* Cola privada para recibir la respuesta del servidor. */
    attr.mq_flags = 0;
    attr.mq_maxmsg = 4;
    attr.mq_msgsize = sizeof(MsgRegistroResp);
    attr.mq_curmsgs = 0;
    mq_propia = mq_open(nombre_propia, O_CREAT | O_RDONLY, 0666, &attr);
    if (mq_propia == (mqd_t)-1)
    {
        perror("mq_open(propia)");
        return -1;
    }

    /* Cola publica del servidor. */
    mq_registro = mq_open(MQ_REGISTRO_NAME, O_WRONLY);
    if (mq_registro == (mqd_t)-1)
    {
        /* Servidor no presente: retornamos -1 y el main abortara. */
        mq_close(mq_propia);
        mq_unlink(nombre_propia);
        return -1;
    }

    if (mq_send(mq_registro, (const char *)&msg, sizeof(msg), 0) == -1)
    {
        perror("mq_send(registro)");
        mq_close(mq_registro);
        mq_close(mq_propia);
        mq_unlink(nombre_propia);
        return -1;
    }
    mq_close(mq_registro);

    /* Esperamos la respuesta con timeout de 3s para no quedarnos colgados. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3;
    if (mq_timedreceive(mq_propia, (char *)&resp, sizeof(resp), NULL, &ts) == -1)
    {
        fprintf(stderr, "nave: sin respuesta del servidor en 3s\n");
    }
    else if (resp.error == 0)
    {
        id = resp.id;
    }
    else
    {
        fprintf(stderr, "nave: servidor rechazo el registro (error=%d)\n", resp.error);
    }

    mq_close(mq_propia);
    if (id == -1)
        mq_unlink(nombre_propia); /* registro fallido: no dejamos la cola colgada */
    /* Si el registro fue exitoso, la cola privada se deja viva mientras la
     * nave esta corriendo (otros hilos pueden necesitarla). Se eliminara en
     * el cleanup del main. */
    return id;
}

/* ─── Hilo soporte vital (task #20) ───────────────────────────────────── */

/*
 * Decrementa el oxigeno de la nave en la SHM cada `intervalo_seg` segundos.
 * Cuando el oxigeno llega a 0, marca la nave como DESACTIVADA (el servidor
 * actualizara la celda del mapa cuando detecte el cambio de estado).
 *
 * Nota: usa el mutex process-shared del mapa para sincronizar con el
 * servidor y los demas hilos.
 */
static void *hilo_soporte_vital(void *arg)
{
    VitalArgs *args = (VitalArgs *)arg;
    Mapa *mapa = args->mapa;
    int id = args->id_nave;

    while (!g_salir)
    {
        /* Dormimos 1 segundo a la vez para poder reaccionar rapido a g_salir. */
        int segundos_restantes = args->intervalo_seg;
        while (segundos_restantes > 0 && !g_salir)
        {
            sleep(1);
            segundos_restantes--;
        }
        if (g_salir)
            break;

        pthread_mutex_lock(&mapa->mutex);
        if (mapa->naves[id].oxigeno > 0)
        {
            mapa->naves[id].oxigeno--;
            if (mapa->naves[id].oxigeno == 0)
                mapa->naves[id].estado = ESTADO_DESACTIVADO;
        }
        pthread_mutex_unlock(&mapa->mutex);
    }
    return NULL;
}

chtype simbolo_nave(int direccion)
{
    switch (direccion)
    {
    case 0:
        return '^';
    case 1:
        return '>';
    case 2:
        return 'v';
    case 3:
        return '<';
    default:
        return '?';
    }
}

/* ─── Hilo radar (task #27) ───────────────────────────────────────────── */

/*
 * Dibuja el mapa y el panel de estado periodicamente.
 *
 * Diseno de la ventana:
 *   ┌──────── Mapa (24x80) ─────────┐ ┌─ Estado nave ──┐
 *   │                               │ │ Combustible    │
 *   │   .......*..........E......   │ │ Oxigeno        │
 *   │   .....@..........            │ │ Inventario     │
 *   │                               │ │  Deuterio  ... │
 *   │                               │ │  Mutexio   ... │
 *   │                               │ │  Semaforita... │
 *   │                               │ │  Kernelio  ... │
 *   └───────────────────────────────┘ └────────────────┘
 *
 * Toma el mutex de proceso compartido del mapa solo para copiar los datos
 * (asi no bloqueamos al servidor mientras dibujamos en pantalla).
 */
static void *hilo_radar(void *arg)
{
    RadarArgs *args = (RadarArgs *)arg;
    Mapa *mapa = args->mapa;
    int id = args->id_nave;

    /* Ventanas ncurses: la del mapa a la izquierda, la de estado a la derecha. */
    int alto_mapa = MAPA_FILAS + 2; /* +2 para el borde */
    int ancho_mapa = MAPA_COLS + 2;
    int alto_panel = MAPA_FILAS + 2;
    int ancho_panel = 28;

    WINDOW *win_mapa = newwin(alto_mapa, ancho_mapa, 0, 0);
    WINDOW *win_panel = newwin(alto_panel, ancho_panel, 0, ancho_mapa + 1);
    if (win_mapa == NULL || win_panel == NULL)
        return NULL;

    /* Para que el dibujo del mapa sea estable entre frames. */
    nodelay(win_mapa, TRUE);

    /* Copia local del mapa para dibujar fuera de la seccion critica. */
    Celda celdas_local[MAPA_FILAS][MAPA_COLS];
    Nave nave_local;

    while (!g_salir)
    {
        /* 1) Snapshot bajo mutex (lo mas corto posible). */
        pthread_mutex_lock(&mapa->mutex);
        memcpy(celdas_local, mapa->celdas, sizeof(celdas_local));
        nave_local = mapa->naves[id];
        pthread_mutex_unlock(&mapa->mutex);

        /* 2) Dibujar mapa. */
        werase(win_mapa);
        box(win_mapa, 0, 0);
        mvwprintw(win_mapa, 0, 2, " Mapa %dx%d ", MAPA_FILAS, MAPA_COLS);
        for (int f = 0; f < MAPA_FILAS; f++)
            for (int c = 0; c < MAPA_COLS; c++)
            {
                /* unsigned char para evitar warning de conversion de signo
                 * al pasar a mvwaddch (chtype = unsigned int). */
                unsigned char ch = (unsigned char)celdas_local[f][c].tipo;
                /* Resaltar la propia nave para distinguirla de otras. */
                if (celdas_local[f][c].tipo == CELDA_NAVE && celdas_local[f][c].idx == id)
                {

                    wattron(win_mapa, A_BOLD | A_REVERSE);
                    mvwaddch(win_mapa, f + 1, c + 1, simbolo_nave(nave_local.direccion));
                    wattroff(win_mapa, A_BOLD | A_REVERSE);
                }
                else
                {
                    mvwaddch(win_mapa, f + 1, c + 1, ch);
                }
            }

        /* 3) Dibujar panel de estado. */
        werase(win_panel);
        box(win_panel, 0, 0);
        mvwprintw(win_panel, 0, 2, " Nave #%d ", id);

        mvwprintw(win_panel, 1, 1, "PID:         %d", nave_local.pid);
        mvwprintw(win_panel, 2, 1, "Posicion:    (%d,%d)", nave_local.fila, nave_local.col);
        mvwprintw(win_panel, 3, 1, "Estado:      %s",
                  nave_local.estado == ESTADO_ACTIVO ? "ACTIVA" : "DESACTIVADA");

        mvwprintw(win_panel, 5, 1, "Combustible: %d", nave_local.combustible);
        mvwprintw(win_panel, 6, 1, "Oxigeno:     %d", nave_local.oxigeno);

        mvwprintw(win_panel, 8, 1, "-- Inventario --");
        mvwprintw(win_panel, 9, 1, " Deuterio:   %d", nave_local.deuterio);
        mvwprintw(win_panel, 10, 1, " Mutexio:    %d", nave_local.mutexio);
        mvwprintw(win_panel, 11, 1, " Semaforita: %d", nave_local.semaforita);
        mvwprintw(win_panel, 12, 1, " Kernelio:   %d", nave_local.kernelio);

        mvwprintw(win_panel, alto_panel - 2, 1, "Ctrl+C: salir");

        wnoutrefresh(win_mapa);
        wnoutrefresh(win_panel);
        doupdate();

        /* 4) Dormir refresh_ms ms. */
        struct timespec ts;
        ts.tv_sec = args->refresh_ms / 1000;
        ts.tv_nsec = (args->refresh_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }

    delwin(win_mapa);
    delwin(win_panel);
    return NULL;
}

static void *hilo_propulsion(void *arg)
{
    PropulsionArgs *args = (PropulsionArgs *)arg;
    Mapa *mapa = args->mapa;
    int id = args->id_nave;
    int ch;

    while (!g_salir)
    {
        bool mover = false;
        int df_mov = 0;
        int dc_mov = 0;

        ch = getch();

        switch (ch)
        {
        case 'a':
        case 'A':
        case KEY_LEFT:
            mapa->naves[id].direccion =
                (mapa->naves[id].direccion + 3) % 4;
            break;

        case 'd':
        case 'D':
        case KEY_RIGHT:
            mapa->naves[id].direccion =
                (mapa->naves[id].direccion + 1) % 4;
            break;

        case 'w':
        case 'W':
        case KEY_UP:
            df_mov = df[mapa->naves[id].direccion];
            dc_mov = dc[mapa->naves[id].direccion];
            mover = true;
            break;

        case 's':
        case 'S':
        case KEY_DOWN:
            df_mov = -df[mapa->naves[id].direccion];
            dc_mov = -dc[mapa->naves[id].direccion];
            mover = true;
            break;

        default:
            break;
        }

        if (mover)
        {
            int fila_actual = mapa->naves[id].fila;
            int col_actual = mapa->naves[id].col;

            int nueva_fila = fila_actual + df_mov;
            int nueva_col = col_actual + dc_mov;
            if (mapa->naves[id].combustible > 0)
            {

                ////// Bloque provisional //////
                sem_t *semaforo_celda = semaforo_celda_abrir(nueva_fila, nueva_col);

                if (semaforo_celda != NULL)
                {
                    sem_wait(semaforo_celda);

                    pthread_mutex_lock(&mapa->mutex);

                    mapa->naves[id].fila = nueva_fila;
                    mapa->naves[id].col = nueva_col;

                    mapa->celdas[nueva_fila][nueva_col].tipo = CELDA_NAVE;
                    mapa->celdas[nueva_fila][nueva_col].idx = id;

                    mapa->celdas[fila_actual][col_actual].tipo = CELDA_VACIA;
                    mapa->celdas[fila_actual][col_actual].idx = -1;

                    mapa->naves[id].combustible--;

                    if (mapa->naves[id].combustible == 0)
                        mapa->naves[id].estado = ESTADO_DESACTIVADO;

                    pthread_mutex_unlock(&mapa->mutex);

                    sem_post(semaforo_celda);

                    semaforo_celda_cerrar(semaforo_celda);
                }
                ////// Fin bloque provisional //////
            }
        }
        mover = false;

        struct timespec ts = {.tv_sec = 0, .tv_nsec = 16000000L};
        nanosleep(&ts, NULL);
    }

    return NULL;
}

/* ─── main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *config_path = (argc > 1) ? argv[1] : CONFIG_PATH;
    Config cfg;
    Mapa *mapa = NULL;
    int id_nave;
    pthread_t th_radar, th_vital, th_propulsion;
    RadarArgs radar_args;
    VitalArgs vital_args;
    PropulsionArgs propulsion_args;
    struct sigaction sa;
    char nombre_cola_propia[MQ_NAVE_NAME_LEN];

    /* 1) Configuracion. */
    if (config_load(config_path, &cfg) == -1)
        fprintf(stderr, "nave: arrancando con valores por defecto\n");

    /* 2) Handler SIGINT (antes de ncurses para que Ctrl+C funcione bien). */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manejar_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* sin SA_RESTART: queremos cortar nanosleep/sleep */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* 3) SHM del mapa (creada por el servidor; sin servidor abortamos). */
    mapa = abrir_shm_mapa();
    if (mapa == NULL)
        return EXIT_FAILURE; /* abrir_shm_mapa ya informo el error */

    /* 4) Registro con el servidor. Sin un id valido la nave no puede operar. */
    id_nave = registrar_nave();
    if (id_nave == -1)
    {
        fprintf(stderr, "nave: no se pudo registrar contra el servidor. Abortando.\n");
        munmap(mapa, sizeof(Mapa));
        return EXIT_FAILURE;
    }

    /* Inicializar campos de la nave en la SHM bajo el mutex. */
    pthread_mutex_lock(&mapa->mutex);
    mapa->naves[id_nave].combustible = NAVE_COMBUSTIBLE_INICIAL;
    mapa->naves[id_nave].oxigeno = NAVE_OXIGENO_INICIAL;
    mapa->naves[id_nave].pid = getpid();
    pthread_mutex_unlock(&mapa->mutex);

    snprintf(nombre_cola_propia, sizeof(nombre_cola_propia), MQ_NAVE_FMT, (int)getpid());

    /* 5) Inicializar ncurses. */
    initscr();
    cbreak();
    noecho();
    curs_set(0); /* ocultar cursor */
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    refresh();

    /* Aviso si la terminal es chica para el layout. */
    int filas, cols;
    getmaxyx(stdscr, filas, cols);
    if (filas < MAPA_FILAS + 2 || cols < MAPA_COLS + 2 + 28 + 1)
    {
        endwin();
        fprintf(stderr, "nave: terminal demasiado chica (necesita %dx%d, actual %dx%d)\n",
                MAPA_FILAS + 2, MAPA_COLS + 2 + 28 + 1, filas, cols);
        munmap(mapa, sizeof(Mapa));
        mq_unlink(nombre_cola_propia);
        return EXIT_FAILURE;
    }

    /* 6) Lanzar hilos. */
    radar_args.mapa = mapa;
    radar_args.id_nave = id_nave;
    radar_args.refresh_ms = cfg.radar_refresh_ms;
    if (pthread_create(&th_radar, NULL, hilo_radar, &radar_args) != 0)
    {
        endwin();
        perror("pthread_create(radar)");
        munmap(mapa, sizeof(Mapa));
        mq_unlink(nombre_cola_propia);
        return EXIT_FAILURE;
    }

    vital_args.mapa = mapa;
    vital_args.id_nave = id_nave;
    vital_args.intervalo_seg = cfg.intervalo_oxigeno_nave;
    if (pthread_create(&th_vital, NULL, hilo_soporte_vital, &vital_args) != 0)
    {
        g_salir = 1;
        pthread_join(th_radar, NULL);
        endwin();
        perror("pthread_create(soporte_vital)");
        munmap(mapa, sizeof(Mapa));
        mq_unlink(nombre_cola_propia);
        return EXIT_FAILURE;
    }

    propulsion_args.mapa = mapa;
    propulsion_args.id_nave = id_nave;
    if (pthread_create(&th_propulsion, NULL, hilo_propulsion, &propulsion_args) != 0)
    {
        g_salir = 1;
        pthread_join(th_radar, NULL);
        pthread_join(th_vital, NULL);
        endwin();
        perror("pthread_create(propulsion)");
        return EXIT_FAILURE;
    }

    /* 7) Esperar a que ambos hilos terminen (cuando g_salir = 1). */
    pthread_join(th_radar, NULL);
    pthread_join(th_vital, NULL);
    pthread_join(th_propulsion, NULL);

    /* 8) Cleanup ncurses. */
    endwin();

    /* 9) Cleanup IPC. La SHM la destruye el servidor, no la nave. */
    munmap(mapa, sizeof(Mapa));
    mq_unlink(nombre_cola_propia);

    printf("nave: saliendo limpiamente\n");
    return EXIT_SUCCESS;
}

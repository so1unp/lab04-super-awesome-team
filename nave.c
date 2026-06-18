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

/* ─── Estado global del proceso nave ──────────────────────────────────── */

/* Bandera de salida (se setea desde el handler de SIGINT). */
static volatile sig_atomic_t g_salir = 0;

/* Argumentos pasados al hilo radar. */
typedef struct {
    Mapa *mapa;
    int   id_nave;
    int   refresh_ms;
} RadarArgs;

/* Argumentos pasados al hilo de soporte vital. */
typedef struct {
    Mapa *mapa;
    int   id_nave;
    int   intervalo_seg;
} VitalArgs;

/*
 * Estado de la ultima alerta de combustible recibida (task #30).
 * Lo escribe el hilo de alertas y lo lee el hilo radar para mostrarlo.
 * Como es estado LOCAL del proceso (no de la SHM), usa su propio mutex.
 */
typedef struct {
    pthread_mutex_t mutex;
    int hay_alerta;     /* 0 hasta recibir la primera */
    int id_estacion;    /* -1 si la estacion no esta registrada */
    int pid_estacion;
    int combustible;    /* combustible reportado por la estacion */
    int total;          /* cantidad total de alertas recibidas */
} EstadoAlerta;

static EstadoAlerta g_alerta;  /* inicializado en main (mutex) y a cero por ser static */

/* Argumentos del hilo de alertas: nombre de la cola privada de la nave. */
typedef struct {
    char cola[MQ_NAVE_NAME_LEN];
} AlertaArgs;

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
                            "El servidor no esta corriendo.\n", SHM_MAPA_NAME);
        else
            perror("shm_open");
        return NULL;
    }

    mapa = mmap(NULL, sizeof(Mapa), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mapa == MAP_FAILED) { perror("mmap"); return NULL; }

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
    msg.op   = REG_OP_REGISTRAR;
    msg.tipo = CLIENTE_NAVE;
    msg.pid  = getpid();
    msg.id   = -1;
    snprintf(nombre_propia, sizeof(nombre_propia), MQ_NAVE_FMT, (int)getpid());
    snprintf(msg.cola_respuesta, sizeof(msg.cola_respuesta), "%s", nombre_propia);
    struct mq_attr attr;
    int id = -1;

    /* Cola privada para recibir la respuesta del servidor. */
    attr.mq_flags = 0; attr.mq_maxmsg = 4;
    attr.mq_msgsize = sizeof(MsgRegistroResp); attr.mq_curmsgs = 0;
    mq_propia = mq_open(nombre_propia, O_CREAT | O_RDONLY, 0666, &attr);
    if (mq_propia == (mqd_t)-1) { perror("mq_open(propia)"); return -1; }

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
        mq_close(mq_registro); mq_close(mq_propia); mq_unlink(nombre_propia);
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
        mq_unlink(nombre_propia);   /* registro fallido: no dejamos la cola colgada */
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
        if (g_salir) break;

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

/* ─── Hilo de alertas de combustible (task #30) ───────────────────────── */

/*
 * Escucha la cola privada de la nave (/cosmikernel_nave_<pid>, ya creada en
 * registrar_nave) esperando mensajes MsgAlertaCombustible que envian las
 * estaciones cuando se quedan sin combustible y piden deuterio.
 *
 * La cola POSIX bufferiza los mensajes, asi que mientras este hilo lea no se
 * pierde ninguno (criterio de aceptacion). La ultima alerta y el total se
 * guardan en g_alerta para que el hilo radar los muestre en el panel.
 *
 * Nota: la cola se creo con mq_msgsize = sizeof(MsgRegistroResp) (la respuesta
 * del registro). Como MsgAlertaCombustible es mas chico, entra sin problema;
 * leemos con un buffer del tamano real de la cola (mq_getattr) para no fallar
 * con EMSGSIZE.
 */
static void *hilo_alertas(void *arg)
{
    AlertaArgs *args = (AlertaArgs *)arg;
    mqd_t mq;
    struct mq_attr attr;
    char *buf;

    mq = mq_open(args->cola, O_RDONLY);
    if (mq == (mqd_t)-1)
        return NULL;  /* sin cola no hay alertas; no es fatal para la nave */

    if (mq_getattr(mq, &attr) == -1) { mq_close(mq); return NULL; }
    buf = malloc((size_t)attr.mq_msgsize);
    if (buf == NULL) { mq_close(mq); return NULL; }

    while (!g_salir)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;  /* timeout 1s para poder revisar g_salir */

        ssize_t n = mq_timedreceive(mq, buf, (size_t)attr.mq_msgsize, NULL, &ts);
        if (n < 0)
            continue;  /* ETIMEDOUT / EINTR: reintentar */

        if ((size_t)n >= sizeof(MsgAlertaCombustible))
        {
            MsgAlertaCombustible al;
            memcpy(&al, buf, sizeof(al));

            pthread_mutex_lock(&g_alerta.mutex);
            g_alerta.hay_alerta   = 1;
            g_alerta.id_estacion  = al.id_estacion;
            g_alerta.pid_estacion = al.pid_estacion;
            g_alerta.combustible  = al.combustible_actual;
            g_alerta.total++;
            pthread_mutex_unlock(&g_alerta.mutex);
        }
    }

    free(buf);
    mq_close(mq);
    return NULL;
}

/* ─── Hilo radar (task #27) ───────────────────────────────────────────── */

/*
 * Dibuja el mapa y el panel de estado periodicamente.
 *
 * Diseno de la ventana:
 *   ┌──────── Mapa (24x80) ─────────┐ ┌─ Estado nave ──┐
 *   │                                │ │ Combustible    │
 *   │   .......*..........E......    │ │ Oxigeno        │
 *   │   .....@..........            │ │ Inventario     │
 *   │                                │ │  Deuterio  ... │
 *   │                                │ │  Mutexio   ... │
 *   │                                │ │  Semaforita... │
 *   │                                │ │  Kernelio  ... │
 *   └────────────────────────────────┘ └────────────────┘
 *
 * Toma el mutex de proceso compartido del mapa solo para copiar los datos
 * (asi no bloqueamos al servidor mientras dibujamos en pantalla).
 */
static void *hilo_radar(void *arg)
{
    RadarArgs *args = (RadarArgs *)arg;
    Mapa *mapa = args->mapa;
    int id = args->id_nave;

    /* Ventanas ncurses: el recuadro "Nave #N" a la izquierda (que contiene
     * arriba las barras de combustible/oxigeno y debajo el mapa), y el panel
     * de estado a la derecha.
     *
     * Layout del recuadro izquierdo:
     *   fila 0      : borde + titulo " Nave #N "
     *   fila 1      : (en blanco)
     *   fila 2      : barra de combustible
     *   fila 3      : barra de oxigeno
     *   fila 4..    : el mapa
     *   ultima fila : borde inferior
     */
    int barra_off_y = 2;                          /* fila de la 1ra barra (fila 1 en blanco) */
    int mapa_off_y  = barra_off_y + 2;            /* blanco + 2 barras => mapa en fila 4 */
    int mapa_off_x  = 1;                          /* borde izquierdo */
    int alto_mapa   = mapa_off_y + MAPA_FILAS + 1;   /* mapa + borde inferior */
    int ancho_mapa  = MAPA_COLS  + 2;
    int alto_panel  = alto_mapa;                  /* misma altura para alinear */
    int ancho_panel = 28;

    WINDOW *win_mapa  = newwin(alto_mapa,  ancho_mapa,  0, 0);
    WINDOW *win_panel = newwin(alto_panel, ancho_panel, 0, ancho_mapa + 1);
    if (win_mapa == NULL || win_panel == NULL)
        return NULL;

    /* Para que el dibujo del mapa sea estable entre frames. */
    nodelay(win_mapa, TRUE);

    /* Copia local del mapa para dibujar fuera de la seccion critica. */
    Celda celdas_local[MAPA_FILAS][MAPA_COLS];
    Nave  nave_local;
    Nave  naves_local[MAX_NAVES];
    Estacion estaciones_local[MAX_ESTACIONES];

    while (!g_salir)
    {
        /* 1) Snapshot bajo mutex (lo mas corto posible). */
        pthread_mutex_lock(&mapa->mutex);
        memcpy(celdas_local, mapa->celdas, sizeof(celdas_local));
        memcpy(naves_local, mapa->naves, sizeof(naves_local));
        memcpy(estaciones_local, mapa->estaciones, sizeof(estaciones_local));
        nave_local = naves_local[id];
        pthread_mutex_unlock(&mapa->mutex);

        /* 2) Dibujar el recuadro "Nave #N": titulo, barras y mapa. */
        werase(win_mapa);
        box(win_mapa, 0, 0);
        mvwprintw(win_mapa, 0, 2, " Nave #%d ", id);

        /* 2a) Barras de combustible y oxigeno dentro del recuadro, debajo del
         * titulo. Muestran el valor numerico antes de la barra de '='. Ambas
         * barras arrancan en la misma columna (etiqueta+numero de ancho fijo).
         * El valor (0-100) se escala al ancho disponible del recuadro. */
        {
            int prefijo_w = 17;   /* longitud de "Combustible: 100 " */
            int ancho_barra = (ancho_mapa - 2) - prefijo_w;
            if (ancho_barra < 1) ancho_barra = 1;

            int comb = nave_local.combustible;
            int oxi  = nave_local.oxigeno;
            if (comb < 0) comb = 0; else if (comb > 100) comb = 100;
            if (oxi  < 0) oxi  = 0; else if (oxi  > 100) oxi  = 100;

            mvwprintw(win_mapa, barra_off_y, 1, "Combustible: %3d ", comb);
            mvwhline(win_mapa, barra_off_y, 1 + prefijo_w, '=', comb * ancho_barra / 100);
            mvwprintw(win_mapa, barra_off_y + 1, 1, "Oxigeno:     %3d ", oxi);
            mvwhline(win_mapa, barra_off_y + 1, 1 + prefijo_w, '=', oxi * ancho_barra / 100);
        }

        /* 2b) Dibujar el mapa (con offset por el titulo + barras). */
        for (int f = 0; f < MAPA_FILAS; f++)
            for (int c = 0; c < MAPA_COLS; c++)
            {
                /* El valor del enum TipoCelda ya es el simbolo a dibujar
                 * (definido en tipos.h, comun a todos los procesos).
                 * unsigned char para evitar warning de conversion de signo. */
                unsigned char ch = (unsigned char)celdas_local[f][c].tipo;
                /* Resaltar la propia nave para distinguirla de otras. */
                if (celdas_local[f][c].tipo == CELDA_NAVE && celdas_local[f][c].idx == id)
                {
                    wattron(win_mapa, A_BOLD | A_REVERSE);
                    mvwaddch(win_mapa, mapa_off_y + f, mapa_off_x + c, ch);
                    wattroff(win_mapa, A_BOLD | A_REVERSE);
                }
                else
                {
                    mvwaddch(win_mapa, mapa_off_y + f, mapa_off_x + c, ch);
                }
            }

        /* 2c) Etiqueta de deuterio al lado de cada estacion (task #30).
         * El combustible de la estacion ES deuterio (ver README). Lo dibujamos
         * a la derecha del '#' con un espacio de separacion; ncurses recorta
         * si llega al borde. */
        for (int e = 0; e < MAX_ESTACIONES; e++)
        {
            if (estaciones_local[e].pid != 0)
            {
                int ef = estaciones_local[e].fila;
                int ec = estaciones_local[e].col;
                if (ef >= 0 && ef < MAPA_FILAS && ec >= 0 && ec < MAPA_COLS)
                {
                    wattron(win_mapa, A_BOLD);
                    mvwprintw(win_mapa, mapa_off_y + ef, mapa_off_x + ec + 1, " Deut: %d",
                              estaciones_local[e].combustible);
                    wattroff(win_mapa, A_BOLD);
                }
            }
        }

        /* 2d) Etiqueta de combustible (deuterio) y oxigeno al lado de cada nave.
         * Se dibuja a la derecha del '^': "D: <comb>" y debajo "O: <oxig>". */
        for (int nv = 0; nv < MAX_NAVES; nv++)
        {
            if (naves_local[nv].pid != 0)
            {
                int nf = naves_local[nv].fila;
                int nc = naves_local[nv].col;
                if (nf >= 0 && nf < MAPA_FILAS && nc >= 0 && nc < MAPA_COLS)
                {
                    /* La O va una fila abajo; si la nave esta en la ultima
                     * fila, la ponemos una fila arriba para no pisar el borde. */
                    int fila_d = mapa_off_y + nf;
                    int fila_o = (nf + 1 < MAPA_FILAS) ? fila_d + 1 : fila_d - 1;
                    int col_lbl = mapa_off_x + nc + 1;
                    wattron(win_mapa, A_BOLD);
                    mvwprintw(win_mapa, fila_d, col_lbl, " D: %d", naves_local[nv].combustible);
                    mvwprintw(win_mapa, fila_o, col_lbl, " O: %d", naves_local[nv].oxigeno);
                    wattroff(win_mapa, A_BOLD);
                }
            }
        }

        /* 3) Dibujar panel de estado.
         *    Layout: inventario (dinero + recursos) arriba, linea separadora
         *    en el medio, eventos/alertas SOS debajo, identificacion al pie.
         *    Combustible y oxigeno NO van aca: se muestran como barras arriba. */
        werase(win_panel);
        box(win_panel, 0, 0);
        mvwprintw(win_panel, 0, 2, " Inventario ");

        /* --- Arriba: inventario (dinero + recursos recolectados) --- */
        /* (linea 1 en blanco) */
        mvwprintw(win_panel, 2, 1, " $0");   /* TODO: dinero (sistema de transacciones futuro) */
        /* (linea 3 en blanco) */
        mvwprintw(win_panel, 4, 1, " Deuterio:   %d", nave_local.deuterio);
        mvwprintw(win_panel, 5, 1, " Mutexio:    %d", nave_local.mutexio);
        mvwprintw(win_panel, 6, 1, " Semaforita: %d", nave_local.semaforita);
        mvwprintw(win_panel, 7, 1, " Kernelio:   %d", nave_local.kernelio);

        /* --- Linea separadora --- */
        mvwhline(win_panel, 9, 1, ACS_HLINE, ancho_panel - 2);

        /* --- Debajo: eventos / alertas SOS (task #30) --- */
        pthread_mutex_lock(&g_alerta.mutex);
        int al_hay  = g_alerta.hay_alerta;
        int al_id   = g_alerta.id_estacion;
        int al_pid  = g_alerta.pid_estacion;
        int al_comb = g_alerta.combustible;
        int al_tot  = g_alerta.total;
        pthread_mutex_unlock(&g_alerta.mutex);

        mvwprintw(win_panel, 11, 1, "Eventos / Alertas SOS");
        /* (linea 12 en blanco entre el titulo y el contenido) */
        if (al_hay)
        {
            wattron(win_panel, A_BOLD);
            if (al_id >= 0)
                mvwprintw(win_panel, 13, 1, "Estacion #%d pide", al_id);
            else
                mvwprintw(win_panel, 13, 1, "Estacion(pid %d)", al_pid);
            mvwprintw(win_panel, 14, 1, "DEUTERIO! comb=%d", al_comb);
            wattroff(win_panel, A_BOLD);
            mvwprintw(win_panel, 15, 1, "recibidas: %d", al_tot);
        }
        else
        {
            mvwprintw(win_panel, 13, 1, "(sin alertas)");
        }

        /* --- Al pie: identificacion de la nave --- */
        mvwprintw(win_panel, alto_panel - 4, 1, "PID:      %d", nave_local.pid);
        mvwprintw(win_panel, alto_panel - 3, 1, "Posicion: (%d,%d)",
                  nave_local.fila, nave_local.col);
        mvwprintw(win_panel, alto_panel - 2, 1, "Estado:   %s",
                  nave_local.estado == ESTADO_ACTIVO ? "ACTIVA" : "DESACTIVADA");

        wnoutrefresh(win_mapa);
        wnoutrefresh(win_panel);
        doupdate();

        /* 4) Dormir refresh_ms ms. */
        struct timespec ts;
        ts.tv_sec  = args->refresh_ms / 1000;
        ts.tv_nsec = (args->refresh_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }

    delwin(win_mapa);
    delwin(win_panel);
    return NULL;
}

/* ─── main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *config_path = (argc > 1) ? argv[1] : CONFIG_PATH;
    Config cfg;
    Mapa *mapa = NULL;
    int id_nave;
    pthread_t th_radar, th_vital, th_alertas;
    RadarArgs radar_args;
    VitalArgs vital_args;
    AlertaArgs alerta_args;
    struct sigaction sa;
    char nombre_cola_propia[MQ_NAVE_NAME_LEN];

    /* 1) Configuracion. */
    if (config_load(config_path, &cfg) == -1)
        fprintf(stderr, "nave: arrancando con valores por defecto\n");

    /* 2) Handler SIGINT (antes de ncurses para que Ctrl+C funcione bien). */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manejar_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* sin SA_RESTART: queremos cortar nanosleep/sleep */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* 3) SHM del mapa (creada por el servidor; sin servidor abortamos). */
    mapa = abrir_shm_mapa();
    if (mapa == NULL) return EXIT_FAILURE;  /* abrir_shm_mapa ya informo el error */

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
    mapa->naves[id_nave].oxigeno     = NAVE_OXIGENO_INICIAL;
    mapa->naves[id_nave].pid         = getpid();
    pthread_mutex_unlock(&mapa->mutex);

    snprintf(nombre_cola_propia, sizeof(nombre_cola_propia), MQ_NAVE_FMT, (int)getpid());

    /* Estado de alertas (task #30): mutex local + nombre de cola para el hilo. */
    pthread_mutex_init(&g_alerta.mutex, NULL);
    g_alerta.id_estacion = -1;
    snprintf(alerta_args.cola, sizeof(alerta_args.cola), "%s", nombre_cola_propia);

    /* 5) Inicializar ncurses. */
    initscr();
    cbreak();
    noecho();
    curs_set(0);              /* ocultar cursor */
    keypad(stdscr, TRUE);
    refresh();

    /* Aviso si la terminal es chica para el layout. */
    int filas, cols;
    getmaxyx(stdscr, filas, cols);
    if (filas < MAPA_FILAS + 5 || cols < MAPA_COLS + 2 + 28 + 1)
    {
        endwin();
        fprintf(stderr, "nave: terminal demasiado chica (necesita %dx%d, actual %dx%d)\n",
                MAPA_FILAS + 5, MAPA_COLS + 2 + 28 + 1, filas, cols);
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

    /* Hilo de alertas de combustible (task #30). */
    if (pthread_create(&th_alertas, NULL, hilo_alertas, &alerta_args) != 0)
    {
        g_salir = 1;
        pthread_join(th_radar, NULL);
        pthread_join(th_vital, NULL);
        endwin();
        perror("pthread_create(alertas)");
        munmap(mapa, sizeof(Mapa));
        mq_unlink(nombre_cola_propia);
        return EXIT_FAILURE;
    }

    /* 7) Esperar a que los hilos terminen (cuando g_salir = 1). */
    pthread_join(th_radar, NULL);
    pthread_join(th_vital, NULL);
    pthread_join(th_alertas, NULL);

    /* 8) Cleanup ncurses. */
    endwin();

    /* 9) Cleanup IPC. La SHM la destruye el servidor, no la nave. */
    munmap(mapa, sizeof(Mapa));
    mq_unlink(nombre_cola_propia);

    printf("nave: saliendo limpiamente\n");
    return EXIT_SUCCESS;
}

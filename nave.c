/*
 * nave.c - Proceso cliente "nave espacial".
 *
 * Implementa los hilos:
 *   - radar (task #27): lee la SHM y dibuja mapa + panel de estado con ncurses.
 *   - soporte vital (task #20): decrementa periodicamente el oxigeno de la nave
 *     en la SHM (mapa->naves[id].oxigeno).
 *   - alertas (task #30): recibe avisos de combustible bajo de las estaciones
 *     y los muestra en el panel.
 *   - propulsion (task #19): mueve la nave con el teclado, usando los semaforos
 *     de celda (task #29) para que dos naves no ocupen la misma celda.
 *   - extraccion (task #21): con la tecla 'e' extrae los recursos del asteroide
 *     que la nave tiene de frente (gasta combustible) y avisa al servidor.
 *   - hangar / transacciones (task #44): al empujar contra una estacion la nave
 *     entra a su hangar (semaforo contador) y con f/o/1-4 compra/vende.
 *
 * Flujo del main:
 *   1. Carga config.txt (radar_refresh_ms, intervalo_oxigeno_nave).
 *   2. Abre la SHM /cosmikernel_mapa creada por el servidor. El servidor
 *      debe estar corriendo; si la SHM no existe, la nave aborta con error.
 *   3. Se registra contra el servidor via MQ_REGISTRO_NAME para obtener su
 *      id en Mapa.naves[]. Si el registro falla, aborta con error.
 *   4. Inicializa ncurses, instala handler de SIGINT.
 *   5. Lanza los hilos radar, soporte_vital, alertas y propulsion.
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
    char cola_trx[MQ_NAVE_TRX_NAME_LEN]; /* cola para respuestas de transaccion (#44) */
} PropulsionArgs;

/* Argumentos pasados al hilo de extraccion. */
typedef struct
{
    Mapa *mapa;
    int id_nave;
} ExtraccionArgs;

/* Desplazamientos por direccion: 0=arriba, 1=derecha, 2=abajo, 3=izquierda. */
static const int df[4] = {-1, 0, 1, 0};
static const int dc[4] = {0, 1, 0, -1};

/*
 * Bandera de "solicitud de extraccion" (task #21). El hilo de propulsion la
 * pone en 1 cuando el jugador presiona la tecla de accion; el hilo de
 * extraccion la consume. Asi un solo hilo (propulsion) lee el teclado.
 */
static volatile sig_atomic_t g_extraer = 0;

/* Combustible que gasta la nave en cada extraccion. */
#define EXTRACCION_COSTO_COMBUSTIBLE 5

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

/*
 * Estado del hangar en que se encuentra la nave actualmente (task #44).
 * El hilo de propulsion lo escribe; el hilo radar lo lee para mostrarlo.
 * Como es estado LOCAL del proceso, usa su propio mutex.
 */
typedef struct {
    pthread_mutex_t mutex;
    int id_estacion;   /* -1 = fuera del hangar */
    sem_t *sem;        /* handle del semaforo del hangar abierto, NULL si no esta */
    char msg[64];      /* ultimo mensaje de evento del hangar */
} EstadoHangar;

static EstadoHangar g_hangar;

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

/* ─── Simbolo de la nave segun su direccion (task #19) ─────────────────── */

/*
 * Devuelve el caracter con que se dibuja una nave segun hacia donde apunta:
 * 0=arriba '^', 1=derecha '>', 2=abajo 'v', 3=izquierda '<'.
 */
static chtype simbolo_nave(int direccion)
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
        return '^';
    }
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
 * Layout del recuadro izquierdo "Nave #N":
 *   fila 0      : borde + titulo " Nave #N "
 *   fila 1      : (en blanco)
 *   fila 2      : barra de combustible
 *   fila 3      : barra de oxigeno
 *   fila 4..    : el mapa
 *   ultima fila : borde inferior
 *
 * Toma el mutex de proceso compartido del mapa solo para copiar los datos
 * (asi no bloqueamos al servidor mientras dibujamos en pantalla).
 */
static void *hilo_radar(void *arg)
{
    RadarArgs *args = (RadarArgs *)arg;
    Mapa *mapa = args->mapa;
    int id = args->id_nave;

    int barra_off_y = 2;                          /* fila de la 1ra barra (fila 1 en blanco) */
    int mapa_off_y  = barra_off_y + 2;            /* blanco + 2 barras => mapa en fila 4 */
    int mapa_off_x  = 1;                          /* borde izquierdo */
    int alto_mapa   = mapa_off_y + MAPA_FILAS + 1;   /* mapa + borde inferior */
    int ancho_mapa  = MAPA_COLS  + 2;
    int alto_panel  = alto_mapa;                  /* misma altura para alinear */
    int ancho_panel = 28;

    WINDOW *win_mapa = newwin(alto_mapa, ancho_mapa, 0, 0);
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

        /* 2b) Dibujar el mapa (con offset por el titulo + barras).
         * Las naves se dibujan segun su direccion (^ > v <, task #19); la
         * propia nave ademas se resalta. El resto de celdas usa el simbolo
         * del enum TipoCelda (definido en tipos.h, comun a todos). */
        for (int f = 0; f < MAPA_FILAS; f++)
            for (int c = 0; c < MAPA_COLS; c++)
            {
                if (celdas_local[f][c].tipo == CELDA_NAVE)
                {
                    int idx_nave = celdas_local[f][c].idx;
                    chtype simbolo = (idx_nave >= 0 && idx_nave < MAX_NAVES)
                                         ? simbolo_nave(naves_local[idx_nave].direccion)
                                         : (chtype)CELDA_NAVE;
                    if (idx_nave == id)
                    {
                        wattron(win_mapa, A_BOLD | A_REVERSE);
                        mvwaddch(win_mapa, mapa_off_y + f, mapa_off_x + c, simbolo);
                        wattroff(win_mapa, A_BOLD | A_REVERSE);
                    }
                    else
                    {
                        mvwaddch(win_mapa, mapa_off_y + f, mapa_off_x + c, simbolo);
                    }
                }
                else
                {
                    unsigned char ch = (unsigned char)celdas_local[f][c].tipo;
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
         * Se dibuja a la derecha de la nave: "D: <comb>" y debajo "O: <oxig>". */
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
        mvwprintw(win_panel, 2, 1, " $%-12d", nave_local.dinero);   /* creditos de la nave (#44) */
        /* (linea 3 en blanco) */
        mvwprintw(win_panel, 4, 1, " Deuterio:   %d", nave_local.deuterio);
        mvwprintw(win_panel, 5, 1, " Mutexio:    %d", nave_local.mutexio);
        mvwprintw(win_panel, 6, 1, " Semaforita: %d", nave_local.semaforita);
        mvwprintw(win_panel, 7, 1, " Kernelio:   %d", nave_local.kernelio);

        /* --- Linea separadora --- */
        mvwhline(win_panel, 9, 1, ACS_HLINE, ancho_panel - 2);

        /* --- Debajo: si estamos en un hangar, opciones de transaccion (#44);
         *     si no, eventos / alertas SOS (#30). --- */
        pthread_mutex_lock(&g_hangar.mutex);
        int h_id = g_hangar.id_estacion;
        char h_msg[64];
        snprintf(h_msg, sizeof(h_msg), "%s", g_hangar.msg);
        pthread_mutex_unlock(&g_hangar.mutex);

        if (h_id >= 0)
        {
            /* En hangar: mostrar las teclas de compra/venta. */
            wattron(win_panel, A_BOLD);
            mvwprintw(win_panel, 11, 1, "** HANGAR #%d **", h_id);
            wattroff(win_panel, A_BOLD);
            mvwprintw(win_panel, 12, 1, "f=Combustible(+10)");
            mvwprintw(win_panel, 13, 1, "o=Oxigeno    (+10)");
            mvwprintw(win_panel, 14, 1, "1=D 2=M 3=S 4=K");
            mvwprintw(win_panel, 15, 1, "%-24s", h_msg);
        }
        else
        {
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
                if (h_msg[0] != '\0')
                    mvwprintw(win_panel, 14, 1, "%-24s", h_msg);
            }
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
        ts.tv_sec = args->refresh_ms / 1000;
        ts.tv_nsec = (args->refresh_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }

    delwin(win_mapa);
    delwin(win_panel);
    return NULL;
}

/* ─── Hilo de extraccion de recursos (task #21) ───────────────────────── */

/*
 * Notifica al servidor que el asteroide `idx` quedo agotado, para que lo
 * elimine del mapa (REG_OP_DESACTIVAR_ASTEROIDE). No espera respuesta.
 */
static void notificar_asteroide_desactivado(int idx)
{
    mqd_t mq = mq_open(MQ_REGISTRO_NAME, O_WRONLY);
    if (mq == (mqd_t)-1)
        return;

    MsgRegistro msg;
    memset(&msg, 0, sizeof(msg));
    msg.op  = REG_OP_DESACTIVAR_ASTEROIDE;
    msg.id  = idx;            /* indice en mapa->asteroides[] */
    msg.pid = getpid();
    mq_send(mq, (const char *)&msg, sizeof(msg), 0);
    mq_close(mq);
}

/*
 * Hilo de extraccion (task #21). Se activa cuando el hilo de propulsion levanta
 * la bandera g_extraer (al presionar la tecla de accion). Extrae TODO el
 * contenido del asteroide que la nave tiene de frente (segun su direccion):
 *   - suma los 4 minerales del asteroide al inventario de la nave,
 *   - gasta combustible (EXTRACCION_COSTO_COMBUSTIBLE),
 *   - vacia y desactiva el asteroide en la SHM (bajo el mutex),
 *   - notifica al servidor para que lo elimine del mapa.
 */
static void *hilo_extraccion(void *arg)
{
    ExtraccionArgs *args = (ExtraccionArgs *)arg;
    Mapa *mapa = args->mapa;
    int id = args->id_nave;

    while (!g_salir)
    {
        if (g_extraer)
        {
            g_extraer = 0;             /* consumir la solicitud */
            int idx_ast = -1;          /* asteroide a eliminar, si se vacio */

            pthread_mutex_lock(&mapa->mutex);

            /* Celda de enfrente segun la direccion de la nave. */
            int dir = mapa->naves[id].direccion;
            int ef = mapa->naves[id].fila + df[dir];
            int ec = mapa->naves[id].col + dc[dir];

            if (ef >= 0 && ef < MAPA_FILAS && ec >= 0 && ec < MAPA_COLS &&
                mapa->celdas[ef][ec].tipo == CELDA_ASTEROIDE &&
                mapa->naves[id].combustible > 0)
            {
                int a = mapa->celdas[ef][ec].idx;  /* indice del asteroide */
                if (a >= 0 && a < MAX_ASTEROIDES &&
                    mapa->asteroides[a].estado == ESTADO_ACTIVO)
                {
                    /* Extraer todo el contenido al inventario de la nave. */
                    mapa->naves[id].deuterio   += mapa->asteroides[a].deuterio;
                    mapa->naves[id].mutexio    += mapa->asteroides[a].mutexio;
                    mapa->naves[id].semaforita += mapa->asteroides[a].semaforita;
                    mapa->naves[id].kernelio   += mapa->asteroides[a].kernelio;

                    mapa->asteroides[a].deuterio = 0;
                    mapa->asteroides[a].mutexio = 0;
                    mapa->asteroides[a].semaforita = 0;
                    mapa->asteroides[a].kernelio = 0;

                    /* Gastar combustible por la extraccion. */
                    mapa->naves[id].combustible -= EXTRACCION_COSTO_COMBUSTIBLE;
                    if (mapa->naves[id].combustible <= 0)
                    {
                        mapa->naves[id].combustible = 0;
                        mapa->naves[id].estado = ESTADO_DESACTIVADO;
                    }

                    /* El asteroide quedo vacio: lo desactivamos y avisamos. */
                    mapa->asteroides[a].estado = ESTADO_DESACTIVADO;
                    idx_ast = a;
                }
            }

            pthread_mutex_unlock(&mapa->mutex);

            if (idx_ast >= 0)
                notificar_asteroide_desactivado(idx_ast);
        }

        /* Poll cada 50ms para no quemar CPU. */
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000000L};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ─── Helpers de hangar / transacciones (task #44) ────────────────────── */

/*
 * Libera el hangar que la nave tiene adquirido:
 *   - Resetea g_hangar (estado local).
 *   - Borra el PID de la nave de Estacion.hangar[] en la SHM.
 *   - Hace sem_post + sem_close al semaforo del hangar.
 * Seguro de llamar aunque la nave no este en ningun hangar (no-op).
 */
static void salir_hangar(Mapa *mapa)
{
    pthread_mutex_lock(&g_hangar.mutex);
    if (g_hangar.id_estacion < 0)
    {
        pthread_mutex_unlock(&g_hangar.mutex);
        return;
    }
    int id_est = g_hangar.id_estacion;
    sem_t *sem = g_hangar.sem;
    g_hangar.id_estacion = -1;
    g_hangar.sem = NULL;
    snprintf(g_hangar.msg, sizeof(g_hangar.msg), "Hangar liberado");
    pthread_mutex_unlock(&g_hangar.mutex);

    /* Borrar nuestro PID de la lista de ocupantes en la SHM. */
    pthread_mutex_lock(&mapa->mutex);
    for (int i = 0; i < 3; i++)
    {
        if (mapa->estaciones[id_est].hangar[i] == (int)getpid())
        {
            mapa->estaciones[id_est].hangar[i] = 0;
            break;
        }
    }
    pthread_mutex_unlock(&mapa->mutex);

    if (sem != NULL)
    {
        sem_post(sem);
        sem_close(sem);
    }
}

/*
 * Envia una transaccion a la cola de la estacion y espera la respuesta en
 * cola_trx (la cola dedicada de la nave, MQ_NAVE_TRX_FMT por su pid). Si tiene
 * exito, actualiza el inventario de la nave en la SHM y escribe el resultado
 * en g_hangar.msg para que el radar lo muestre en el panel.
 */
static void enviar_transaccion(Mapa *mapa, int id_nave, int id_estacion,
                               TipoOperacion op, int cantidad,
                               const char *cola_trx)
{
    char nombre_mq[MQ_ESTACION_NAME_LEN];
    snprintf(nombre_mq, sizeof(nombre_mq), MQ_ESTACION_FMT, id_estacion);

    mqd_t mq = mq_open(nombre_mq, O_WRONLY);
    if (mq == (mqd_t)-1)
    {
        pthread_mutex_lock(&g_hangar.mutex);
        snprintf(g_hangar.msg, sizeof(g_hangar.msg), "Sin conexion estacion");
        pthread_mutex_unlock(&g_hangar.mutex);
        return;
    }

    MsgTransaccion msg;
    memset(&msg, 0, sizeof(msg));
    msg.operacion = op;
    msg.cantidad = cantidad;
    msg.pid_nave = getpid();
    msg.id_nave = id_nave;
    msg.dinero_nave = mapa->naves[id_nave].dinero; /* para que la estacion valide la compra */
    /* La estacion responde a MQ_NAVE_TRX_FMT(pid_nave) == cola_trx. */

    if (mq_send(mq, (const char *)&msg, sizeof(msg), 0) == -1)
    {
        mq_close(mq);
        return;
    }
    mq_close(mq);

    /* Leer la respuesta de la cola dedicada con timeout de 2s. */
    mqd_t mq_resp = mq_open(cola_trx, O_RDONLY);
    if (mq_resp == (mqd_t)-1)
        return;

    MsgTransaccionResp resp;
    char buf[sizeof(MsgTransaccionResp) + 16]; /* margen de seguridad */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;
    ssize_t n = mq_timedreceive(mq_resp, buf, sizeof(buf), NULL, &ts);
    mq_close(mq_resp);

    if (n <= 0)
        return;

    memcpy(&resp, buf, sizeof(resp));

    if (resp.error == 0)
    {
        pthread_mutex_lock(&mapa->mutex);
        switch (op)
        {
        case OP_COMPRAR_COMBUSTIBLE:
            mapa->naves[id_nave].combustible += resp.cantidad_efectiva;
            break;
        case OP_COMPRAR_OXIGENO:
            mapa->naves[id_nave].oxigeno += resp.cantidad_efectiva;
            break;
        case OP_VENDER_DEUTERIO:
            mapa->naves[id_nave].deuterio -= resp.cantidad_efectiva;
            break;
        case OP_VENDER_MUTEXIO:
            mapa->naves[id_nave].mutexio -= resp.cantidad_efectiva;
            break;
        case OP_VENDER_SEMAFORITA:
            mapa->naves[id_nave].semaforita -= resp.cantidad_efectiva;
            break;
        case OP_VENDER_KERNELIO:
            mapa->naves[id_nave].kernelio -= resp.cantidad_efectiva;
            break;
        }
        /* Dinero: comprar gasta, vender gana (precio que pago / me pagan). */
        if (op == OP_COMPRAR_COMBUSTIBLE || op == OP_COMPRAR_OXIGENO)
            mapa->naves[id_nave].dinero -= resp.precio_total;
        else
            mapa->naves[id_nave].dinero += resp.precio_total;
        pthread_mutex_unlock(&mapa->mutex);

        pthread_mutex_lock(&g_hangar.mutex);
        snprintf(g_hangar.msg, sizeof(g_hangar.msg),
                 "OK: %d u. / $%d", resp.cantidad_efectiva, resp.precio_total);
        pthread_mutex_unlock(&g_hangar.mutex);
    }
    else
    {
        pthread_mutex_lock(&g_hangar.mutex);
        snprintf(g_hangar.msg, sizeof(g_hangar.msg), "Sin stock / rechazada");
        pthread_mutex_unlock(&g_hangar.mutex);
    }
}

/* ─── Hilo de propulsion (task #19, usa semaforos de celda #29) ────────── */

/*
 * Lee el teclado (no bloqueante) y mueve la nave:
 *   a / d (o flechas izq/der): giran la nave (cambian su direccion)
 *   w / s (o flechas arr/abajo): avanzan / retroceden en la direccion actual
 *
 * Sincronizacion con los semaforos de celda (task #29). Invariante: la nave
 * mantiene TOMADO (valor 0) el semaforo de la celda que ocupa.
 *   - Al arrancar reserva el semaforo de su celda inicial.
 *   - Al moverse: sem_trywait del semaforo de la celda DESTINO; si lo logra,
 *     actualiza la SHM (bajo el mutex) y hace sem_post del semaforo de la celda
 *     ORIGEN. El de la celda destino queda tomado. Usar sem_trywait (no
 *     sem_wait) evita deadlock cuando dos naves intentan intercambiar celdas.
 *   - Al salir libera el semaforo de la celda que ocupa.
 * Cada movimiento gasta 1 de combustible; si llega a 0, la nave se desactiva.
 */
static void *hilo_propulsion(void *arg)
{
    PropulsionArgs *args = (PropulsionArgs *)arg;
    Mapa *mapa = args->mapa;
    int id = args->id_nave;
    int ch;

    /* Reservar el semaforo de la celda inicial (mantener el invariante). */
    {
        int fi, ci;
        pthread_mutex_lock(&mapa->mutex);
        fi = mapa->naves[id].fila;
        ci = mapa->naves[id].col;
        pthread_mutex_unlock(&mapa->mutex);
        sem_t *sem_ini = semaforo_celda_abrir(fi, ci);
        if (sem_ini != NULL)
        {
            sem_trywait(sem_ini); /* reserva la celda inicial si estaba libre */
            semaforo_celda_cerrar(sem_ini);
        }
    }

    while (!g_salir)
    {
        int mover = 0;
        int df_mov = 0;
        int dc_mov = 0;

        ch = getch();

        switch (ch)
        {
        case 'a':
        case 'A':
        case KEY_LEFT:
            mapa->naves[id].direccion = (mapa->naves[id].direccion + 3) % 4;
            break;

        case 'd':
        case 'D':
        case KEY_RIGHT:
            mapa->naves[id].direccion = (mapa->naves[id].direccion + 1) % 4;
            break;

        case 'w':
        case 'W':
        case KEY_UP:
            df_mov = df[mapa->naves[id].direccion];
            dc_mov = dc[mapa->naves[id].direccion];
            mover = 1;
            break;

        case 's':
        case 'S':
        case KEY_DOWN:
            df_mov = -df[mapa->naves[id].direccion];
            dc_mov = -dc[mapa->naves[id].direccion];
            mover = 1;
            break;

        case 'e':
        case 'E':
            /* Tecla de accion: pedimos extraer al hilo de extraccion (#21). */
            g_extraer = 1;
            break;

        /* Transacciones en el hangar (task #44): f=combustible, o=oxigeno,
         * 1-4 = vender deuterio/mutexio/semaforita/kernelio (todo el stock). */
        case 'f':
        case 'F':
        {
            pthread_mutex_lock(&g_hangar.mutex);
            int h = g_hangar.id_estacion;
            pthread_mutex_unlock(&g_hangar.mutex);
            if (h >= 0)
                enviar_transaccion(mapa, id, h, OP_COMPRAR_COMBUSTIBLE, 10, args->cola_trx);
            break;
        }
        case 'o':
        case 'O':
        {
            pthread_mutex_lock(&g_hangar.mutex);
            int h = g_hangar.id_estacion;
            pthread_mutex_unlock(&g_hangar.mutex);
            if (h >= 0)
                enviar_transaccion(mapa, id, h, OP_COMPRAR_OXIGENO, 10, args->cola_trx);
            break;
        }
        case '1':
        {
            pthread_mutex_lock(&g_hangar.mutex);
            int h = g_hangar.id_estacion;
            pthread_mutex_unlock(&g_hangar.mutex);
            if (h >= 0)
            {
                int qty = mapa->naves[id].deuterio;
                if (qty > 0)
                    enviar_transaccion(mapa, id, h, OP_VENDER_DEUTERIO, qty, args->cola_trx);
            }
            break;
        }
        case '2':
        {
            pthread_mutex_lock(&g_hangar.mutex);
            int h = g_hangar.id_estacion;
            pthread_mutex_unlock(&g_hangar.mutex);
            if (h >= 0)
            {
                int qty = mapa->naves[id].mutexio;
                if (qty > 0)
                    enviar_transaccion(mapa, id, h, OP_VENDER_MUTEXIO, qty, args->cola_trx);
            }
            break;
        }
        case '3':
        {
            pthread_mutex_lock(&g_hangar.mutex);
            int h = g_hangar.id_estacion;
            pthread_mutex_unlock(&g_hangar.mutex);
            if (h >= 0)
            {
                int qty = mapa->naves[id].semaforita;
                if (qty > 0)
                    enviar_transaccion(mapa, id, h, OP_VENDER_SEMAFORITA, qty, args->cola_trx);
            }
            break;
        }
        case '4':
        {
            pthread_mutex_lock(&g_hangar.mutex);
            int h = g_hangar.id_estacion;
            pthread_mutex_unlock(&g_hangar.mutex);
            if (h >= 0)
            {
                int qty = mapa->naves[id].kernelio;
                if (qty > 0)
                    enviar_transaccion(mapa, id, h, OP_VENDER_KERNELIO, qty, args->cola_trx);
            }
            break;
        }

        default:
            break;
        }

        if (mover)
        {
            int fila_actual = mapa->naves[id].fila;
            int col_actual = mapa->naves[id].col;
            /* Movimiento toroidal: al cruzar un borde, la nave aparece en el
             * borde opuesto (el "+ MAPA_*" evita indices negativos antes del %). */
            int nueva_fila = (fila_actual + df_mov + MAPA_FILAS) % MAPA_FILAS;
            int nueva_col = (col_actual + dc_mov + MAPA_COLS) % MAPA_COLS;

            {
                /* Tipo de la celda destino (lectura corta bajo mutex). */
                pthread_mutex_lock(&mapa->mutex);
                TipoCelda tipo_dest = mapa->celdas[nueva_fila][nueva_col].tipo;
                int idx_dest = mapa->celdas[nueva_fila][nueva_col].idx;
                pthread_mutex_unlock(&mapa->mutex);

                if (tipo_dest == CELDA_ESTACION)
                {
                    /* Empujar contra una estacion = ENTRAR al hangar (task #44):
                     * tomamos su semaforo contador. No hay movimiento fisico. */
                    pthread_mutex_lock(&g_hangar.mutex);
                    int ya_dentro = (g_hangar.id_estacion >= 0);
                    pthread_mutex_unlock(&g_hangar.mutex);

                    if (!ya_dentro && idx_dest >= 0 && idx_dest < MAX_ESTACIONES)
                    {
                        char sem_name[SEM_HANGAR_NAME_LEN];
                        snprintf(sem_name, sizeof(sem_name), SEM_HANGAR_FMT, idx_dest);
                        sem_t *sem_h = sem_open(sem_name, 0); /* abrir el existente */
                        if (sem_h != SEM_FAILED)
                        {
                            if (sem_trywait(sem_h) == 0)
                            {
                                /* Lugar libre: registrar nuestro pid en la SHM. */
                                pthread_mutex_lock(&mapa->mutex);
                                for (int i = 0; i < 3; i++)
                                {
                                    if (mapa->estaciones[idx_dest].hangar[i] == 0)
                                    {
                                        mapa->estaciones[idx_dest].hangar[i] = (int)getpid();
                                        break;
                                    }
                                }
                                pthread_mutex_unlock(&mapa->mutex);

                                pthread_mutex_lock(&g_hangar.mutex);
                                g_hangar.id_estacion = idx_dest;
                                g_hangar.sem = sem_h;
                                snprintf(g_hangar.msg, sizeof(g_hangar.msg),
                                         "En hangar: f/o/1-4");
                                pthread_mutex_unlock(&g_hangar.mutex);
                            }
                            else
                            {
                                /* Hangar lleno (capacidad 3 alcanzada). */
                                sem_close(sem_h);
                                pthread_mutex_lock(&g_hangar.mutex);
                                snprintf(g_hangar.msg, sizeof(g_hangar.msg), "Hangar lleno");
                                pthread_mutex_unlock(&g_hangar.mutex);
                            }
                        }
                    }
                    /* No nos movemos sobre la celda de la estacion. */
                }
                else if (mapa->naves[id].combustible > 0)
                {
                    /* Movimiento normal. Si estabamos en un hangar, salir primero. */
                    pthread_mutex_lock(&g_hangar.mutex);
                    int en_hangar = (g_hangar.id_estacion >= 0);
                    pthread_mutex_unlock(&g_hangar.mutex);
                    if (en_hangar)
                        salir_hangar(mapa);

                    sem_t *sem_destino = semaforo_celda_abrir(nueva_fila, nueva_col);
                    if (sem_destino != NULL)
                    {
                        /* sem_trywait: si la celda destino esta ocupada por otra
                         * nave, no nos movemos (no bloquea -> sin deadlock). */
                        if (sem_trywait(sem_destino) == 0)
                        {
                            int movido = 0;
                            pthread_mutex_lock(&mapa->mutex);
                            /* Defensa: solo nos movemos si la celda sigue vacia. */
                            if (mapa->celdas[nueva_fila][nueva_col].tipo == CELDA_VACIA)
                            {
                                mapa->naves[id].fila = nueva_fila;
                                mapa->naves[id].col = nueva_col;
                                mapa->celdas[nueva_fila][nueva_col].tipo = CELDA_NAVE;
                                mapa->celdas[nueva_fila][nueva_col].idx = id;
                                mapa->celdas[fila_actual][col_actual].tipo = CELDA_VACIA;
                                mapa->celdas[fila_actual][col_actual].idx = -1;
                                mapa->naves[id].combustible--;
                                if (mapa->naves[id].combustible == 0)
                                    mapa->naves[id].estado = ESTADO_DESACTIVADO;
                                movido = 1;
                            }
                            pthread_mutex_unlock(&mapa->mutex);

                            if (movido)
                            {
                                /* Liberamos el semaforo de la celda ORIGEN. El de la
                                 * celda DESTINO queda tomado (la nave la ocupa). */
                                sem_t *sem_origen = semaforo_celda_abrir(fila_actual, col_actual);
                                if (sem_origen != NULL)
                                {
                                    sem_post(sem_origen);
                                    semaforo_celda_cerrar(sem_origen);
                                }
                            }
                            else
                            {
                                /* No nos movimos: devolvemos el semaforo destino. */
                                sem_post(sem_destino);
                            }
                        }
                        semaforo_celda_cerrar(sem_destino);
                    }
                }
            }
        }

        /* Poll cada 16ms (~60 Hz) para no quemar CPU. */
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 16000000L};
        nanosleep(&ts, NULL);
    }

    /* Al salir, soltar el hangar si todavia lo teniamos (task #44). */
    salir_hangar(mapa);

    /* Al salir, liberar el semaforo de la celda que ocupa la nave. */
    {
        int ff, cf;
        pthread_mutex_lock(&mapa->mutex);
        ff = mapa->naves[id].fila;
        cf = mapa->naves[id].col;
        pthread_mutex_unlock(&mapa->mutex);
        sem_t *sem_fin = semaforo_celda_abrir(ff, cf);
        if (sem_fin != NULL)
        {
            sem_post(sem_fin);
            semaforo_celda_cerrar(sem_fin);
        }
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
    pthread_t th_radar, th_vital, th_alertas, th_propulsion, th_extraccion;
    RadarArgs radar_args;
    VitalArgs vital_args;
    AlertaArgs alerta_args;
    PropulsionArgs propulsion_args;
    ExtraccionArgs extraccion_args;
    struct sigaction sa;
    char nombre_cola_propia[MQ_NAVE_NAME_LEN];
    char nombre_cola_trx[MQ_NAVE_TRX_NAME_LEN];

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
    mapa->naves[id_nave].dinero = 0; /* arranca sin creditos: mina y vende para ganar (#44) */
    mapa->naves[id_nave].pid = getpid();
    pthread_mutex_unlock(&mapa->mutex);

    snprintf(nombre_cola_propia, sizeof(nombre_cola_propia), MQ_NAVE_FMT, (int)getpid());
    snprintf(nombre_cola_trx, sizeof(nombre_cola_trx), MQ_NAVE_TRX_FMT, (int)getpid());

    /* Estado de alertas (task #30): mutex local + nombre de cola para el hilo. */
    pthread_mutex_init(&g_alerta.mutex, NULL);
    g_alerta.id_estacion = -1;
    snprintf(alerta_args.cola, sizeof(alerta_args.cola), "%s", nombre_cola_propia);

    /* Cola dedicada para las RESPUESTAS de transaccion de la estacion (task #44).
     * La creamos aca para que exista cuando la estacion responda; el hilo de
     * propulsion la re-abre por cada transaccion. */
    {
        struct mq_attr attr_trx;
        attr_trx.mq_flags = 0;
        attr_trx.mq_maxmsg = 4;
        attr_trx.mq_msgsize = (long)sizeof(MsgTransaccionResp);
        attr_trx.mq_curmsgs = 0;
        mqd_t mq_trx = mq_open(nombre_cola_trx, O_CREAT | O_RDONLY, 0666, &attr_trx);
        if (mq_trx == (mqd_t)-1)
            perror("nave: no se pudo crear la cola de transacciones");
        else
            mq_close(mq_trx);
    }

    /* Estado del hangar (task #44): mutex local. */
    pthread_mutex_init(&g_hangar.mutex, NULL);
    g_hangar.id_estacion = -1;
    g_hangar.sem = NULL;
    g_hangar.msg[0] = '\0';

    /* 5) Inicializar ncurses. */
    initscr();
    cbreak();
    noecho();
    curs_set(0); /* ocultar cursor */
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE); /* getch() no bloquea (lo usa el hilo de propulsion) */
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

    /* Hilo de propulsion (task #19 + transacciones #44). */
    propulsion_args.mapa = mapa;
    propulsion_args.id_nave = id_nave;
    snprintf(propulsion_args.cola_trx, sizeof(propulsion_args.cola_trx), "%s", nombre_cola_trx);
    if (pthread_create(&th_propulsion, NULL, hilo_propulsion, &propulsion_args) != 0)
    {
        g_salir = 1;
        pthread_join(th_radar, NULL);
        pthread_join(th_vital, NULL);
        pthread_join(th_alertas, NULL);
        endwin();
        perror("pthread_create(propulsion)");
        munmap(mapa, sizeof(Mapa));
        mq_unlink(nombre_cola_propia);
        return EXIT_FAILURE;
    }

    /* Hilo de extraccion (task #21). */
    extraccion_args.mapa = mapa;
    extraccion_args.id_nave = id_nave;
    if (pthread_create(&th_extraccion, NULL, hilo_extraccion, &extraccion_args) != 0)
    {
        g_salir = 1;
        pthread_join(th_radar, NULL);
        pthread_join(th_vital, NULL);
        pthread_join(th_alertas, NULL);
        pthread_join(th_propulsion, NULL);
        endwin();
        perror("pthread_create(extraccion)");
        munmap(mapa, sizeof(Mapa));
        mq_unlink(nombre_cola_propia);
        return EXIT_FAILURE;
    }

    /* 7) Esperar a que los hilos terminen (cuando g_salir = 1). */
    pthread_join(th_radar, NULL);
    pthread_join(th_vital, NULL);
    pthread_join(th_alertas, NULL);
    pthread_join(th_propulsion, NULL);
    pthread_join(th_extraccion, NULL);

    /* 8) Cleanup ncurses. */
    endwin();

    /* 9) Cleanup IPC. La SHM la destruye el servidor, no la nave. */
    munmap(mapa, sizeof(Mapa));
    mq_unlink(nombre_cola_propia);
    mq_unlink(nombre_cola_trx);

    printf("nave: saliendo limpiamente\n");
    return EXIT_SUCCESS;
}

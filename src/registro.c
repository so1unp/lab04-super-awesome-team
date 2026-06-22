#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <mqueue.h>
#include <fcntl.h>
#include <unistd.h>

#include "registro.h"
#include "ipc.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal_stop(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int posicion_aleatoria_libre(Mapa *mapa, int *fila_out, int *col_out)
{
    const int max_intentos = MAPA_FILAS * MAPA_COLS * 2;

    for (int i = 0; i < max_intentos; i++)
    {
        int fila = rand() % MAPA_FILAS;
        int col = rand() % MAPA_COLS;

        if (mapa->celdas[fila][col].tipo == CELDA_VACIA)
        {
            *fila_out = fila;
            *col_out = col;
            return 0;
        }
    }

    return -1;
}

static int buscar_slot_nave(const Mapa *mapa)
{
    for (int i = 0; i < MAX_NAVES; i++)
    {
        if (mapa->naves[i].pid == 0)
            return i;
    }
    return -1;
}

static int buscar_slot_estacion(const Mapa *mapa, int max_estaciones)
{
    for (int i = 0; i < max_estaciones; i++)
    {
        if (mapa->estaciones[i].pid == 0)
            return i;
    }
    return -1;
}

static int buscar_nave_por_pid(const Mapa *mapa, pid_t pid)
{
    for (int i = 0; i < MAX_NAVES; i++)
    {
        if (mapa->naves[i].pid == pid)
            return i;
    }
    return -1;
}

static int buscar_estacion_por_pid(const Mapa *mapa, pid_t pid, int max_estaciones)
{
    for (int i = 0; i < max_estaciones; i++)
    {
        if (mapa->estaciones[i].pid == pid)
            return i;
    }
    return -1;
}

static void liberar_celda(Mapa *mapa, int fila, int col)
{
    if (fila < 0 || fila >= MAPA_FILAS || col < 0 || col >= MAPA_COLS)
        return;

    mapa->celdas[fila][col].tipo = CELDA_VACIA;
    mapa->celdas[fila][col].idx = -1;
}

static void responder_registro(const MsgRegistro *req, const MsgRegistroResp *resp)
{
    if (req->cola_respuesta[0] == '\0')
        return;

    mqd_t qresp = mq_open(req->cola_respuesta, O_WRONLY);
    if (qresp == (mqd_t)-1)
        return;

    (void)mq_send(qresp, (const char *)resp, sizeof(*resp), 0);
    mq_close(qresp);
}

static void procesar_registrar(Mapa *mapa, const Config *cfg, const MsgRegistro *req)
{
    MsgRegistroResp resp;
    int fila = -1;
    int col = -1;
    int slot = -1;

    memset(&resp, 0, sizeof(resp));
    resp.id = -1;
    resp.fila = -1;
    resp.col = -1;
    resp.error = 1;
    (void)snprintf(resp.shm_name, sizeof(resp.shm_name), "%s", SHM_MAPA_NAME);

    if (posicion_aleatoria_libre(mapa, &fila, &col) != 0)
    {
        responder_registro(req, &resp);
        return;
    }

    if (req->tipo == CLIENTE_NAVE)
    {
        slot = buscar_slot_nave(mapa);
        if (slot < 0)
        {
            responder_registro(req, &resp);
            return;
        }

        mapa->naves[slot].id = slot;
        mapa->naves[slot].pid = req->pid;
        mapa->naves[slot].fila = fila;
        mapa->naves[slot].col = col;
        mapa->naves[slot].estado = ESTADO_ACTIVO;
        mapa->num_naves++;

        mapa->celdas[fila][col].tipo = CELDA_NAVE;
        mapa->celdas[fila][col].idx = slot;
    }
    else
    {
        int max_est = cfg->num_estaciones;
        if (max_est > MAX_ESTACIONES)
            max_est = MAX_ESTACIONES;

        slot = buscar_slot_estacion(mapa, max_est);
        if (slot < 0)
        {
            responder_registro(req, &resp);
            return;
        }

        mapa->estaciones[slot].id = slot;
        mapa->estaciones[slot].pid = req->pid;
        mapa->estaciones[slot].fila = fila;
        mapa->estaciones[slot].col = col;
        mapa->estaciones[slot].estado = ESTADO_ACTIVO;
        mapa->num_estaciones++;

        mapa->celdas[fila][col].tipo = CELDA_ESTACION;
        mapa->celdas[fila][col].idx = slot;
    }

    resp.id = slot;
    resp.fila = fila;
    resp.col = col;
    resp.error = 0;

    responder_registro(req, &resp);
}

static void procesar_desregistrar(Mapa *mapa, const Config *cfg, const MsgRegistro *req)
{
    int idx;

    if (req->tipo == CLIENTE_NAVE)
    {
        idx = req->id >= 0 ? req->id : buscar_nave_por_pid(mapa, req->pid);
        if (idx < 0 || idx >= MAX_NAVES)
            return;

        if (mapa->naves[idx].estado == ESTADO_DESACTIVADO)
        {
            /* Nave game over: se mantiene en el mapa como nave muerta */
            return;
        }

        liberar_celda(mapa, mapa->naves[idx].fila, mapa->naves[idx].col);
        memset(&mapa->naves[idx], 0, sizeof(mapa->naves[idx]));
        if (mapa->num_naves > 0)
            mapa->num_naves--;
    }
    else
    {
        int max_est = cfg->num_estaciones;
        if (max_est > MAX_ESTACIONES)
            max_est = MAX_ESTACIONES;

        idx = req->id >= 0 ? req->id : buscar_estacion_por_pid(mapa, req->pid, max_est);
        if (idx < 0 || idx >= MAX_ESTACIONES)
            return;

        liberar_celda(mapa, mapa->estaciones[idx].fila, mapa->estaciones[idx].col);
        memset(&mapa->estaciones[idx], 0, sizeof(mapa->estaciones[idx]));
        if (mapa->num_estaciones > 0)
            mapa->num_estaciones--;
    }
}

static int estaciones_activas_count(const Mapa *mapa)
{
    int activas = 0;
    for (int i = 0; i < MAX_ESTACIONES; i++) {
        if (mapa->estaciones[i].pid != 0 &&
            mapa->estaciones[i].estado == ESTADO_ACTIVO)
            activas++;
    }
    return activas;
}

static void notificar_fin_juego(const Mapa *mapa)
{
    printf("servidor: FIN DE JUEGO - todas las estaciones desactivadas\n");
    /* Notificar a todas las naves activas */
    for (int i = 0; i < MAX_NAVES; i++) {
        if (mapa->naves[i].pid != 0)
            kill(mapa->naves[i].pid, SIGUSR1);
    }
}

static void procesar_desactivar_estacion(Mapa *mapa, const MsgRegistro *req)
{
    int idx = req->id >= 0 ? req->id : -1;
    if (idx < 0 || idx >= MAX_ESTACIONES)
        return;

    mapa->estaciones[idx].estado = ESTADO_DESACTIVADO;
    printf("servidor: estacion %d desactivada (sin combustible)\n", idx);

    if (estaciones_activas_count(mapa) == 0) {
        mapa->juego_activo = 0;
        notificar_fin_juego(mapa);
        g_stop = 1;
    }
}

static void procesar_desactivar(Mapa *mapa, const MsgRegistro *req)
{
    int idx;

    if (req->tipo != CLIENTE_NAVE)
        return;

    idx = req->id >= 0 ? req->id : buscar_nave_por_pid(mapa, req->pid);
    if (idx < 0 || idx >= MAX_NAVES)
        return;

    mapa->naves[idx].estado = ESTADO_DESACTIVADO;
    mapa->celdas[mapa->naves[idx].fila][mapa->naves[idx].col].tipo = CELDA_NAVE_MUERTA;
    mapa->celdas[mapa->naves[idx].fila][mapa->naves[idx].col].idx = idx;
}

/*
 * Asteroide agotado (task #21): la nave lo notifica cuando lo vacia.
 * Lo marcamos desactivado, liberamos su celda y bajamos el contador.
 * req->id es el indice del asteroide en mapa->asteroides[].
 */
static void procesar_desactivar_asteroide(Mapa *mapa, const MsgRegistro *req)
{
    int idx = req->id;
    if (idx < 0 || idx >= MAX_ASTEROIDES)
        return;

    /* Solo liberamos la celda si todavia figura como asteroide (no pisar
     * una nave que pudiera haber entrado). */
    int af = mapa->asteroides[idx].fila;
    int ac = mapa->asteroides[idx].col;
    if (af >= 0 && af < MAPA_FILAS && ac >= 0 && ac < MAPA_COLS &&
        mapa->celdas[af][ac].tipo == CELDA_ASTEROIDE)
        liberar_celda(mapa, af, ac);

    mapa->asteroides[idx].estado = ESTADO_DESACTIVADO;
    if (mapa->num_asteroides > 0)
        mapa->num_asteroides--;
}

int registro_servidor_loop(Mapa *mapa, const Config *cfg)
{
    struct mq_attr attr;
    mqd_t qreg;
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal_stop;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);

    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = MQ_REGISTRO_MAXMSG;
    attr.mq_msgsize = (long)sizeof(MsgRegistro);

    mq_unlink(MQ_REGISTRO_NAME);
    qreg = mq_open(MQ_REGISTRO_NAME, O_CREAT | O_RDONLY, 0660, &attr);
    if (qreg == (mqd_t)-1)
    {
        perror("mq_open registro");
        return -1;
    }

    printf("servidor: cola de registro '%s' lista\n", MQ_REGISTRO_NAME);

    srand((unsigned int)time(NULL));

    while (!g_stop)
    {
        MsgRegistro req;
        struct timespec ts;

        if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
            continue;
        ts.tv_sec += 1;

        if (mq_timedreceive(qreg, (char *)&req, sizeof(req), NULL, &ts) == -1)
        {
            if (errno == ETIMEDOUT || errno == EINTR)
                continue;
            perror("mq_receive registro");
            break;
        }

        if (pthread_mutex_lock(&mapa->mutex) != 0)
            continue;

        switch (req.op)
        {
        case REG_OP_REGISTRAR:
            procesar_registrar(mapa, cfg, &req);
            break;
        case REG_OP_DESREGISTRAR:
            procesar_desregistrar(mapa, cfg, &req);
            break;
        case REG_OP_DESACTIVAR:
            procesar_desactivar(mapa, &req);
            break;
        case REG_OP_DESACTIVAR_ESTACION:
            procesar_desactivar_estacion(mapa, &req);
            break;
        case REG_OP_DESACTIVAR_ASTEROIDE:
            procesar_desactivar_asteroide(mapa, &req);
            break;
        default:
            break;
        }

        (void)pthread_mutex_unlock(&mapa->mutex);
    }

    mq_close(qreg);
    mq_unlink(MQ_REGISTRO_NAME);
    return 0;
}

#include <stdio.h>
#include <string.h>
#include <mqueue.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <time.h>

#include "ipc.h"
#include "mapa.h"

int main(void)
{
    char cola_resp[MQ_REG_RESP_LEN];
    mqd_t qresp;
    mqd_t qreg;
    MsgRegistro req;
    MsgRegistroResp resp;

    snprintf(cola_resp, sizeof(cola_resp), "/cosmikernel_resp_%d", (int)getpid());

    struct mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = 8;
    attr.mq_msgsize = sizeof(MsgRegistroResp);

    mq_unlink(cola_resp);
    qresp = mq_open(cola_resp, O_CREAT | O_RDONLY, 0660, &attr);
    if (qresp == (mqd_t)-1) {
        perror("mq_open resp");
        return 1;
    }

    qreg = mq_open(MQ_REGISTRO_NAME, O_WRONLY);
    if (qreg == (mqd_t)-1) {
        perror("mq_open registro");
        mq_close(qresp);
        mq_unlink(cola_resp);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.op = REG_OP_REGISTRAR;
    req.tipo = CLIENTE_NAVE;
    req.pid = getpid();
    req.id = -1;
    snprintf(req.cola_respuesta, sizeof(req.cola_respuesta), "%s", cola_resp);

    if (mq_send(qreg, (const char *)&req, sizeof(req), 0) == -1) {
        perror("mq_send registrar");
        mq_close(qreg);
        mq_close(qresp);
        mq_unlink(cola_resp);
        return 1;
    }

    if (mq_receive(qresp, (char *)&resp, sizeof(resp), NULL) == -1) {
        perror("mq_receive resp");
        mq_close(qreg);
        mq_close(qresp);
        mq_unlink(cola_resp);
        return 1;
    }

    if (resp.error != 0) {
        fprintf(stderr, "registro rechazado\n");
        mq_close(qreg);
        mq_close(qresp);
        mq_unlink(cola_resp);
        return 1;
    }

    int fd = shm_open(resp.shm_name, O_RDONLY, 0);
    if (fd == -1) {
        perror("shm_open");
        mq_close(qreg);
        mq_close(qresp);
        mq_unlink(cola_resp);
        return 1;
    }

    Mapa *mapa = mmap(NULL, sizeof(Mapa), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (mapa == MAP_FAILED) {
        perror("mmap");
        mq_close(qreg);
        mq_close(qresp);
        mq_unlink(cola_resp);
        return 1;
    }

    printf("REG OK: id=%d pos=(%d,%d) shm=%s\n", resp.id, resp.fila, resp.col, resp.shm_name);
    printf("MAPA: celda tipo=%c idx=%d num_naves=%d\n",
           (char)mapa->celdas[resp.fila][resp.col].tipo,
           mapa->celdas[resp.fila][resp.col].idx,
           mapa->num_naves);

    memset(&req, 0, sizeof(req));
    req.op = REG_OP_DESREGISTRAR;
    req.tipo = CLIENTE_NAVE;
    req.pid = getpid();
    req.id = resp.id;

    if (mq_send(qreg, (const char *)&req, sizeof(req), 0) == -1)
        perror("mq_send desregistrar");
    else
        printf("DESREG enviado\n");

    {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 200000000L;
        (void)nanosleep(&ts, NULL);
    }

    printf("MAPA post-desreg: celda tipo=%c idx=%d num_naves=%d\n",
           (char)mapa->celdas[resp.fila][resp.col].tipo,
           mapa->celdas[resp.fila][resp.col].idx,
           mapa->num_naves);

    munmap(mapa, sizeof(Mapa));

    mq_close(qreg);
    mq_close(qresp);
    mq_unlink(cola_resp);

    return 0;
}

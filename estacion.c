#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <mqueue.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "include/estacion.h"
#include "include/config.h"
#include "include/ipc.h"
#include "include/mapa.h"

Estacion mi_estacion;
int combustible = ESTACION_COMBUSTIBLE_INICIAL;

/* SHM del mapa: la estacion la lee para conocer los PIDs de las naves
 * registradas y poder enviarles la alerta de combustible (task #30). */
Mapa *mapa_shm = NULL;
int  mi_id_estacion = -1;  /* slot en Mapa.estaciones[] si la estacion se registro */

//declaramos el mutex global
pthread_mutex_t lock;

/*
 * Registra la estacion contra el servidor (cola MQ_REGISTRO_NAME) usando el
 * protocolo de ipc.h. El servidor le asigna un slot en Mapa.estaciones[],
 * una posicion y marca la celda como CELDA_ESTACION (asi aparece la 'E' en
 * el mapa). Devuelve el id/slot asignado o -1 si falla.
 * La posicion asignada se devuelve en *fila_out / *col_out (si no son NULL).
 */
static int registrar_estacion(int *fila_out, int *col_out)
{
    mqd_t mq_registro, mq_resp;
    char nombre_resp[MQ_ESTACION_NAME_LEN];
    MsgRegistro msg;
    MsgRegistroResp resp;
    struct mq_attr attr;
    struct timespec ts;
    int id = -1;

    /* Cola temporal para recibir la respuesta del registro. */
    snprintf(nombre_resp, sizeof(nombre_resp), MQ_ESTACION_FMT, (int)getpid());

    memset(&msg, 0, sizeof(msg));
    msg.op   = REG_OP_REGISTRAR;
    msg.tipo = CLIENTE_ESTACION;
    msg.pid  = getpid();
    msg.id   = -1;
    snprintf(msg.cola_respuesta, sizeof(msg.cola_respuesta), "%s", nombre_resp);

    attr.mq_flags = 0; attr.mq_maxmsg = 4;
    attr.mq_msgsize = sizeof(MsgRegistroResp); attr.mq_curmsgs = 0;
    mq_resp = mq_open(nombre_resp, O_CREAT | O_RDONLY, 0666, &attr);
    if (mq_resp == (mqd_t)-1) { perror("mq_open(estacion resp)"); return -1; }

    mq_registro = mq_open(MQ_REGISTRO_NAME, O_WRONLY);
    if (mq_registro == (mqd_t)-1)
    {
        mq_close(mq_resp); mq_unlink(nombre_resp);
        return -1;
    }

    if (mq_send(mq_registro, (const char *)&msg, sizeof(msg), 0) == -1)
    {
        perror("mq_send(registro estacion)");
        mq_close(mq_registro); mq_close(mq_resp); mq_unlink(nombre_resp);
        return -1;
    }
    mq_close(mq_registro);

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3;  /* timeout 3s para no colgarnos si el servidor no responde */
    if (mq_timedreceive(mq_resp, (char *)&resp, sizeof(resp), NULL, &ts) != -1 &&
        resp.error == 0)
    {
        id = resp.id;
        if (fila_out) *fila_out = resp.fila;
        if (col_out)  *col_out  = resp.col;
    }

    mq_close(mq_resp);
    mq_unlink(nombre_resp);  /* la estacion no usa esta cola despues del registro */
    return id;
}

sem_t *semaforo_hangar;

char nombre_semaforo[] = "/hangar_estacion_1";

void* gasto_combustible(void* arg){
    (void)arg;  /* el hilo no usa argumentos */
    while(1){
        usleep(1000000);// rapidito para probar
        /* sleep(DEFAULT_INTERVALO_COMBUSTIBLE); */
        
        int lugares_libres;
        sem_getvalue(semaforo_hangar, &lugares_libres);

        if(lugares_libres < 0){
            lugares_libres = 0; //pongo en 0 para que no sea negativo
        }

        //bloqueamos antes de tocar la variable compartida
        pthread_mutex_lock(&lock);
        
        combustible -= 4;
        printf("Combustible actual: %d\n", combustible);
        
        //evaluamos si nos quedamos en cero para apagar todo
        if(combustible <= 0){
            printf("Estacion espacial sin combustible... Desactivando\n");
            /* Reflejar la desactivacion en la SHM para el radar (task #30). */
            if (mapa_shm != NULL && mi_id_estacion >= 0) {
                pthread_mutex_lock(&mapa_shm->mutex);
                mapa_shm->estaciones[mi_id_estacion].combustible = 0;
                mapa_shm->estaciones[mi_id_estacion].estado = ESTADO_DESACTIVADO;
                pthread_mutex_unlock(&mapa_shm->mutex);
            }
            pthread_mutex_unlock(&lock);
            break; //rompe el while para ir al exit
        }

        /* Reflejar el combustible (deuterio) actual en la SHM para que el
         * radar de las naves lo muestre al lado de la estacion (task #30). */
        if (mapa_shm != NULL && mi_id_estacion >= 0) {
            pthread_mutex_lock(&mapa_shm->mutex);
            mapa_shm->estaciones[mi_id_estacion].combustible = combustible;
            pthread_mutex_unlock(&mapa_shm->mutex);
        }
        
        //evaluamos si pasamos el umbral de alerta
        if(combustible <= DEFAULT_UMBRAL_COMBUSTIBLE ){
            printf("¡ME ESTOY QUEDANDO SIN COMBUSTIBLE!!!!!\n");

            /*
             * Aviso de deuterio a las naves del cuadrante (task #26 -> #30).
             * Usamos el protocolo de ipc.h: leemos los PIDs de las naves
             * registradas desde la SHM y enviamos un MsgAlertaCombustible a
             * la cola privada de cada nave (/cosmikernel_nave_<pid>).
             */
            if (mapa_shm != NULL) {
                int pids[MAX_NAVES];
                int n = 0;

                /* Copiamos los PIDs de naves activas bajo el mutex del mapa
                 * (seccion critica corta: solo copiar, no enviar). */
                pthread_mutex_lock(&mapa_shm->mutex);
                for (int i = 0; i < MAX_NAVES; i++) {
                    if (mapa_shm->naves[i].pid != 0 &&
                        mapa_shm->naves[i].estado == ESTADO_ACTIVO) {
                        pids[n] = mapa_shm->naves[i].pid;
                        n++;
                    }
                }
                pthread_mutex_unlock(&mapa_shm->mutex);

                MsgAlertaCombustible alerta;
                alerta.id_estacion = mi_id_estacion;
                alerta.pid_estacion = getpid();
                alerta.combustible_actual = combustible;

                for (int i = 0; i < n; i++) {
                    char nombre_cola_nave[MQ_NAVE_NAME_LEN];
                    snprintf(nombre_cola_nave, sizeof(nombre_cola_nave),
                             MQ_NAVE_FMT, pids[i]);

                    mqd_t cola_nave = mq_open(nombre_cola_nave, O_WRONLY);
                    if (cola_nave != (mqd_t)-1) {
                        if (mq_send(cola_nave, (const char *)&alerta,
                                    sizeof(alerta), 0) == -1)
                            perror("Error al enviar S.O.S a una nave");
                        else
                            printf("-> S.O.S (deuterio) enviado a nave pid %d\n", pids[i]);
                        mq_close(cola_nave);
                    }
                }
            }
        }
        
        //soltamos el mutex
        pthread_mutex_unlock(&lock);
    }
    
    exit(EXIT_SUCCESS);
}

int main(){
    //inicializamos el mutex antes de arrancar cualquier hilo
    if (pthread_mutex_init(&lock, NULL) != 0) {
        perror("Error al inicializar el mutex");
        return 1;
    }

    semaforo_hangar = sem_open(nombre_semaforo, O_CREAT, 0644, 3);
    if(semaforo_hangar == SEM_FAILED){
        printf("Error al crear el semaforo del hangar");
        return 1;
    }
    printf("Hangar inicializado con 3 espacios disponibles.\n");

    /* Abrir la SHM del mapa (creada por el servidor). La estacion la usa para:
     *  - conocer los PIDs de las naves registradas (para mandarles la alerta)
     *  - reflejar su combustible/deuterio para que el radar lo muestre.
     * No es fatal si falla: la estacion sigue consumiendo combustible. */
    int fd_shm = shm_open(SHM_MAPA_NAME, O_RDWR, 0666);
    if (fd_shm != -1) {
        mapa_shm = mmap(NULL, sizeof(Mapa), PROT_READ | PROT_WRITE, MAP_SHARED, fd_shm, 0);
        close(fd_shm);
        if (mapa_shm == MAP_FAILED) {
            mapa_shm = NULL;
            fprintf(stderr, "estacion: no se pudo mapear la SHM\n");
        }
    } else {
        fprintf(stderr, "estacion: SHM no encontrada (servidor no corriendo)\n");
    }

    /* Registrarse con el servidor: nos asigna un slot y posicion en el mapa,
     * y marca la celda como CELDA_ESTACION (asi aparece la 'E' en el radar). */
    if (mapa_shm != NULL) {
        int est_fila = -1, est_col = -1;
        mi_id_estacion = registrar_estacion(&est_fila, &est_col);
        if (mi_id_estacion < 0) {
            fprintf(stderr, "estacion: no se pudo registrar contra el servidor\n");
        } else {
            /* Inicializamos nuestro combustible (deuterio) en la SHM. */
            pthread_mutex_lock(&mapa_shm->mutex);
            mapa_shm->estaciones[mi_id_estacion].combustible = combustible;
            pthread_mutex_unlock(&mapa_shm->mutex);
            printf("Estacion registrada con id %d en (%d,%d)\n",
                   mi_id_estacion, est_fila, est_col);
        }
    }

    pthread_t estacion1;
    
    //crea el hilo
    if (pthread_create(&estacion1, NULL, gasto_combustible, NULL) != 0) {
        perror("Error al crear el hilo");
        return 1;
    }

    //esperamos a que el hilo termine (esto frena al main para que no se cierre)
    pthread_join(estacion1, NULL);

    //limpiamos la memoria del mutex antes de apagar el programa
    sem_close(semaforo_hangar);
    sem_unlink(nombre_semaforo); //borra el semaforo del SO
    pthread_mutex_destroy(&lock);
    
    return 0;
}
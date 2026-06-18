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

sem_t *semaforo_hangar;

char nombre_semaforo[] = "/hangar_estacion_1";

void* gasto_combustible(void* arg){
    while(1){
        usleep(1000000);// rapidito para probar
        /* sleep(DEFAULT_INTERVALO_COMBUSTIBLE); */
        
        int lugares_libres;
        sem_getvalue(semaforo_hangar, &lugares_libres);

        if(lugares_libres < 0){
            lugares_libres = 0; //pongo en 0 para que no sea negativo
        }
        
        int naver_adentro = 3 - lugares_libres;

        //bloqueamos antes de tocar la variable compartida
        pthread_mutex_lock(&lock);
        
        combustible -= 4;
        printf("Combustible actual: %d\n", combustible);
        
        //evaluamos si nos quedamos en cero para apagar todo
        if(combustible <= 0){
            printf("Estacion espacial sin combustible... Desactivando\n");
            pthread_mutex_unlock(&lock);
            break; //rompe el while para ir al exit
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

    /* Abrir la SHM del mapa para conocer las naves registradas (task #30).
     * No es fatal si falla: la estacion sigue consumiendo combustible, pero
     * sin SHM no puede avisar a las naves. */
    int fd_shm = shm_open(SHM_MAPA_NAME, O_RDWR, 0666);
    if (fd_shm != -1) {
        mapa_shm = mmap(NULL, sizeof(Mapa), PROT_READ | PROT_WRITE, MAP_SHARED, fd_shm, 0);
        close(fd_shm);
        if (mapa_shm == MAP_FAILED) {
            mapa_shm = NULL;
            fprintf(stderr, "estacion: no se pudo mapear la SHM; sin alertas a naves\n");
        } else {
            /* Si la estacion esta registrada, buscamos nuestro slot por PID. */
            for (int i = 0; i < MAX_ESTACIONES; i++) {
                if (mapa_shm->estaciones[i].pid == (int)getpid()) {
                    mi_id_estacion = i;
                    break;
                }
            }
        }
    } else {
        fprintf(stderr, "estacion: SHM no encontrada (servidor no corriendo); sin alertas a naves\n");
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
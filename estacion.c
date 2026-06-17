#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <mqueue.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <string.h>
#include "include/estacion.h"
#include "include/config.h"

Estacion mi_estacion;
int combustible = ESTACION_COMBUSTIBLE_INICIAL;

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
            
            mqd_t cola;
            char buff[256];
            int prio = 0;

            sprintf(buff, "SOS_DEUTERIO");

            int cant_naves = 3;
            int ids_naves[] = {1, 2, 3};

            for(int i=0 ; i<cant_naves ; i++){
                char nombre_cola_nave[50];
                sprintf(nombre_cola_nave, "/cola_nave_%d", ids_naves[i]);

                mqd_t cola_nave = mq_open(nombre_cola_nave, O_WRONLY);
                
                if (cola_nave != (mqd_t)-1) {
                    if (mq_send(cola_nave, buff, strlen(buff) + 1, prio) == -1) {
                        perror("Error al enviar S.O.S a una nave");
                    } else {
                        printf("-> S.O.S enviado a la nave %d por %s\n", ids_naves[i], nombre_cola_nave);
                    }
                    mq_close(cola_nave);
                } else {
                    printf("Aviso: La nave %d no tiene su cola abierta aún.\n", ids_naves[i]);
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
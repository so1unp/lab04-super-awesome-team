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
#include <signal.h>
#include "include/estacion.h"
#include "include/config.h"
#include "include/ipc.h"
#include "include/mapa.h"

/* Bandera de salida: la setea el handler de SIGUSR1 (servidor apagado)
 * o cualquier condicion interna de fin. */
static volatile sig_atomic_t g_salir = 0;

/* Bandera que indica que la salida fue por desconexion del servidor. */
static volatile sig_atomic_t g_servidor_desconectado = 0;

static void manejar_sigusr1(int sig)
{
    (void)sig;
    g_servidor_desconectado = 1;
    g_salir = 1;
}

Estacion mi_estacion;
int combustible = ESTACION_COMBUSTIBLE_INICIAL;

// --- INVENTARIO Y CAJA DE LA ESTACIÓN ---
int oxigeno = 1000000;
int creditos = 1000000;

// Stock de minerales comprados
int stock_deuterio = 0;
int stock_mutexio = 0;
int stock_semaforita = 0;
int stock_kernelio = 0;

/* SHM del mapa: la estacion la lee para conocer los PIDs de las naves
 * registradas y poder enviarles la alerta de combustible (task #30). */
Mapa *mapa_shm = NULL;
int mi_id_estacion = -1; /* slot en Mapa.estaciones[] si la estacion se registro */

/* Segundos entre cada decremento de combustible (se carga de config.txt). */
int intervalo_combustible_seg = DEFAULT_INTERVALO_COMBUSTIBLE;

// declaramos el mutex global
pthread_mutex_t lock;

// Precios de transacciones (cargados desde config.txt en main)
int precio_deuterio   = DEFAULT_PRECIO_DEUTERIO;
int precio_mutexio    = DEFAULT_PRECIO_MUTEXIO;
int precio_semaforita = DEFAULT_PRECIO_SEMAFORITA;
int precio_kernelio   = DEFAULT_PRECIO_KERNELIO;
int precio_combustible = DEFAULT_PRECIO_COMBUSTIBLE;
int precio_oxigeno    = DEFAULT_PRECIO_OXIGENO;

// --- BITÁCORA DE TRANSACCIONES ---
static int fd_bitacora = -1;
static pthread_mutex_t mutex_bitacora = PTHREAD_MUTEX_INITIALIZER;

/*
 * Escribe una línea de transacción en la bitácora de forma atómica.
 * Usa O_APPEND + write() (atómico en POSIX) protegido con mutex_bitacora.
 * Formato: [timestamp] nave=X tipo=venta|compra recurso=Y cantidad=Z precio=W
 */
static void registrar_bitacora(pid_t pid_nave, const char *tipo, const char *recurso, int cantidad, int precio)
{
    if (fd_bitacora < 0)
        return;

    char linea[256];
    time_t ahora = time(NULL);
    struct tm *tm_info = localtime(&ahora);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm_info);

    int len = snprintf(linea, sizeof(linea),
                       "[%s] nave=%d tipo=%s recurso=%s cantidad=%d precio=%d\n",
                       ts, (int)pid_nave, tipo, recurso, cantidad, precio);

    pthread_mutex_lock(&mutex_bitacora);
    write(fd_bitacora, linea, (size_t)len);
    pthread_mutex_unlock(&mutex_bitacora);
}

// Variables globales para la cola de transacciones
char nombre_mq_transacciones[MQ_ESTACION_NAME_LEN];
mqd_t mq_transacciones;
sem_t *semaforo_hangar = SEM_FAILED;
char nombre_semaforo[SEM_HANGAR_NAME_LEN]; /* se arma con SEM_HANGAR_FMT(id) tras registrarse */

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
    msg.op = REG_OP_REGISTRAR;
    msg.tipo = CLIENTE_ESTACION;
    msg.pid = getpid();
    msg.id = -1;
    snprintf(msg.cola_respuesta, sizeof(msg.cola_respuesta), "%s", nombre_resp);

    attr.mq_flags = 0;
    attr.mq_maxmsg = 4;
    attr.mq_msgsize = sizeof(MsgRegistroResp);
    attr.mq_curmsgs = 0;
    mq_resp = mq_open(nombre_resp, O_CREAT | O_RDONLY, 0666, &attr);
    if (mq_resp == (mqd_t)-1)
    {
        perror("mq_open(estacion resp)");
        return -1;
    }

    mq_registro = mq_open(MQ_REGISTRO_NAME, O_WRONLY);
    if (mq_registro == (mqd_t)-1)
    {
        mq_close(mq_resp);
        mq_unlink(nombre_resp);
        return -1;
    }

    if (mq_send(mq_registro, (const char *)&msg, sizeof(msg), 0) == -1)
    {
        perror("mq_send(registro estacion)");
        mq_close(mq_registro);
        mq_close(mq_resp);
        mq_unlink(nombre_resp);
        return -1;
    }
    mq_close(mq_registro);

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3; /* timeout 3s para no colgarnos si el servidor no responde */
    if (mq_timedreceive(mq_resp, (char *)&resp, sizeof(resp), NULL, &ts) != -1 &&
        resp.error == 0)
    {
        id = resp.id;
        if (fila_out)
            *fila_out = resp.fila;
        if (col_out)
            *col_out = resp.col;
    }

    mq_close(mq_resp);
    mq_unlink(nombre_resp); /* la estacion no usa esta cola despues del registro */
    return id;
}

// --- HILO DE TRANSACCIONES (CAJERO) ---
void *atender_transacciones(void *arg)
{
    (void)arg;
    MsgTransaccion msg;
    MsgTransaccionResp resp;

    while (!g_salir)
    {
        /* Usamos timedreceive con timeout de 1s para poder revisar g_salir. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        ssize_t leidos = mq_timedreceive(mq_transacciones, (char *)&msg, sizeof(msg), NULL, &ts);

        if (leidos <= 0)
        {
            /* ETIMEDOUT, EINTR u otro error transitorio: volvemos al while. */
            continue;
        }
        // Preparamos la respuesta por defecto
        resp.operacion = msg.operacion;
        resp.error = 0;
        resp.cantidad_efectiva = 0;
        resp.precio_total = 0;

        // ZONA CRÍTICA: Bloqueamos el inventario
        pthread_mutex_lock(&lock);

        // Procesamos la compra/venta
        switch (msg.operacion)
        {
        case OP_VENDER_DEUTERIO:
            stock_deuterio += msg.cantidad;
            combustible += msg.cantidad; /* el deuterio recarga el combustible de la estacion */
            resp.cantidad_efectiva = msg.cantidad;
            resp.precio_total = msg.cantidad * precio_deuterio;
            creditos -= resp.precio_total;
            /* Reflejar el nuevo combustible en la SHM para el radar de las naves. */
            if (mapa_shm != NULL && mi_id_estacion >= 0)
            {
                pthread_mutex_lock(&mapa_shm->mutex);
                mapa_shm->estaciones[mi_id_estacion].combustible = combustible;
                pthread_mutex_unlock(&mapa_shm->mutex);
            }
            printf("[Transacción] Nave %d VENDIÓ %d Deuterio (recarga combustible -> %d). Pagamos: %d\n",
                   msg.pid_nave, msg.cantidad, combustible, resp.precio_total);
            registrar_bitacora(msg.pid_nave, "venta", "deuterio", msg.cantidad, resp.precio_total);
            break;

        case OP_VENDER_MUTEXIO:
            stock_mutexio += msg.cantidad;
            resp.cantidad_efectiva = msg.cantidad;
            resp.precio_total = msg.cantidad * precio_mutexio;
            creditos -= resp.precio_total;
            printf("[Transacción] Nave %d VENDIÓ %d Mutexio. Pagamos: %d\n", msg.pid_nave, msg.cantidad, resp.precio_total);
            registrar_bitacora(msg.pid_nave, "venta", "mutexio", msg.cantidad, resp.precio_total);
            break;

        case OP_VENDER_SEMAFORITA:
            stock_semaforita += msg.cantidad;
            resp.cantidad_efectiva = msg.cantidad;
            resp.precio_total = msg.cantidad * precio_semaforita;
            creditos -= resp.precio_total;
            printf("[Transacción] Nave %d VENDIÓ %d Semaforita. Pagamos: %d\n", msg.pid_nave, msg.cantidad, resp.precio_total);
            registrar_bitacora(msg.pid_nave, "venta", "semaforita", msg.cantidad, resp.precio_total);
            break;

        case OP_VENDER_KERNELIO:
            stock_kernelio += msg.cantidad;
            resp.cantidad_efectiva = msg.cantidad;
            resp.precio_total = msg.cantidad * precio_kernelio;
            creditos -= resp.precio_total;
            printf("[Transacción] Nave %d VENDIÓ %d Kernelio. Pagamos: %d\n", msg.pid_nave, msg.cantidad, resp.precio_total);
            registrar_bitacora(msg.pid_nave, "venta", "kernelio", msg.cantidad, resp.precio_total);
            break;

        case OP_COMPRAR_COMBUSTIBLE:
        {
            /* Limitamos la cantidad por el stock de la estacion Y por el dinero
             * disponible de la nave (no le vendemos mas de lo que puede pagar). */
            int cant = msg.cantidad;
            if (cant > combustible)
                cant = combustible;
            if (precio_combustible > 0 && cant * precio_combustible > msg.dinero_nave)
                cant = msg.dinero_nave / precio_combustible;
            if (cant > 0)
            {
                combustible -= cant;
                resp.cantidad_efectiva = cant;
                resp.precio_total = cant * precio_combustible;
                creditos += resp.precio_total;
                printf("[Transacción] Nave %d COMPRÓ %d Combustible por %d créditos.\n", msg.pid_nave, cant, resp.precio_total);
                registrar_bitacora(msg.pid_nave, "compra", "combustible", cant, resp.precio_total);
            }
            else
            {
                resp.error = 1; // sin stock o sin creditos
                printf("[Alerta] Nave %d no pudo comprar Combustible (sin stock o sin creditos).\n", msg.pid_nave);
            }
            break;
        }

        case OP_COMPRAR_OXIGENO:
        {
            int cant = msg.cantidad;
            if (cant > oxigeno)
                cant = oxigeno;
            if (precio_oxigeno > 0 && cant * precio_oxigeno > msg.dinero_nave)
                cant = msg.dinero_nave / precio_oxigeno;
            if (cant > 0)
            {
                oxigeno -= cant;
                resp.cantidad_efectiva = cant;
                resp.precio_total = cant * precio_oxigeno;
                creditos += resp.precio_total;
                printf("[Transacción] Nave %d COMPRÓ %d Oxígeno por %d créditos.\n", msg.pid_nave, cant, resp.precio_total);
                registrar_bitacora(msg.pid_nave, "compra", "oxigeno", cant, resp.precio_total);
            }
            else
            {
                resp.error = 1; // sin stock o sin creditos
                printf("[Alerta] Nave %d no pudo comprar Oxígeno (sin stock o sin creditos).\n", msg.pid_nave);
            }
            break;
        }
        }

        printf(" -> Caja actual: %d créditos | Oxígeno rest: %d\n", creditos, oxigeno);

        // Liberamos el inventario
        pthread_mutex_unlock(&lock);

        // ENVIAMOS LA RESPUESTA A LA NAVE (a su cola dedicada de transacciones)
        char nombre_cola_resp[MQ_NAVE_TRX_NAME_LEN];
        snprintf(nombre_cola_resp, sizeof(nombre_cola_resp), MQ_NAVE_TRX_FMT, msg.pid_nave);

        mqd_t mq_resp = mq_open(nombre_cola_resp, O_WRONLY);
        if (mq_resp != (mqd_t)-1)
        {
            mq_send(mq_resp, (const char *)&resp, sizeof(resp), 0);
            mq_close(mq_resp);
        }
        else
        {
            printf("[Error] No se pudo abrir la cola de respuesta de la nave %d\n", msg.pid_nave);
        }
    }
    return NULL;
}

void *gasto_combustible(void *arg)
{
    (void)arg; /* el hilo no usa argumentos */
    while (!g_salir)
    {
        /* Respetar el intervalo configurado en config.txt. */
        sleep((unsigned int)intervalo_combustible_seg);

        int lugares_libres = 0;
        if (semaforo_hangar != SEM_FAILED)
            sem_getvalue(semaforo_hangar, &lugares_libres);

        if (lugares_libres < 0)
        {
            lugares_libres = 0; // pongo en 0 para que no sea negativo
        }

        // bloqueamos antes de tocar la variable compartida
        pthread_mutex_lock(&lock);

        combustible -= 10;
        printf("Combustible actual: %d\n", combustible);

        // evaluamos si nos quedamos en cero para apagar todo
        if (combustible <= 0)
        {
            printf("Estacion espacial sin combustible... Desactivando\n");
            /* Reflejar la desactivacion en la SHM para el radar (task #30). */
            if (mapa_shm != NULL && mi_id_estacion >= 0)
            {
                pthread_mutex_lock(&mapa_shm->mutex);
                mapa_shm->estaciones[mi_id_estacion].combustible = 0;
                mapa_shm->estaciones[mi_id_estacion].estado = ESTADO_DESACTIVADO;
                pthread_mutex_unlock(&mapa_shm->mutex);
            }
            pthread_mutex_unlock(&lock);
            break; // rompe el while para ir al exit
        }

        /* Reflejar el combustible (deuterio) actual en la SHM para que el
         * radar de las naves lo muestre al lado de la estacion (task #30). */
        if (mapa_shm != NULL && mi_id_estacion >= 0)
        {
            pthread_mutex_lock(&mapa_shm->mutex);
            mapa_shm->estaciones[mi_id_estacion].combustible = combustible;
            pthread_mutex_unlock(&mapa_shm->mutex);
        }

        // evaluamos si pasamos el umbral de alerta
        if (combustible <= DEFAULT_UMBRAL_COMBUSTIBLE)
        {
            printf("¡ME ESTOY QUEDANDO SIN COMBUSTIBLE!!!!!\n");

            /*
             * Aviso de deuterio a las naves del cuadrante (task #26 -> #30).
             * Usamos el protocolo de ipc.h: leemos los PIDs de las naves
             * registradas desde la SHM y enviamos un MsgAlertaCombustible a
             * la cola privada de cada nave (/cosmikernel_nave_<pid>).
             */
            if (mapa_shm != NULL)
            {
                int pids[MAX_NAVES];
                int n = 0;

                /* Copiamos los PIDs de naves activas bajo el mutex del mapa
                 * (seccion critica corta: solo copiar, no enviar). */
                pthread_mutex_lock(&mapa_shm->mutex);
                for (int i = 0; i < MAX_NAVES; i++)
                {
                    if (mapa_shm->naves[i].pid != 0 &&
                        mapa_shm->naves[i].estado == ESTADO_ACTIVO)
                    {
                        pids[n] = mapa_shm->naves[i].pid;
                        n++;
                    }
                }
                pthread_mutex_unlock(&mapa_shm->mutex);

                MsgAlertaCombustible alerta;
                alerta.id_estacion = mi_id_estacion;
                alerta.pid_estacion = getpid();
                alerta.combustible_actual = combustible;

                for (int i = 0; i < n; i++)
                {
                    char nombre_cola_nave[MQ_NAVE_NAME_LEN];
                    snprintf(nombre_cola_nave, sizeof(nombre_cola_nave),
                             MQ_NAVE_FMT, pids[i]);

                    mqd_t cola_nave = mq_open(nombre_cola_nave, O_WRONLY);
                    if (cola_nave != (mqd_t)-1)
                    {
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

        // soltamos el mutex
        pthread_mutex_unlock(&lock);
    }

    return NULL;
}

int main()
{
    /* Registrar handler SIGUSR1: el servidor nos avisa que se apaga. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manejar_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    /* Cargar config.txt para respetar el intervalo de consumo de combustible. */
    Config cfg;
    if (config_load(CONFIG_PATH, &cfg) == -1)
        fprintf(stderr, "estacion: arrancando con valores por defecto\n");
    intervalo_combustible_seg = cfg.intervalo_combustible_estacion;
    precio_deuterio    = cfg.precio_deuterio;
    precio_mutexio     = cfg.precio_mutexio;
    precio_semaforita  = cfg.precio_semaforita;
    precio_kernelio    = cfg.precio_kernelio;
    precio_combustible = cfg.precio_combustible;
    precio_oxigeno     = cfg.precio_oxigeno;

    /* Abrir (o crear) la bitácora con O_APPEND para escrituras atómicas (README). */
    fd_bitacora = open("bitacora.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_bitacora < 0)
        perror("estacion: no se pudo abrir bitacora.txt");

    // inicializamos el mutex antes de arrancar cualquier hilo
    if (pthread_mutex_init(&lock, NULL) != 0)
    {
        perror("Error al inicializar el mutex");
        return 1;
    }

    /* El semaforo del hangar se crea despues de registrarse, con SEM_HANGAR_FMT(id),
     * para que la nave lo encuentre por el id de la estacion. */

    /* Abrir la SHM del mapa (creada por el servidor) */
    int fd_shm = shm_open(SHM_MAPA_NAME, O_RDWR, 0666);
    if (fd_shm != -1)
    {
        mapa_shm = mmap(NULL, sizeof(Mapa), PROT_READ | PROT_WRITE, MAP_SHARED, fd_shm, 0);
        close(fd_shm);
        if (mapa_shm == MAP_FAILED)
        {
            mapa_shm = NULL;
            fprintf(stderr, "estacion: no se pudo mapear la SHM\n");
        }
    }
    else
    {
        fprintf(stderr, "estacion: SHM no encontrada (servidor no corriendo)\n");
    }

    /* Registrarse con el servidor: nos asigna un slot y posicion en el mapa,
     * y marca la celda como CELDA_ESTACION (asi aparece la 'E' en el radar). */
    if (mapa_shm != NULL)
    {
        int est_fila = -1, est_col = -1;
        mi_id_estacion = registrar_estacion(&est_fila, &est_col);
        if (mi_id_estacion < 0)
        {
            fprintf(stderr, "estacion: no se pudo registrar contra el servidor\n");
        }
        else
        {
            /* Inicializamos nuestro combustible (deuterio) en la SHM. */
            pthread_mutex_lock(&mapa_shm->mutex);
            mapa_shm->estaciones[mi_id_estacion].combustible = combustible;
            pthread_mutex_unlock(&mapa_shm->mutex);
            printf("Estacion registrada con id %d en (%d,%d)\n",
                   mi_id_estacion, est_fila, est_col);

            /* Crear el semaforo contador del hangar con nuestro id (task #23/#44),
             * para que las naves lo abran con SEM_HANGAR_FMT(id). Capacidad 3. */
            snprintf(nombre_semaforo, sizeof(nombre_semaforo), SEM_HANGAR_FMT, mi_id_estacion);
            semaforo_hangar = sem_open(nombre_semaforo, O_CREAT, 0644, 3);
            if (semaforo_hangar == SEM_FAILED)
                perror("estacion: no se pudo crear el semaforo del hangar");
            else
                printf("Hangar (%s) inicializado con 3 espacios.\n", nombre_semaforo);

            // --- INICIO APERTURA COLA DE TRANSACCIONES ---
            struct mq_attr attr_trx;
            attr_trx.mq_flags = 0;
            attr_trx.mq_maxmsg = 10;
            attr_trx.mq_msgsize = sizeof(MsgTransaccion);
            attr_trx.mq_curmsgs = 0;

            // Armamos el nombre usando el formato definido en ipc.h
            snprintf(nombre_mq_transacciones, sizeof(nombre_mq_transacciones), MQ_ESTACION_FMT, mi_id_estacion);
            mq_transacciones = mq_open(nombre_mq_transacciones, O_CREAT | O_RDONLY, 0666, &attr_trx);

            if (mq_transacciones == (mqd_t)-1)
            {
                perror("Error al crear la cola de transacciones");
            }
            else
            {
                // Si la cola se abrió bien, lanzamos el hilo del cajero
                pthread_t hilo_trx;
                if (pthread_create(&hilo_trx, NULL, atender_transacciones, NULL) != 0)
                {
                    perror("Error al crear el hilo de transacciones");
                }
            }
            // ---FIN APERTURA COLA DE TRANSACCIONES ---
        }
    }

    pthread_t estacion1;

    // crea el hilo
    if (pthread_create(&estacion1, NULL, gasto_combustible, NULL) != 0)
    {
        perror("Error al crear el hilo");
        return 1;
    }

    // esperamos a que el hilo termine (esto frena al main para que no se cierre)
    pthread_join(estacion1, NULL);

    if (g_servidor_desconectado)
        printf("Servidor desconectado\n");

    // limpiamos la memoria del mutex antes de apagar el programa
    mq_close(mq_transacciones);
    mq_unlink(nombre_mq_transacciones);
    if (semaforo_hangar != SEM_FAILED)
    {
        sem_close(semaforo_hangar);
        sem_unlink(nombre_semaforo); // borra el semaforo del SO
    }
    pthread_mutex_destroy(&lock);

    if (fd_bitacora >= 0)
        close(fd_bitacora);

    return 0;
}
# CosmiKernel — Documentación de Arquitectura y Diseño de Sistemas Operativos

> Documento de defensa para la presentación.
> Explica **qué** mecanismos de SO usamos, **dónde** están en el código y, sobre
> todo, **por qué** tomamos cada decisión de diseño.

---

## Índice

1. [Visión general en una página](#1-visión-general-en-una-página)
2. [Modelo de procesos (multiproceso, cliente-servidor)](#2-modelo-de-procesos-multiproceso-cliente-servidor)
3. [Modelo de hilos (multihilo)](#3-modelo-de-hilos-multihilo)
4. [Memoria compartida POSIX](#4-memoria-compartida-posix)
5. [Sincronización](#5-sincronización)
6. [Paso de mensajes (colas POSIX)](#6-paso-de-mensajes-colas-posix)
7. [Señales](#7-señales)
8. [Sistema de archivos](#8-sistema-de-archivos)
9. [Ciclo de vida de los recursos IPC (no dejar huérfanos)](#9-ciclo-de-vida-de-los-recursos-ipc-no-dejar-huérfanos)
10. [Flujos clave paso a paso](#10-flujos-clave-paso-a-paso)
11. [Decisiones de diseño y su justificación (el "por qué")](#11-decisiones-de-diseño-y-su-justificación-el-por-qué)
12. [Tabla resumen de mecanismos de SO](#12-tabla-resumen-de-mecanismos-de-so)
13. [Preguntas frecuentes para la defensa](#13-preguntas-frecuentes-para-la-defensa)
14. [Controles del juego (cómo se juega)](#14-controles-del-juego-cómo-se-juega)

---

## 1. Visión general en una página

**CosmiKernel** es un juego de minería espacial escrito en **C sobre Linux/POSIX**.
Su objetivo académico es ejercitar los pilares de un Sistema Operativo:
**procesos, hilos, sincronización, comunicación entre procesos (IPC),
entrada/salida y sistema de archivos**.

La arquitectura es **cliente-servidor con memoria compartida**:

```
                       ┌──────────────────────────────────────┐
                       │          PROCESO SERVIDOR             │
                       │  (autoridad del mapa, 1 solo hilo)    │
                       │                                       │
                       │  • crea la SHM /cosmikernel_mapa      │
                       │  • crea 1920 semáforos de celda       │
                       │  • atiende la cola de registro        │
                       └───────────────┬───────────────────────┘
                                       │ crea / administra
          ┌────────────────────────────┼────────────────────────────┐
          │                            │                            │
          ▼                            ▼                            ▼
  MEMORIA COMPARTIDA            COLAS DE MENSAJES            SEMÁFOROS POSIX
  /cosmikernel_mapa            /cosmikernel_registro        /cosmikernel_cell_*
  (struct Mapa completo)       /cosmikernel_estacion_<id>   /hangar_estacion_<id>
                               /cosmikernel_nave_<pid>
                               /cosmikernel_nave_trx_<pid>
          ▲                            ▲                            ▲
          │  mmap (lectura/escritura)  │  mq_send / mq_receive      │ sem_wait/post
          │                            │                            │
   ┌──────┴───────┐            ┌───────┴────────┐
   │ PROCESO NAVE │  ......     │ PROCESO ESTACIÓN│  ......  (N naves, M estaciones)
   │  5 hilos     │            │  3 hilos        │
   └──────────────┘            └─────────────────┘
```

| Componente | Ejecutable | Rol | Hilos |
|---|---|---|---|
| Servidor | `bin/servidor` | Administra el mapa, asigna posiciones, arbitra altas/bajas | 1 (bucle principal) |
| Nave | `bin/nave` | Cliente jugable; mina, se mueve, comercia | main + 5 |
| Estación | `bin/estacion` | Cliente; compra/vende recursos, recarga naves | main + 2 |

**Idea central:** el **mapa del juego es un único `struct Mapa` que vive en memoria
compartida POSIX**. Todos los procesos lo *ven* directamente (visualización
instantánea, sin pedir datos al servidor). Para que nadie corrompa ese estado
compartido, se protege con un **mutex `PTHREAD_PROCESS_SHARED`** y, para la regla
física "dos naves no pueden ocupar la misma celda", se usa un **semáforo binario
por celda**. Las **transacciones** (compra/venta) y las **alertas** se hacen por
**paso de mensajes** (colas POSIX), nunca tocando memoria ajena.

---

## 2. Modelo de procesos (multiproceso, cliente-servidor)

Hay **tres programas independientes**. Cada nave y cada estación es un **proceso
separado** lanzado por el usuario (ver `run.sh`), no son `fork()` del servidor.
Esto es deliberado: simula clientes que entran y salen del juego en cualquier
momento.

### 2.1 Servidor (`servidor.c`)

Es la **autoridad** del escenario. Su `main` es muy corto y orquesta los módulos:

1. `config_load()` — lee `config.txt`.
2. `shm_crear()` — crea la SHM y el mutex compartido.
3. `asteroides_inicializar()` — siembra los asteroides.
4. `semaforos_crear()` — crea los `MAPA_FILAS × MAPA_COLS` semáforos de celda.
5. `registro_servidor_loop()` — **bucle principal**: atiende altas/bajas.
6. Al recibir `SIGINT`: `apagado_guardar_y_notificar()` + `semaforos_destruir()` + `shm_destruir()`.

<ref_file file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\servidor.c" />

> **Decisión clave:** el servidor es **monohilo**. Procesa la cola de registro en
> un bucle con `mq_timedreceive` (timeout de 1 s). No necesita hilos porque su
> única responsabilidad reactiva es el registro/baja de clientes; *todo el resto
> del estado lo modifican los propios clientes sobre la SHM*. Un solo hilo
> elimina cualquier condición de carrera **dentro** del servidor y simplifica el
> apagado limpio.

El bucle vive en <ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\src\registro.c" lines="315-392" /> y maneja 6 operaciones:

| Operación | Significado |
|---|---|
| `REG_OP_REGISTRAR` | Alta de nave/estación: busca slot libre, asigna posición aleatoria, marca la celda |
| `REG_OP_DESREGISTRAR` | Baja ordenada (libera celda y slot) |
| `REG_OP_DESACTIVAR` | Nave en *game over*: pasa a `'X'` (nave muerta saqueable) |
| `REG_OP_DESACTIVAR_ESTACION` | Estación sin combustible: si todas mueren, fin del juego |
| `REG_OP_DESACTIVAR_ASTEROIDE` | Asteroide agotado: se libera su celda |
| `REG_OP_SAQUEAR_NAVE` | Nave muerta ya saqueada: se libera su celda |

Cada operación se ejecuta **bajo el mutex del mapa** (`pthread_mutex_lock(&mapa->mutex)`),
de modo que las modificaciones estructurales del servidor son atómicas frente a
las de los clientes.

### 2.2 Nave (`nave.c`) — cliente

Flujo de arranque:
1. Carga config, instala manejadores de señal (`SIGINT`, `SIGTERM`, `SIGUSR1`).
2. Abre (no crea) la SHM `/cosmikernel_mapa`. **Si no existe, aborta** (no hay juego sin servidor).
3. Se **registra** contra el servidor → obtiene su `id` (slot en `Mapa.naves[]`).
4. Inicializa ncurses y crea sus colas privadas.
5. Lanza **5 hilos** y espera (`pthread_join`).

### 2.3 Estación (`estacion.c`) — cliente

1. Carga config, instala `SIGUSR1`.
2. Abre la bitácora `bitacora.txt` con `O_APPEND`.
3. Abre la SHM y se **registra** → `id` en `Mapa.estaciones[]`.
4. Crea su **semáforo de hangar** (contador, valor 3) y su **cola de transacciones**.
5. Lanza **2 hilos** (cajero + consumo de combustible) y espera.

---

## 3. Modelo de hilos (multihilo)

El requisito "multihilo" se cumple en los **clientes**. Cada hilo tiene una
responsabilidad única (principio de responsabilidad única → menos puntos de
contención y código más claro).

### 3.1 Hilos de la NAVE (5)

```
                         PROCESO NAVE
   ┌─────────────────────────────────────────────────────────┐
   │  main: registra, inicializa ncurses, lanza y hace join   │
   └───┬──────────┬───────────┬────────────┬──────────┬───────┘
       │          │           │            │          │
       ▼          ▼           ▼            ▼          ▼
   ┌────────┐ ┌─────────┐ ┌─────────┐ ┌────────────┐ ┌────────────┐
   │ radar  │ │ soporte │ │ alertas │ │ propulsión │ │ extracción │
   │ (#27)  │ │ vital   │ │ (#30)   │ │ (#19/#44)  │ │ (#21/#42)  │
   │        │ │ (#20)   │ │         │ │            │ │            │
   │ dibuja │ │ baja O2 │ │ lee SOS │ │ teclado +  │ │ mina aste- │
   │ mapa   │ │ cada T  │ │ de cola │ │ mueve nave │ │ roide /    │
   │ ncurses│ │ seg.    │ │ privada │ │ + hangar + │ │ saquea     │
   │        │ │         │ │         │ │ comercia   │ │ nave muerta│
   └────────┘ └─────────┘ └─────────┘ └────────────┘ └────────────┘
```

| Hilo | Función | Qué hace | Periodicidad |
|---|---|---|---|
| **Radar** | `hilo_radar` | Copia el mapa bajo mutex y lo dibuja con ncurses (mapa + panel + barras de O₂/combustible) | `radar_refresh_ms` (100 ms) |
| **Soporte vital** | `hilo_soporte_vital` | Decrementa el oxígeno; si llega a 0 → desactiva la nave y avisa al servidor | `intervalo_oxigeno_nave` (2 s) |
| **Alertas** | `hilo_alertas` | Escucha `/cosmikernel_nave_<pid>` y guarda el último SOS de combustible | bloqueante con timeout 1 s |
| **Propulsión** | `hilo_propulsion` | **Único lector del teclado** (`getch`): gira/mueve la nave, entra al hangar, dispara compras/ventas | poll 16 ms (~60 Hz) |
| **Extracción** | `hilo_extraccion` | Cuando propulsión levanta la bandera `g_extraer`, extrae el asteroide o saquea la nave muerta de enfrente | poll 50 ms |

> **Decisión clave (teclado en un solo hilo):** sólo **propulsión** llama a
> `getch()`. ncurses no es *thread-safe* para entrada concurrente. Para la acción
> de minar, propulsión **no** mina directamente: levanta una bandera
> `g_extraer` (`volatile sig_atomic_t`) que el hilo de extracción consume. Así se
> separa "leer input" de "trabajo pesado de extracción" sin dos hilos peleando
> por el teclado.
> Ver <ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\nave.c" lines="1137-1141" />.

### 3.2 Hilos de la ESTACIÓN (2)

| Hilo | Función | Qué hace |
|---|---|---|
| **Cajero** | `atender_transacciones` | Bloquea en `/cosmikernel_estacion_<id>`, procesa compra/venta bajo el mutex `lock`, responde a la nave y registra en bitácora |
| **Consumo** | `gasto_combustible` | Cada `intervalo_combustible_estacion` baja el combustible; si cruza el umbral envía SOS a todas las naves; si llega a 0 desactiva la estación |

<ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\estacion.c" lines="173-321" />

---

## 4. Memoria compartida POSIX

### 4.1 Qué se comparte

**Todo el estado del juego** en un único objeto `Mapa` (`include/mapa.h`):

```c
typedef struct {
    Celda     celdas[MAPA_FILAS][MAPA_COLS];   // 24 × 80 = 1920 celdas
    Nave      naves[MAX_NAVES];                // 8 naves
    Estacion  estaciones[MAX_ESTACIONES];      // 3 estaciones
    Asteroide asteroides[MAX_ASTEROIDES];      // 20 asteroides
    int       num_naves, num_estaciones, num_asteroides;
    int       juego_activo;
    pthread_mutex_t mutex;        // ← PROCESS_SHARED, vive DENTRO de la SHM
} Mapa;
```

Cada `Celda` guarda su `tipo` (`' '`, `^`, `#`, `@`, `X`) y el `idx` de la entidad
que la ocupa. El **carácter de dibujo y el dato lógico viven juntos**, así
cualquier proceso puede pintar el mapa sin traducciones.

### 4.2 Cómo se crea (sólo el servidor)

<ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\src\shm.c" lines="15-101" />

1. `shm_open(O_CREAT|O_EXCL)` — si quedó una corrida anterior, hace `shm_unlink` y reintenta (idempotencia).
2. `ftruncate(fd, sizeof(Mapa))` — fija el tamaño.
3. `mmap(MAP_SHARED)` — mapea el objeto al espacio de direcciones.
4. `memset` a cero + inicializa celdas vacías.
5. **Inicializa el mutex como `PTHREAD_PROCESS_SHARED`** (`pthread_mutexattr_setpshared`). Esto es lo que permite que un `pthread_mutex_t` funcione **entre procesos** y no sólo entre hilos del mismo proceso.

Los clientes hacen `shm_open(O_RDWR)` (sin `O_CREAT`) y `mmap`. Si la SHM no
existe, abortan: <ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\nave.c" lines="179-205" />.

> **Decisión clave (¿por qué SHM y no todo por mensajes?):** la **visualización**
> del mapa necesita leerse decenas de veces por segundo (radar a 100 ms × N
> naves). Si cada refresco fuese un mensaje al servidor, el servidor sería un
> cuello de botella y la latencia se notaría. Con SHM, **leer el mapa es un
> acceso a memoria local**: O(1), sin syscalls de IPC. La SHM es ideal para
> *estado compartido de alta frecuencia de lectura*.

> **Decisión clave (mutex dentro de la SHM):** el mutex se coloca **adentro** del
> `struct Mapa`, no como objeto aparte. Como la SHM se mapea en todos los
> procesos, el mutex es visible para todos en la misma dirección relativa. El
> servidor lo inicializa una sola vez al crear la SHM.

---

## 5. Sincronización

Usamos **cuatro niveles** de sincronización, cada uno para un problema distinto.
Esta es la parte más importante para la materia.

```
NIVEL 1  Mutex global del mapa (PROCESS_SHARED)  → integridad del struct Mapa
NIVEL 2  Semáforo binario por celda              → exclusión de posición física
NIVEL 3  Semáforo contador por hangar (=3)       → aforo de 3 naves por estación
NIVEL 4  Mutex locales por proceso               → estado interno de cada cliente
```

### 5.1 Nivel 1 — Mutex global `PROCESS_SHARED` (el "candado grande")

- **Qué protege:** *cualquier* lectura/escritura consistente del `struct Mapa`
  (celdas, naves, estaciones, asteroides, contadores).
- **Quién lo usa:** los tres tipos de proceso.
- **Granularidad:** **gruesa** (un solo candado para toda la estructura).

> **¿Por qué un candado grueso y no uno por arreglo?** Las modificaciones suelen
> tocar **varias zonas a la vez y deben ser atómicas en conjunto**. Ejemplo: al
> mover una nave hay que (a) cambiar `naves[id].fila/col`, (b) poner la celda
> destino en `CELDA_NAVE` y (c) limpiar la celda origen. Esas tres escrituras
> deben verse como **una sola operación atómica**; si hubiese candados separados
> por arreglo habría que tomarlos todos en orden y aparecería riesgo de
> *deadlock*. Un único mutex hace el invariante trivial de mantener. El costo
> (menor paralelismo) es irrelevante porque las secciones críticas son
> microscópicas (unas pocas asignaciones).

> **Patrón "snapshot bajo mutex":** el radar **no** dibuja con el candado tomado.
> Copia el mapa a variables locales dentro de la sección crítica y **suelta el
> mutex antes** de hacer el I/O lento de ncurses. Así el render (que es lento) no
> bloquea al servidor ni a las otras naves.
> Ver <ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\nave.c" lines="500-507" />.

### 5.2 Nivel 2 — Semáforo binario por celda (exclusión de posición)

- **Qué es:** un semáforo POSIX **nombrado por cada celda**:
  `/cosmikernel_cell_RRR_CCC`. Son `24 × 80 = 1920` semáforos, todos inicializados
  en **1** (celda libre). Los crea el servidor (`semaforos_crear`).
- **Invariante:** una nave mantiene **tomado (valor 0)** el semáforo de la celda
  que ocupa. Al moverse: `sem_trywait(destino)` → si lo consigue, actualiza la
  SHM y hace `sem_post(origen)`.

<ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\nave.c" lines="1296-1342" />

> **Decisión clave (`sem_trywait` y no `sem_wait`):** si la nave A quiere ir a la
> celda de B y B quiere ir a la de A **al mismo tiempo**, con `sem_wait`
> (bloqueante) cada una esperaría el semáforo que tiene la otra → **deadlock
> clásico (espera circular)**. Con `sem_trywait` (no bloqueante), "si no puedo
> tomar la celda, simplemente no me muevo". Se elimina la condición de Coffman de
> *hold-and-wait* y nunca hay abrazo mortal. Es exactamente lo que pide el
> enunciado: *"el sistema no debe generar abrazos mortales"*.

> **¿Por qué semáforos nombrados y no un arreglo de mutex en la SHM?** Porque la
> propiedad de la celda (la posición) debe ser respetada **entre procesos** y de
> forma independiente del candado de estructura. Un semáforo nombrado por celda
> modela perfectamente "este lugar físico es un recurso que a lo sumo uno toma".

### 5.3 Nivel 3 — Semáforo contador del hangar (aforo = 3)

- **Qué es:** un semáforo POSIX **contador** por estación, `/hangar_estacion_<id>`,
  inicializado en **3**. Lo crea la estación al registrarse.
- **Uso:** la nave hace `sem_trywait` para **entrar** al hangar (decrementa) y
  `sem_post` para **salir** (incrementa). Si el contador está en 0, el hangar está
  lleno y la nave recibe "Hangar lleno".

Creación (estación): <ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\estacion.c" lines="506-513" />
Entrada (nave): <ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\nave.c" lines="1246-1283" />

> **Decisión clave:** es el caso de libro del **semáforo contador para un recurso
> con capacidad limitada** (el aforo de 3 plazas que pide el enunciado). El valor
> inicial *es* la capacidad. No hace falta contar a mano cuántas naves hay: el
> propio semáforo es la cuenta.

### 5.4 Nivel 4 — Mutex locales de cada proceso

Protegen estado **interno** del proceso (que no está en la SHM):

| Proceso | Mutex | Protege |
|---|---|---|
| Nave | `g_alerta.mutex` | Última alerta SOS recibida (escribe hilo alertas, lee radar) |
| Nave | `g_hangar.mutex` | Estado del hangar actual y mensajes (escribe propulsión, lee radar) |
| Estación | `lock` | Inventario y caja (combustible, oxígeno, créditos, stock) |
| Estación | `mutex_bitacora` | Serializa escrituras en `bitacora.txt` |

> **¿Por qué mutex aparte y no el mutex de la SHM?** Porque ese estado **no se
> comparte entre procesos**, vive sólo en el proceso. Usar el mutex global de la
> SHM para esto sería tomar un candado entre-procesos para datos privados:
> contención innecesaria. Cada candado protege exactamente su dominio.

---

## 6. Paso de mensajes (colas POSIX)

Las **transacciones y avisos** se hacen por **colas de mensajes POSIX** (`mq_*`),
**no** tocando memoria del otro proceso. Esto cumple el requisito del enunciado y
da un acoplamiento débil (request/response).

| Cola | Quién la crea | Quién lee | Quién escribe | Para qué |
|---|---|---|---|---|
| `/cosmikernel_registro` | Servidor | Servidor | Naves y estaciones | Alta/baja/desactivación (8 msg máx.) |
| `/cosmikernel_estacion_<id>` | Estación | Estación (cajero) | Naves en el hangar | Compra/venta (`MsgTransaccion`) |
| `/cosmikernel_nave_<pid>` | Nave | Nave (hilo alertas) | Estaciones; servidor (resp. de registro) | SOS de combustible (`MsgAlertaCombustible`) |
| `/cosmikernel_nave_trx_<pid>` | Nave | Nave (propulsión) | Estación (cajero) | **Respuesta** de la transacción (`MsgTransaccionResp`) |

El protocolo (estructuras de mensaje y nombres) está centralizado en
<ref_file file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\include\ipc.h" />,
lo que evita que cada proceso "invente" su propio formato.

> **Decisión clave (dos colas separadas en la nave):** la nave tiene una cola para
> **alertas** (`_nave_`) y otra para **respuestas de transacción**
> (`_nave_trx_`). Son tipos de mensaje distintos (`MsgAlertaCombustible` vs
> `MsgTransaccionResp`), los leen **hilos distintos** (alertas vs propulsión) y
> tienen semánticas distintas. Mezclarlos en una sola cola obligaría a
> discriminar por tamaño/tipo y a que un hilo robe mensajes del otro. Separarlas
> = cada consumidor lee sólo lo suyo.

> **Decisión clave (`mq_timedreceive` con timeout de 1 s en los bucles):** ningún
> hilo se bloquea para siempre en una cola. Cada ~1 s vuelve a chequear la
> bandera de salida (`g_salir`/`g_stop`). Esto permite un **apagado limpio**:
> cuando se pide salir, los hilos no quedan colgados esperando un mensaje que
> quizá nunca llegue. Ver el bucle del cajero en
> <ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\estacion.c" lines="179-191" />.

### Validación de la transacción (servidor de la regla de negocio: la estación)

La **estación es la autoridad** de la compra/venta: valida stock y dinero de la
nave antes de aceptar, ajusta cantidades, y sólo entonces responde. La nave
aplica el resultado a su inventario en la SHM **sólo si `resp.error == 0`**.
Ver validación en <ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\estacion.c" lines="249-273" />.

---

## 7. Señales

| Señal | Receptor | Handler | Efecto |
|---|---|---|---|
| `SIGINT` / `SIGTERM` | Servidor | `on_signal_stop` | Levanta `g_stop` → sale del bucle → guarda estado y limpia IPC |
| `SIGINT` / `SIGTERM` | Nave | `manejar_sigint` | Levanta `g_salir` → join de hilos → cleanup |
| `SIGUSR1` | Nave | `manejar_sigusr1` | El servidor avisa *fin de juego*: muestra **GAME OVER** (no mata el proceso) |
| `SIGUSR1` | Estación | `manejar_sigusr1` | El servidor se apagó: salida ordenada |

El servidor **envía `SIGUSR1` con `kill()`** a todos los clientes activos cuando:
- todas las estaciones se quedan sin combustible (`notificar_fin_juego`), o
- el servidor se apaga normalmente (`apagado_guardar_y_notificar` → `notificar_clientes`).

<ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\src\apagado.c" lines="47-63" />

> **Decisión clave (banderas `volatile sig_atomic_t`):** los handlers de señal
> **sólo** escriben una bandera atómica; **no** hacen `printf`, `malloc`, ni tocan
> estructuras complejas (no son *async-signal-safe*). El trabajo real
> (mostrar GAME OVER, guardar) lo hace el hilo principal cuando ve la bandera.
> Este es el patrón correcto y seguro de manejo de señales.

---

## 8. Sistema de archivos

| Archivo | Proceso | Modo | Propósito |
|---|---|---|---|
| `config.txt` | Todos | lectura | Configuración inicial (estaciones, asteroides, precios, intervalos) |
| `bitacora.txt` | Estación | `O_APPEND` | Registro **atómico** de cada compra/venta |
| `estado_mapa.txt` | Servidor | escritura | *Snapshot* del estado al apagar |

### 8.1 Configuración (`config.c`)
`config_load` aplica **valores por defecto**, parsea `clave = valor`, ignora
comentarios y luego **valida rangos** (`config_validate`), corrigiendo lo
inválido. Es robusto: si falta el archivo, el juego arranca igual con defaults.

### 8.2 Bitácora atómica (`estacion.c`)
> **Decisión clave (escritura atómica):** la bitácora se abre con `O_APPEND` y se
> escribe con un único `write()`. En POSIX, un `write` a un archivo abierto con
> `O_APPEND` es **atómico**: el desplazamiento al final y la escritura ocurren sin
> que otro proceso pueda intercalarse. Además se protege con `mutex_bitacora`
> entre los hilos de la misma estación. Por eso la bitácora **nunca se corrompe ni
> se entremezclan líneas**, justo lo que exige el enunciado.
> Ver <ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\estacion.c" lines="73-91" />.

### 8.3 Persistencia al apagar (`apagado.c`)
Al cerrar normalmente, el servidor **primero notifica** a los clientes (para que
reaccionen cuanto antes) y **después** vuelca el estado completo a
`estado_mapa.txt` (asteroides, naves, estaciones). Cumple "si el servidor termina
normalmente, guarda su estado y avisa a los clientes".

---

## 9. Ciclo de vida de los recursos IPC (no dejar huérfanos)

Un criterio de evaluación explícito es **no dejar recursos huérfanos**. La regla
de diseño es: **quien crea, destruye**.

| Recurso | Lo crea | Lo destruye (`unlink`) |
|---|---|---|
| SHM `/cosmikernel_mapa` | Servidor (`shm_crear`) | Servidor (`shm_destruir`) |
| 1920 semáforos de celda | Servidor (`semaforos_crear`) | Servidor (`semaforos_destruir`) |
| Cola de registro | Servidor | Servidor |
| Semáforo de hangar | Estación | Estación (`sem_unlink` al salir) |
| Cola de transacciones de estación | Estación | Estación |
| Colas privadas de nave (`_nave_`, `_nave_trx_`) | Nave | Nave (`mq_unlink` en cleanup) |

Detalles que evitan huérfanos:
- El servidor hace `shm_unlink`/`sem_unlink` **antes de recrear** por si una
  corrida anterior se cayó (idempotencia defensiva).
- El cliente **abre** (no crea) los recursos del servidor y sólo cierra su
  *descriptor* (`sem_close`, `mq_close`), nunca hace `unlink` de lo ajeno.
- `run.sh stop` limpia `/dev/shm/cosmikernel_*` y `/dev/mqueue/cosmikernel_*` como
  red de seguridad final.

> **Caso especial — naves muertas:** `procesar_desregistrar` **a propósito NO
> elimina** una nave en estado `DESACTIVADO`. Una nave muerta (`'X'`) debe
> permanecer en el mapa para que otra la saquee (regla del juego). Sólo se libera
> con la operación dedicada `REG_OP_SAQUEAR_NAVE`, después de que alguien la
> saqueó. Ver <ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\src\registro.c" lines="185-195" />.

---

## 10. Flujos clave paso a paso

### 10.1 Registro de un cliente

```
NAVE                                  SERVIDOR
 │ crea cola privada /nave_<pid>        │ (bloqueado en mq_timedreceive
 │                                      │  sobre /cosmikernel_registro)
 │ mq_send(MsgRegistro REGISTRAR) ─────►│
 │                                      │ lock(mutex)
 │                                      │  busca slot libre + posición libre
 │                                      │  escribe naves[slot], marca celda
 │                                      │ unlock(mutex)
 │ ◄──── mq_send(MsgRegistroResp) ──────│ responde a la cola privada
 │ guarda id, inicializa O2/combustible │
 ▼ lanza hilos                          ▼ vuelve a esperar
```

### 10.2 Movimiento de la nave (con semáforos de celda)

```
tecla 'w' → propulsión calcula (nueva_fila, nueva_col)  [mapa toroidal]
  ├─ ¿destino es ESTACIÓN? → intentar entrar al hangar (sem_trywait contador)
  └─ ¿destino libre y hay combustible?
        sem_trywait(sem_celda_destino)
          ├─ falla → la celda está ocupada → NO se mueve (sin deadlock)
          └─ ok → lock(mutex): mueve nave + actualiza celdas + gasta combustible
                  unlock(mutex)
                  sem_post(sem_celda_origen)   ← libera la celda anterior
```

### 10.3 Transacción (venta/compra en el hangar)

```
NAVE (propulsión)                      ESTACIÓN (cajero)
 │ mq_send(MsgTransaccion) ───────────►│ lock(lock)
 │   a /cosmikernel_estacion_<id>       │  ajusta stock/caja, valida dinero
 │                                      │  registrar_bitacora()  (O_APPEND)
 │                                      │ unlock(lock)
 │ ◄── mq_send(MsgTransaccionResp) ─────│ a /cosmikernel_nave_trx_<pid>
 │ si error==0: lock(mutex SHM)         │
 │   aplica al inventario de la nave    │
 │ unlock(mutex SHM)                    │
```

### 10.4 Alerta SOS de combustible

```
ESTACIÓN (gasto_combustible): combustible <= umbral
  └─ lock(mutex SHM): copia PIDs de naves activas → unlock
     para cada PID: mq_send(MsgAlertaCombustible) a /cosmikernel_nave_<pid>
                          │
NAVE (hilo alertas) ◄─────┘ guarda en g_alerta (bajo g_alerta.mutex)
RADAR lo muestra como "Estación #k pide DEUTERIO!"
```

### 10.5 Game over / fin del juego

```
O2 o combustible de una nave llega a 0
  → la nave se marca DESACTIVADA en la SHM + REG_OP_DESACTIVAR
  → servidor pinta la celda como 'X' (saqueable)
  → la nave NO termina: muestra overlay GAME OVER (sigue viéndose el mapa)

Todas las estaciones sin combustible
  → REG_OP_DESACTIVAR_ESTACION → si activas==0: juego_activo=0
  → servidor envía SIGUSR1 a todas las naves y termina
```

---

## 11. Decisiones de diseño y su justificación (el "por qué")

Resumen de las decisiones que conviene saber defender:

1. **Cliente-servidor + SHM (no todo por mensajes).** El mapa es estado de
   **lectura muy frecuente** (render). SHM = lectura local O(1); el servidor no es
   cuello de botella. Los mensajes se reservan para eventos discretos
   (transacciones, alertas, altas/bajas).

2. **Servidor monohilo.** Su única tarea reactiva es el registro. Un hilo →
   cero condiciones de carrera internas y apagado trivial. El estado del juego lo
   mutan los clientes sobre la SHM, no el servidor.

3. **Candado grueso (un mutex para todo el `Mapa`).** Las operaciones tocan varias
   estructuras a la vez y deben ser atómicas en conjunto; un solo mutex hace
   imposible el deadlock por orden de adquisición y mantiene los invariantes
   simples. Secciones críticas mínimas ⇒ el costo de paralelismo es despreciable.

4. **Mutex `PROCESS_SHARED` dentro de la SHM.** Es la forma POSIX estándar de
   sincronizar **hilos de procesos distintos** sobre memoria compartida.

5. **Semáforo binario por celda + `sem_trywait`.** Modela "una celda, un ocupante"
   y, al ser no bloqueante, **elimina el deadlock** del intercambio de celdas
   entre dos naves (rompe *hold-and-wait*).

6. **Semáforo contador (=3) para el hangar.** El patrón canónico de recurso con
   aforo limitado; el valor del semáforo *es* la cantidad de plazas.

7. **Mutex locales por proceso.** Datos privados se protegen con candados privados
   → no se contamina el candado global entre procesos.

8. **Dos colas por nave (alertas vs respuestas).** Separar tipos de mensaje que
   leen hilos distintos evita robos de mensajes y discriminación por tamaño.

9. **`mq_timedreceive` con timeout.** Permite chequear la bandera de salida y
   apagar limpio; nunca un hilo colgado para siempre.

10. **Handlers de señal mínimos (`sig_atomic_t`).** Sólo levantan banderas; el
    trabajo lo hace el hilo principal. Cumple *async-signal-safety*.

11. **Bitácora con `O_APPEND` + mutex.** Escrituras atómicas ⇒ la bitácora nunca
    se corrompe ni se entremezcla (criterio de evaluación).

12. **"Quien crea, destruye" + `unlink` defensivo.** Ningún recurso IPC queda
    huérfano; el arranque tolera corridas previas que se cayeron.

13. **Nave muerta persistente.** No se borra al desregistrar: queda como `'X'`
    saqueable hasta que otra nave la vacíe (regla del juego, con operación de IPC
    dedicada).

14. **Game over "no fatal".** La nave incapacitada no cierra el proceso: muestra
    GAME OVER y mantiene el mapa visible; el servidor conserva sus recursos para
    que otros los aprovechen.

---

## 12. Tabla resumen de mecanismos de SO

| Pilar de SO | Mecanismo concreto | Dónde |
|---|---|---|
| **Procesos** | 3 ejecutables independientes (cliente-servidor) | `servidor.c`, `nave.c`, `estacion.c` |
| **Hilos** | `pthread` (nave: 5, estación: 2) | `pthread_create` en ambos clientes |
| **Memoria compartida** | `shm_open` + `mmap(MAP_SHARED)` | `src/shm.c` |
| **Mutex entre procesos** | `pthread_mutex_t` + `PTHREAD_PROCESS_SHARED` | `Mapa.mutex` |
| **Mutex entre hilos** | `pthread_mutex_t` local | `lock`, `g_alerta.mutex`, `g_hangar.mutex`, `mutex_bitacora` |
| **Semáforo binario** | `sem_open` valor 1, por celda | `src/semaforos.c` |
| **Semáforo contador** | `sem_open` valor 3, por hangar | `estacion.c` / `nave.c` |
| **Paso de mensajes** | colas POSIX `mq_*` | `include/ipc.h` + clientes |
| **Señales** | `sigaction` (`SIGINT`, `SIGTERM`, `SIGUSR1`) + `kill` | todos |
| **E/S** | ncurses (radar), teclado no bloqueante | `nave.c` |
| **Sistema de archivos** | `config.txt`, `bitacora.txt` (`O_APPEND`), `estado_mapa.txt` | `config.c`, `estacion.c`, `apagado.c` |
| **Prevención de deadlock** | `sem_trywait` (no bloqueante) | movimiento e ingreso a hangar |

Parámetros (de `include/config.h` y `config.txt`):
`MAPA_FILAS=24`, `MAPA_COLS=80`, `MAX_NAVES=8`, `MAX_ESTACIONES=3`,
`MAX_ASTEROIDES=20`, oxígeno/combustible inicial de nave = 100,
combustible inicial de estación = 2000.

---

## 13. Preguntas frecuentes para la defensa

**P: ¿Por qué no usan `fork()` para crear las naves?**
R: Las naves y estaciones son **clientes que entran y salen libremente**; el
usuario los lanza cuando quiere (`run.sh`). Son procesos verdaderamente
independientes que se conectan al servidor por IPC, no hijos suyos. Esto modela
mejor un sistema cliente-servidor real.

**P: ¿Cómo evitan los abrazos mortales (deadlock)?**
R: (1) Un **único mutex** para el mapa ⇒ no hay orden de adquisición que invertir.
(2) En el movimiento y al entrar al hangar usamos **`sem_trywait`** (no
bloqueante): si no se puede tomar el recurso, no se espera, se reintenta luego.
Eso rompe la condición de *hold-and-wait*.

**P: ¿Qué pasa si dos naves quieren la misma celda a la vez?**
R: Sólo una gana el `sem_trywait` del semáforo de esa celda (valor 1→0). La otra
falla y no se mueve. Imposible que ambas la ocupen.

**P: ¿Por qué el mutex está dentro de la SHM?**
R: Para que sea visible por todos los procesos. Se inicializa con el atributo
`PTHREAD_PROCESS_SHARED`, que es lo que habilita un mutex de pthreads para
funcionar entre procesos distintos y no sólo entre hilos.

**P: ¿Cómo garantizan que la bitácora no se corrompa?**
R: `O_APPEND` hace atómico el "ir al final + escribir" a nivel de SO, y además un
mutex serializa los hilos de la estación. Cada `write()` escribe una línea
completa.

**P: ¿El radar bloquea a todos mientras dibuja?**
R: No. Copia el mapa **bajo el mutex** (sección crítica de microsegundos) y suelta
el candado **antes** de dibujar con ncurses (lo lento). El servidor y las demás
naves nunca esperan por el render.

**P: Si una nave muere, ¿por qué no desaparece?**
R: Queda como `'X'` saqueable (regla del juego). El servidor la conserva; sólo se
libera cuando otra nave la saquea, mediante una operación de IPC dedicada
(`REG_OP_SAQUEAR_NAVE`).

**P: ¿Qué se comunica por SHM y qué por mensajes?**
R: Por **SHM** va el *estado continuo* del mapa (posiciones, recursos, niveles).
Por **mensajes** van los *eventos discretos*: registro, transacciones y alertas.
SHM = compartir estado de alta frecuencia; colas = pedir/responder acciones.

---

## 14. Controles del juego (cómo se juega)

Toda la entrada de la nave la maneja el **hilo de propulsión** (único lector del
teclado con `getch()` no bloqueante). Estos son los controles reales tal como
están en el código <ref_snippet file="C:\Users\Ignacio\Desktop\escritorio\SO\laboratorio4\lab04-super-awesome-team\nave.c" lines="1107-1220" />.

### 14.1 Símbolos del mapa

| Símbolo | Significado | Constante |
|:---:|---|---|
| `^` `>` `v` `<` | Tu nave (apunta arriba/derecha/abajo/izquierda). La propia se resalta | `CELDA_NAVE` + `simbolo_nave()` |
| `#` | Estación espacial (acercate para entrar al hangar) | `CELDA_ESTACION` |
| `@` | Asteroide con recursos | `CELDA_ASTEROIDE` |
| `X` | Nave muerta (saqueable) | `CELDA_NAVE_MUERTA` |
| (espacio) | Espacio profundo vacío | `CELDA_VACIA` |

### 14.2 Movimiento (siempre disponible)

| Tecla | Alternativa | Acción | Costo |
|:---:|:---:|---|---|
| `w` / `W` | `↑` | **Avanzar** en la dirección a la que apunta la nave | −1 combustible |
| `s` / `S` | `↓` | **Retroceder** (marcha atrás) | −1 combustible |
| `a` / `A` | `←` | **Girar** a la izquierda (no se mueve, sólo rota) | gratis |
| `d` / `D` | `→` | **Girar** a la derecha (no se mueve, sólo rota) | gratis |

> **Importante:** primero se *gira* con `a`/`d` y luego se *avanza* con `w`. La
> nave se mueve hacia donde apunta la punta (`^ > v <`).
> El mapa es **toroidal**: si salís por un borde, aparecés por el opuesto.

### 14.3 Acción: minar o saquear (siempre disponible)

| Tecla | Acción | Costo |
|:---:|---|---|
| `e` / `E` | Actúa sobre la celda **de enfrente**: si es un asteroide `@` lo **mina** (sus recursos pasan a tu inventario); si es una nave muerta `X` la **saquea** (te llevás sus minerales + combustible + oxígeno + créditos) | extraer asteroide: −5 combustible; saquear: gratis |

### 14.4 Comercio en el hangar (sólo dentro de una estación)

Para **entrar al hangar**: avanzá (`w`) **empujando contra una estación `#`**. No
te movés sobre ella; tomás una de sus 3 plazas (semáforo contador). Para **salir**
simplemente movete a otra celda. Estando dentro, el panel muestra estas opciones:

| Tecla | Acción |
|:---:|---|
| `f` / `F` | **Comprar combustible** (+10 unidades, paga con tus créditos) |
| `o` / `O` | **Comprar oxígeno** (+10 unidades, paga con tus créditos) |
| `1` | **Vender** todo tu **deuterio** |
| `2` | **Vender** todo tu **mutexio** |
| `3` | **Vender** toda tu **semaforita** |
| `4` | **Vender** todo tu **kernelio** |

> Ganás créditos vendiendo minerales y los gastás comprando combustible/oxígeno.
> La nave arranca con 0 créditos: hay que **minar y vender** para poder reabastecerse.

### 14.5 Salir

| Tecla | Acción |
|:---:|---|
| `Ctrl + C` | Cierra la nave de forma ordenada (libera celda, hangar y colas) |

### 14.6 Objetivo y "game over"

- Cuidá el **combustible** (lo gastás al moverte y al minar) y el **oxígeno** (baja
  solo con el tiempo, cada `intervalo_oxigeno_nave` segundos).
- Si cualquiera llega a **0**, la tripulación queda incapacitada: aparece
  **GAME OVER**, tu nave pasa a `X` y otra nave puede saquearte. El proceso **no se
  cierra solo**: seguís viendo el mapa hasta que salís con `Ctrl+C`.
- El juego entero termina si **todas las estaciones** se quedan sin combustible
  (por eso conviene venderles deuterio cuando mandan el SOS).

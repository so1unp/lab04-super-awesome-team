CC=gcc
BIN=./bin
SRC=./src
CFLAGS=-std=gnu99 -g -Wall -Wextra -Wshadow -Wconversion -Wunreachable-code -Iinclude

# Librerias necesarias:
#  -lpthread : hilos POSIX (todos los procesos usan pthread_mutex/threads)
#  -lrt      : memoria compartida y colas de mensaje POSIX (shm_*, mq_*)
#  -lncurses : interfaz de la nave (hilo radar)
LDLIBS_COMMON=-lpthread -lrt
LDLIBS_NAVE=$(LDLIBS_COMMON) -lncurses

PROG=nave estacion servidor

LIST=$(addprefix $(BIN)/, $(PROG))

# Objetos compartidos compilados desde src/
SHARED_OBJS=$(SRC)/config.o $(SRC)/shm.o $(SRC)/asteroides.o $(SRC)/registro.o $(SRC)/apagado.o $(SRC)/semaforos.o

# Headers: si cambia cualquiera (p.ej. config.h con el tamano del mapa) se
# recompila todo lo que dependa de ellos.
HEADERS=$(wildcard include/*.h)

.PHONY: all
all: $(LIST)

# Objetos de src/ (dependen de los headers)
$(SRC)/%.o: $(SRC)/%.c $(HEADERS)
	$(CC) -c -o $@ $< $(CFLAGS)

# En los binarios, $(filter-out ...) saca los .h del comando de gcc
# (estan como dependencia solo para forzar la recompilacion, no para linkear).
$(BIN)/servidor: servidor.c $(SHARED_OBJS) $(HEADERS)
	$(CC) -o $@ $(filter-out $(HEADERS),$^) $(CFLAGS) $(LDLIBS_COMMON)

$(BIN)/nave: nave.c $(SHARED_OBJS) $(HEADERS)
	$(CC) -o $@ $(filter-out $(HEADERS),$^) $(CFLAGS) $(LDLIBS_NAVE)

$(BIN)/estacion: estacion.c $(SHARED_OBJS) $(HEADERS)
	$(CC) -o $@ $(filter-out $(HEADERS),$^) $(CFLAGS) $(LDLIBS_COMMON)

test:
	@./test.sh ||:

.PHONY: clean
clean:
	rm -f $(LIST) $(SHARED_OBJS)

zip:
	git archive --format zip --output ${USER}-lab04.zip HEAD

html:
	pandoc -o README.html -f gfm README.md

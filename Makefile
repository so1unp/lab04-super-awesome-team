CC=gcc
BIN=./bin
SRC=./src
CFLAGS=-g -Wall -Wextra -Wshadow -Wconversion -Wunreachable-code -Iinclude

# Librerias necesarias:
#  -lpthread : hilos POSIX (todos los procesos usan pthread_mutex/threads)
#  -lrt      : memoria compartida y colas de mensaje POSIX (shm_*, mq_*)
#  -lncurses : interfaz de la nave (hilo radar)
LDLIBS_COMMON=-lpthread -lrt
LDLIBS_NAVE=$(LDLIBS_COMMON) -lncurses

PROG=nave estacion servidor

LIST=$(addprefix $(BIN)/, $(PROG))

# Objetos compartidos compilados desde src/
SHARED_OBJS=$(SRC)/config.o

.PHONY: all
all: $(LIST)

# Objetos de src/
$(SRC)/%.o: $(SRC)/%.c
	$(CC) -c -o $@ $< $(CFLAGS)

$(BIN)/servidor: servidor.c $(SHARED_OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDLIBS_COMMON)

$(BIN)/nave: nave.c $(SHARED_OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDLIBS_NAVE)

$(BIN)/estacion: estacion.c $(SHARED_OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDLIBS_COMMON)

test:
	@./test.sh ||:

.PHONY: clean
clean:
	rm -f $(LIST) $(SHARED_OBJS)

zip:
	git archive --format zip --output ${USER}-lab04.zip HEAD

html:
	pandoc -o README.html -f gfm README.md

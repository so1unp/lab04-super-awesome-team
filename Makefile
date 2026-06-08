CC=gcc
BIN=./bin
SRC=./src
CFLAGS=-g -Wall -Wextra -Wshadow -Wconversion -Wunreachable-code -Iinclude

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
	$(CC) -o $@ $^ $(CFLAGS)

$(BIN)/nave: nave.c $(SHARED_OBJS)
	$(CC) -o $@ $^ $(CFLAGS)

$(BIN)/estacion: estacion.c $(SHARED_OBJS)
	$(CC) -o $@ $^ $(CFLAGS)

test:
	@./test.sh ||:

.PHONY: clean
clean:
	rm -f $(LIST) $(SHARED_OBJS)

zip:
	git archive --format zip --output ${USER}-lab04.zip HEAD

html:
	pandoc -o README.html -f gfm README.md

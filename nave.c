#include <ncurses.h>
#include <pthread.h>
#include <unistd.h>
#include "include/nave.h"
#include "include/config.h"

Nave miNave;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void *decrementar(void *arg)
{
    while (1)
    {
        sleep(DEFAULT_INTERVALO_OXIGENO);
        // usleep(500000);

        pthread_mutex_lock(&mutex);

        if (miNave.oxigeno > 0)
            miNave.oxigeno--;

        pthread_mutex_unlock(&mutex);
    }

    return NULL;
}

int main()
{
    pthread_t hilo;
    miNave.oxigeno = NAVE_OXIGENO_INICIAL;

    pthread_create(&hilo, NULL, decrementar, NULL);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    int startx, starty, width, height;
    int screen_height, screen_width;

    getmaxyx(stdscr, screen_height, screen_width);

    height = screen_height / 2;
    width = screen_width / 2;

    starty = (screen_height - height) / 2;
    startx = (screen_width - width) / 2;

    WINDOW *ventana = newwin(height, width, starty, startx);
    box(ventana, 0, 0);
    wrefresh(ventana);

    while (1)
    {
        pthread_mutex_lock(&mutex);
        int valor = miNave.oxigeno;
        pthread_mutex_unlock(&mutex);

        wclear(ventana);

        mvwprintw(ventana, 1, 2, "Oxigeno: %d", valor);
        mvwprintw(ventana, 2, 2, "ENTER = reiniciar a 100");
        mvwprintw(ventana, 3, 2, "q = salir");
        box(ventana, 0, 0);
        wrefresh(ventana);

        wtimeout(ventana, 100); // getch espera 100 ms

        int tecla = wgetch(ventana);

        if (tecla == '\n' || tecla == KEY_ENTER)
        {
            pthread_mutex_lock(&mutex);
            miNave.oxigeno = NAVE_OXIGENO_INICIAL;
            pthread_mutex_unlock(&mutex);
        }
        else if (tecla == 'q')
        {
            break;
        }
    }

    delwin(ventana);
    endwin();

    return 0;
}
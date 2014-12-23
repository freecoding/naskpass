#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <ncurses.h>
#include <sys/time.h>
#include <sys/types.h>
#include <signal.h>

#include "ui.h"
#include "ui_ani.h"
#include "ui_input.h"
#include "ui_statusbar.h"

#include "status.h"

#define APP_TIMEOUT 60
#define APP_TIMEOUT_FMT "%02d"
#define PASSWD_WIDTH 35
#define PASSWD_HEIGHT 5
#define PASSWD_XRELPOS (unsigned int)(PASSWD_WIDTH / 2) - (PASSWD_WIDTH / 6)
#define PASSWD_YRELPOS (unsigned int)(PASSWD_HEIGHT / 2) + 1

#define STRLEN(s) (sizeof(s)/sizeof(s[0]))


char *passwd = NULL;

static unsigned int max_x, max_y;
static WINDOW *wnd_main;
static struct nask_ui *nui = NULL;
static pthread_t thrd;
static bool active;
static unsigned int atmout = APP_TIMEOUT;
static pthread_cond_t cnd_update = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t mtx_update = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mtx_busy = PTHREAD_MUTEX_INITIALIZER;
static sem_t sem_rdy;


void
register_ui_elt(ui_callback uicb, void *data, WINDOW *wnd)
{
  struct nask_ui *tmp, *new;

  if (nui != NULL) {
    tmp = nui;
    while (tmp->next != NULL) {
     tmp = tmp->next;
    }
  }
  new = calloc(1, sizeof(struct nask_ui));
  new->ui_elt_cb = uicb;
  new->wnd = wnd;
  new->data = data;
  new->next = NULL;
  if (nui == NULL) {
    nui = new;
    nui->next = NULL;
  } else {
    tmp->next = new;
  }
}

void
unregister_ui_elt(void *data)
{
  struct nask_ui *cur = nui, *next, *before = NULL;

  while (cur != NULL) {
    next = cur->next;
    if (cur->data != NULL && cur->data == data) {
      free(cur);
      if (before != NULL) {
        before->next = next;
      } else {
        nui = next;
      }
    }
    before = cur;
    cur = next;
  }
}

static int
do_ui_update(bool timed_out)
{
  int retval = UICB_OK;
  int curx = getcurx(wnd_main);
  int cury = getcury(wnd_main);
  struct nask_ui *cur = nui;

  /* call all draw callback's */
  erase();
  while (cur != NULL) {
    if (cur->ui_elt_cb != NULL) {
      cur->ui_elt_cb(cur->wnd, cur->data, timed_out);
      doupdate();
    } else {
      retval = UICB_ERR_CB;
    }
    cur = cur->next;
  }
  /* TODO: Maybe export to an extra module? */
  attron(COLOR_PAIR(1));
  mvprintw(0, max_x - STRLEN(APP_TIMEOUT_FMT), "[" APP_TIMEOUT_FMT "]", atmout);
  attroff(COLOR_PAIR(1));
  /* EoT (End of Todo) */
  wmove(wnd_main, cury, curx);
  wrefresh(wnd_main);
  return (retval);
}

static void *
ui_thrd(void *arg)
{
  int cnd_ret;
  struct timeval now;
  struct timespec wait;

  pthread_mutex_lock(&mtx_update);
  gettimeofday(&now, NULL);
  wait.tv_sec = now.tv_sec + UILOOP_TIMEOUT;
  wait.tv_nsec = now.tv_usec * 1000;
  do_ui_update(true);
  sem_post(&sem_rdy);
  while (active == true) {
    pthread_mutex_unlock(&mtx_busy);
    cnd_ret = pthread_cond_timedwait(&cnd_update, &mtx_update, &wait);
    if (cnd_ret == ETIMEDOUT) {
      wait.tv_sec += UILOOP_TIMEOUT;
    }
    pthread_mutex_lock(&mtx_busy);
    if (--atmout == 0) active = false;
    if (active == false) {
      break;
    }
    do_ui_update( (cnd_ret == ETIMEDOUT ? true : false) );
  }
  pthread_mutex_unlock(&mtx_busy);
  pthread_mutex_unlock(&mtx_update);
  return (NULL);
}

void
ui_thrd_force_update(void)
{
  pthread_cond_signal(&cnd_update);
}

WINDOW *
init_ui(void)
{
  wnd_main = initscr();
  max_x = getmaxx(wnd_main);
  max_y = getmaxy(wnd_main);
  start_color();
  init_pair(1, COLOR_RED, COLOR_WHITE);
  init_pair(2, COLOR_WHITE, COLOR_BLACK);
  init_pair(3, COLOR_BLACK, COLOR_WHITE);
  raw();
  keypad(stdscr, TRUE);
  noecho();
  cbreak();
  return (wnd_main);
}

void
free_ui(void)
{
  delwin(wnd_main);
  endwin();
  clear();
  printf(" \033[2J");
}

int
run_ui_thrd(void) {
  pthread_mutex_lock(&mtx_busy);
  active = true;
  pthread_cond_signal(&cnd_update);
  pthread_mutex_unlock(&mtx_busy);
  return (pthread_create(&thrd, NULL, &ui_thrd, NULL));
}

int
stop_ui_thrd(void) {
  pthread_mutex_lock(&mtx_busy);
  active = false;
  pthread_mutex_unlock(&mtx_busy);
  return (pthread_join(thrd, NULL));
}

static bool
process_key(wchar_t key, struct input *a, WINDOW *win)
{
  bool retval = true;

  atmout = APP_TIMEOUT;
  switch (key) {
    case UIKEY_ENTER:
      passwd = malloc((a->input_len + 1)*sizeof(char));
      strncpy(passwd, a->input, a->input_len);
      passwd[a->input_len] = '\0';
      retval = false;
      break;
    case UIKEY_BACKSPACE:
      del_input(win, a);
      break;
    case UIKEY_ESC:
      retval = false;
      ui_thrd_force_update();
      break;
    case UIKEY_DOWN:
    case UIKEY_UP:
    case UIKEY_LEFT:
    case UIKEY_RIGHT:
      break;
    default:
      add_input(win, a, key);
  }
  return (retval);
}

static int
lower_statusbar_update(WINDOW *win, struct statusbar *bar)
{
  char *tmp = get_system_stat();
  set_statusbar_text(bar, tmp);
  free(tmp);
  return (0);
}

int
do_ui(void)
{
  struct input *pw_input;
  struct anic *heartbeat;
  struct statusbar *higher, *lower;
  char key = '\0';

  if (sem_init(&sem_rdy, 0, 0) == -1) {
    perror("init semaphore");
    return (DOUI_ERR);
  }
  init_ui();
  pw_input = init_input((unsigned int)(max_x / 2)-PASSWD_XRELPOS, (unsigned int)(max_y / 2)-PASSWD_YRELPOS, PASSWD_WIDTH, "PASSWORD: ", 128, COLOR_PAIR(3), COLOR_PAIR(2));
  heartbeat = init_anic(0, 0, A_BOLD | COLOR_PAIR(1), "[%c]");
  higher = init_statusbar(0, max_x, A_BOLD | COLOR_PAIR(3), NULL);
  lower = init_statusbar(max_y - 1, max_x, COLOR_PAIR(3), lower_statusbar_update);
  register_input(NULL, pw_input);
  register_statusbar(higher);
  register_statusbar(lower);
  register_anic(heartbeat);
  activate_input(wnd_main, pw_input);
  set_statusbar_text(higher, "/* NASKPASS */");
  if (run_ui_thrd() != 0) {
    return (DOUI_ERR);
  }
  sem_wait(&sem_rdy);
  wtimeout(wnd_main, 1000);
  while (active == true) {
    if ((key = wgetch(wnd_main)) == '\0') {
      break;
    }
    if (key == -1) {
      continue;
    }
    pthread_mutex_lock(&mtx_busy);
    active = process_key(key, pw_input, wnd_main);
    activate_input(wnd_main, pw_input);
    do_ui_update(false);
    pthread_mutex_unlock(&mtx_busy);
  }
  stop_ui_thrd();
  unregister_ui_elt(lower);
  unregister_ui_elt(higher);
  unregister_ui_elt(heartbeat);
  unregister_ui_elt(pw_input);
  free_input(pw_input);
  free_anic(heartbeat);
  free_statusbar(higher);
  free_statusbar(lower);
  free_ui();
  return (DOUI_OK);
}

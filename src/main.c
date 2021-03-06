#include "config.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

#include "opt.h"
#include "log.h"

#include "ui.h"
#include "ui_ipc.h"
#include "ui_ani.h"
#include "ui_input.h"
#include "ui_statusbar.h"
#include "ui_nask.h"

#define MSG(msg_idx) msg_arr[msg_idx]


enum msg_index {
  MSG_BUSY_FD = 0,
  MSG_BUSY,
  MSG_NO_FIFO,
  MSG_FIFO_ERR,
  MSG_FIFO_BUSY,
  MSG_CRYPTCMD_ERR,
  MSG_NUM
};
static const char *msg_arr[] = { "Please wait, got a piped password",
                                 "Please wait, busy",
                                 "check_fifo: %s is not a FIFO\n",
                                 "check_fifo: %s error(%d): %s\n",
                                 "fifo: cryptcreate busy",
                                 "cryptcreate error"
                               };


static bool
check_fifo(char *fifo_path)
{
  struct stat st;

  if (mkfifo(fifo_path, S_IRUSR | S_IWUSR) == 0) {
    return (true);
  } else {
    if (errno == EEXIST) {
      if (stat(fifo_path, &st) == 0) {
        if (S_ISFIFO(st.st_mode) == 1) {
          return (true);
        } else {
          fprintf(stderr, MSG(MSG_NO_FIFO), fifo_path);
          return (false);
        }
      }
    }
  }
  fprintf(stderr, MSG(MSG_FIFO_ERR), fifo_path, errno, strerror(errno));
  return (false);
}

int
run_cryptcreate(char *pass, char *crypt_cmd)
{
  int retval;
  char *cmd;

  if (crypt_cmd == NULL || pass == NULL) return -1;
#ifdef DEBUG
  if (asprintf(&cmd, "echo '%s' | %s 1>&2", pass, crypt_cmd) < 0)
#else
  if (asprintf(&cmd, "echo '%s' | %s >/dev/null 2>/dev/null", pass, crypt_cmd) < 0)
#endif
    return -2;
  retval = system(cmd);
  free(cmd);
  return (retval);
}

void sigfunc(int signal)
{
  switch (signal) {
    case SIGSEGV:
    case SIGTERM:
      ui_ipc_semtrywait(SEM_UI);
      break;
  }
}

int
main(int argc, char **argv)
{
  int ret = EXIT_FAILURE, ffd = -1, c_status;
  pid_t child;
  char pbuf[IPC_MQSIZ+1];
  bool csetup_ok = false;

  signal(SIGINT, SIG_IGN);
  signal(SIGTERM, sigfunc);

  if ( parse_cmd(argc, argv) != 0 )
    exit(EXIT_FAILURE);
  if (OPT(CRYPT_CMD).found == 0) {
    fprintf(stderr, "%s: crypt cmd is mandatory\n", argv[0]);
    goto error;
  }
  if (check_fifo(GETOPT(FIFO_PATH).str) == false)
    goto error;
  if ((ffd = open(GETOPT(FIFO_PATH).str, O_NONBLOCK | O_RDWR)) < 0) {
    fprintf(stderr, "%s: fifo '%s' error: %d (%s)\n", argv[0], GETOPT(FIFO_PATH).str, errno, strerror(errno));
    goto error;
  }

  if (ui_ipc_init(1) != 0) {
    fprintf(stderr, "%s: can not create semaphore/message queue: %d (%s)\n", argv[0], errno, strerror(errno));
    goto error;
  }

  memset(pbuf, '\0', IPC_MQSIZ+1);
  log_init( GETOPT(LOG_FILE).str );
  logs("%s\n", "log init");
  logs_dbg("%s\n", "debug mode active");

  ui_ipc_sempost(SEM_UI);
  ui_ipc_sempost(SEM_IN);
  if ((child = fork()) == 0) {
    /* child */
    logs("%s\n", "child");
    if (ffd >= 0) close(ffd);
#ifndef DEBUG
    fclose(stderr);
#endif
    /* Slave process: TUI */
    if (ui_ipc_init(0) == 0) {
      do_ui();
    }
    ui_ipc_free(0);
    exit(0);
  } else if (child > 0) {
    /* parent */
    logs("%s\n", "parent");
    fclose(stdin);
    fclose(stdout);
    /* Master process: mainloop (read passwd from message queue or fifo and exec cryptcreate */
    while ( ui_ipc_getvalue(SEM_UI) > 0 ) {
      if (read(ffd, pbuf, IPC_MQSIZ) >= 0) {
        ui_ipc_semwait(SEM_IN);
        logs_dbg("%s\n", "fifo password");
        if (run_cryptcreate(pbuf, GETOPT(CRYPT_CMD).str) != 0) {
          logs_dbg("%s\n", "cryptcreate error");
          sleep(3);
        } else {
          csetup_ok = true;
          logs_dbg("%s\n", "cryptcreate success");
          ui_ipc_semtrywait(SEM_UI);
        }
        ui_ipc_sempost(SEM_IN);
      } else if ( ui_ipc_msgcount(MQ_PW) > 0 ) {
        //ui_ipc_semwait(SEM_IN);
        ui_ipc_msgrecv(MQ_PW, pbuf, 0);
        logs_dbg("%s\n", "password");
        ui_ipc_msgsend(MQ_IF, MSG(MSG_BUSY));
        if (run_cryptcreate(pbuf, GETOPT(CRYPT_CMD).str) != 0) {
          logs_dbg("%s\n", "cryptcreate error");
          ui_ipc_msgsend(MQ_IF, MSG(MSG_CRYPTCMD_ERR));
        } else {
          csetup_ok = true;
          logs_dbg("%s\n", "cryptcreate success");
          ui_ipc_semtrywait(SEM_UI);
        }
        //ui_ipc_sempost(SEM_IN);
      }
      usleep(100000);
    }
    logs("%s\n", "waiting for child");
    wait(&c_status);
    memset(pbuf, '\0', IPC_MQSIZ+1);
  } else {
    /* fork error */
    perror("fork");
    goto error;
  }

  if (csetup_ok)
    ret = EXIT_SUCCESS;
  else
    ret = EXIT_FAILURE;
  ui_ipc_free(1);
error:
  logs("%s\n", "exiting ..");
  if (ffd >= 0) close(ffd);
  log_free();
  exit(ret);
}

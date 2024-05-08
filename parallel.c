#include "config.h"
#include "defs.h"
#include "interrupt.h"
#include "parallel.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct worker_args
{
  /// worker index, e.g. parallel port 0, 1, or 2
  int idx;

  /// the local unix sock connection
  int fd;
};

static int num_parallel_workers;

static void*
parallel_listener(void* args);

/// args : int* -> fd
static void*
parallel_port_worker(void* args);

static void*
parallel_listener(void* args)
{
  (void)args;

  bool avail_ports[3] = { true, true, true };
  int listen_sock;
  struct sockaddr_un addr;

  if (vm_config.parallel_loc == NULL)
    return NULL;
  if (strlen(vm_config.parallel_loc) > sizeof(addr.sun_path))
    ERR("provided parallel socket path is too long (>%lu)\n",
        sizeof(addr.sun_path));

  listen_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_sock < 0)
    ERR("failed to create parallel socket at %s\n", vm_config.parallel_loc)

  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, vm_config.parallel_loc);

  if (bind(listen_sock, (const struct sockaddr*)&addr, sizeof(addr)) < 0)
    ERR("failed to bind parallel socket at %s\n", vm_config.parallel_loc)

  if (listen(listen_sock, 1) < 0)
    ERR("failed to listen to parallel socket at %s\n", vm_config.parallel_loc);

  for (;;) {
    int new_sock = accept(listen_sock, NULL, NULL);

    if (new_sock < 0) {
      perror("failed to accept incoming socket in parallel port handler");
      continue;
    }

    if (num_parallel_workers == 3) {
      printf("found incoming connection to parallel port, but all 3 ports are "
             "already in use!\n");
      close(new_sock);
      continue;
    }

    struct worker_args* args = malloc(sizeof(struct worker_args));
    args->fd = new_sock;

    for (int i = 0; i < 3; i++) {
      if (avail_ports[i] == true) {
        avail_ports[i] = false;
        args->idx = i;
        break;
      }
    }

    pthread_t thread;
    pthread_create(&thread, NULL, parallel_port_worker, args);
    pthread_detach(thread);

    num_parallel_workers += 1;
  }

  return NULL;
}

static void*
parallel_port_worker(void* _args)
{
  struct worker_args args = *(struct worker_args*)_args;
  free(_args);

  char buf;
  int fd, pipein, err;
  int pipe_tmp[2];
  enum interrupt_type ty;

  fd = args.fd;

  if (pipe(pipe_tmp) < 0) {
    perror("failed to create parallel port pipe forwarding channel");
    return NULL;
  }

  switch (args.idx) {
    case 0:
      ty = INT_P0;
      p0_fd = pipe_tmp[0];
      break;
    case 1:
      ty = INT_P1;
      p1_fd = pipe_tmp[0];
      break;
    case 2:
      ty = INT_P2;
      p2_fd = pipe_tmp[0];
      break;

    default:
      exit(1);
  }

  for (;;) {
    err = read(fd, &buf, 1);

    if (err == 0) {
      printf("disconnecting serial port\n");
      break;
    }

    if (err < 0) {
      perror("parallel port error");
      break;
    }

    pthread_mutex_lock(&interrupt_cond_mutex);
    pthread_cond_wait(&interrupt_cond, &interrupt_cond_mutex);
    current_interrupt = ty;
    pthread_mutex_unlock(&interrupt_cond_mutex);
  }

  close(fd);
  close(pipe_tmp[0]);
  close(pipe_tmp[1]);
  num_parallel_workers -= 1;

  return NULL;
}

extern void
parallel_init(void)
{
  num_parallel_workers = 0;

  p0_fd = -1;
  p1_fd = -1;
  p2_fd = -1;
}

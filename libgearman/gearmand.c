/* Gearman server and library
 * Copyright (C) 2008 Brian Aker, Eric Day
 * All rights reserved.
 *
 * Use and distribution licensed under the BSD license.  See
 * the COPYING file in the parent directory for full text.
 */

/**
 * @file
 * @brief Basic Server definitions
 */

#include "common.h"

/*
 * Private declarations
 */

/**
 * @addtogroup gearmand_private Private Basic Server Functions
 * @ingroup gearmand
 * @{
 */

#ifdef HAVE_EVENT_H
static gearman_return_t _listen_init(in_port_t port, int backlog,
                                     struct event_base *base,
                                     struct event *event, void *arg);

static void _listen_accept(int fd, short events __attribute__ ((unused)),
                           void *arg);

static gearman_return_t _con_watch(gearman_con_st *con, short events,
                                   void *arg __attribute__ ((unused)));

static void _con_ready(int fd __attribute__ ((unused)), short events,
                       void *arg);

static gearman_return_t _con_close(gearman_con_st *con, gearman_return_t ret,
                                   void *arg __attribute__ ((unused)));
#endif

/** @} */

/*
 * Public definitions
 */

gearmand_st *gearmand_init(in_port_t port, int backlog)
{
#ifdef HAVE_EVENT_H
  gearmand_st *gearmand;

  gearmand= malloc(sizeof(gearmand_st));
  if (gearmand == NULL)
    return NULL;

  memset(gearmand, 0, sizeof(gearmand_st));
  
  gearmand->base= event_init();
  if (gearmand->base == NULL)
  {
    free(gearmand);
    printf("event_init:NULL\n");
    return NULL;
  }

  if (gearman_server_create(&(gearmand->server)) == NULL)
  {
    free(gearmand);
    printf("gearman_server_create:NULL\n");
    return NULL;
  }

  gearman_server_set_event_cb(&(gearmand->server), _con_watch, _con_close,
                              NULL);

  if (_listen_init(port, backlog, gearmand->base, &(gearmand->listen_event),
                   gearmand) != GEARMAN_SUCCESS)
  {
    gearmand_destroy(gearmand);
    return NULL;
  }

  return gearmand;
#else
  (void) port;
  (void) backlog;
  printf("Library not built with libevent support!\n");
  return NULL;
#endif
}

void gearmand_destroy(gearmand_st *gearmand)
{
#ifdef HAVE_EVENT_H
  gearman_server_free(&(gearmand->server));
  event_base_free(gearmand->base);
  free(gearmand);
#else
  (void) gearmand;
#endif
}
 
void gearmand_run(gearmand_st *gearmand)
{
#ifdef HAVE_EVENT_H
  if (event_base_loop(gearmand->base, 0) == -1)
    printf("event_base_loop:-1\n");
#else
  (void) gearmand;
#endif
}

/*
 * Private definitions
 */

#ifdef HAVE_EVENT_H
static gearman_return_t _listen_init(in_port_t port, int backlog,
                                     struct event_base *base,
                                     struct event *event, void *arg)
{
  struct sockaddr_in sa;
  int fd;
  int opt= 1;

  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
  {
    printf("signal:%d\n", errno);
    return GEARMAN_ERRNO;
  }

  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port= htons(port);
  sa.sin_addr.s_addr = INADDR_ANY;

  fd= socket(sa.sin_family, SOCK_STREAM, 0);
  if (fd == -1)
  {
    printf("socket:%d\n", errno);
    return GEARMAN_ERRNO;
  }

  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
  {
    printf("setsockopt:%d\n", errno);
    return GEARMAN_ERRNO;
  }

  if (bind(fd, (struct sockaddr *)(&sa), sizeof(sa)) == -1)
  {
    printf("bind:%d\n", errno);
    return GEARMAN_ERRNO;
  }

  if (listen(fd, backlog) == -1)
  {
    printf("listen:%d\n", errno);
    return GEARMAN_ERRNO;
  }

  event_set(event, fd, EV_READ | EV_PERSIST, _listen_accept, arg);
  event_base_set(base, event);

  if (event_add(event, NULL) == -1)
  {
    printf("event_add\n");
    return GEARMAN_ERRNO;
  }

  return GEARMAN_SUCCESS;
}

static void _listen_accept(int fd, short events __attribute__ ((unused)),
                           void *arg)
{
  gearmand_st *gearmand= (gearmand_st *)arg;
  gearmand_con_st *dcon;
  socklen_t sa_len;

  dcon= malloc(sizeof(gearmand_con_st));
  if (dcon == NULL)
  {
    printf("malloc:%d\n", errno);
    exit(1);
  }

  sa_len= sizeof(dcon->sa);
  dcon->fd= accept(fd, (struct sockaddr *)(&(dcon->sa)), &sa_len);
  if (dcon->fd == -1)
  {
    free(dcon);
    printf("accept:%d\n", errno);
    exit(1);
  }

  printf("Connect: %s:%u\n", inet_ntoa(dcon->sa.sin_addr),
         ntohs(dcon->sa.sin_port));

  dcon->gearmand= gearmand;

  if (gearman_server_add_con(&(gearmand->server), &(dcon->server_con), dcon->fd,
                             dcon) == NULL)
  {
    close(dcon->fd);
    free(dcon);
    printf("gearman_server_add_con:%s\n",
           gearman_server_error(&(gearmand->server)));
    exit(1);
  }
}

static gearman_return_t _con_watch(gearman_con_st *con, short events,
                                   void *arg __attribute__ ((unused)))
{
  gearmand_con_st *dcon;
  short set_events= 0;

  dcon= (gearmand_con_st *)gearman_con_data(con);
  dcon->con= con;

  if (events & POLLIN)
    set_events|= EV_READ;
  if (events & POLLOUT)
    set_events|= EV_WRITE;

  event_set(&(dcon->event), dcon->fd, set_events, _con_ready, dcon);
  event_base_set(dcon->gearmand->base, &(dcon->event));

  if (event_add(&(dcon->event), NULL) == -1)
    return GEARMAN_EVENT;

  return GEARMAN_SUCCESS;
}

static void _con_ready(int fd __attribute__ ((unused)), short events,
                       void *arg)
{
  gearmand_con_st *dcon= (gearmand_con_st *)arg;
  gearman_server_con_st *server_con;
  short revents= 0;
  gearman_return_t ret;

  if (events & EV_READ)
    revents|= POLLIN;
  if (events & EV_WRITE)
    revents|= POLLOUT;

  gearman_con_set_revents(dcon->con, revents);

  server_con= gearman_server_run(&(dcon->gearmand->server), &ret);
  if (ret != GEARMAN_SUCCESS && ret != GEARMAN_IO_WAIT)
  {
    printf("gearman_server_run:%s\n",
           gearman_server_error(&(dcon->gearmand->server)));
    if (server_con == NULL)
      exit(1);

    dcon= (gearmand_con_st *)gearman_server_con_data(server_con);

    printf("Disconnect: %s:%u\n", inet_ntoa(dcon->sa.sin_addr),
           ntohs(dcon->sa.sin_port));

    gearman_server_con_free(server_con);
    free(dcon);
  }
}

static gearman_return_t _con_close(gearman_con_st *con, gearman_return_t ret,
                                   void *arg __attribute__ ((unused)))
{
  gearmand_con_st *dcon= (gearmand_con_st *)gearman_con_data(con);

  if (ret != GEARMAN_SUCCESS)
    printf("_con_close:%s\n", gearman_server_error(&(dcon->gearmand->server)));

  if (event_del(&(dcon->event)) == -1)
    return GEARMAN_EVENT;

  gearman_con_free(con);
  free(dcon);

  return GEARMAN_SUCCESS;
}
#endif
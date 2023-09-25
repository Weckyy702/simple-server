#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PORT 8000
#define REQUEST_SIZE 1024
#define MAX_FILES 10

#define fail(msg)                                                              \
  do {                                                                         \
    perror(msg);                                                               \
    exit(EXIT_FAILURE);                                                        \
  } while (0)

#define log_error(msg)                                                         \
  do {                                                                         \
    fputs(msg, stderr);                                                        \
  } while (0)

#define respond_error(code)                                                    \
  do {                                                                         \
    dprintf(client_socket, "HTTP/1.1 %.3d error\r\n\r\n Error %.3d", code,     \
            code);                                                             \
    return;                                                                    \
  } while (0)

typedef struct {
  const char *content;
  size_t len;
} StringView;

typedef struct {
  StringView name;
  StringView path;
  int fd;
} File;

static File open_files[MAX_FILES] = {0};

void handle_request(int client_socket) {
  char buffer[REQUEST_SIZE] = {0};
  int len = 0;
  if ((len = read(client_socket, buffer, REQUEST_SIZE)) < 0) {
    perror("read from client");
    return;
  }

  // Include terminal null byte
  if (len < 6 || strncmp(buffer, "GET /", 5)) {
    log_error("Invalid HTTP request line!\n");
    respond_error(400);
  }

  char *filename = buffer + 5;
  char *p = filename;
  for (; *p && *p != ' '; ++p)
    ;
  if (!*p) {
    log_error("Unexpected end of string before end of filename!\n");
    respond_error(400);
  }
  size_t filename_len = p - filename;

  // Empty GET request should be forwarded to index.html
  if (!filename_len) {
    filename = "index.html";
    filename_len = 10;
  }

  // Just linear scan through the files, might get slow with 1000s of files tho
  File *to_serve = NULL;
  for (File *f = open_files; f->name.content; ++f) {
    if (f->name.len != filename_len)
      continue;
    if (strncmp(f->name.content, filename, f->name.len))
      continue;
    to_serve = f;
    break;
  }

  if (!to_serve) {
    fprintf(stderr, "I cannot serve requested file '%.*s'\n", (int)filename_len,
            filename);
    respond_error(404);
  }

  // Note: after we send the 200 OK, if sendfile fails, we have no way to
  // let the client know
  dprintf(client_socket, "HTTP/1.1 200 OK\r\n"
                         "Cache-Control: max-age: 600\r\n"
                         "\r\n");
  off_t offset = 0;
  ssize_t nbytes;
  // TODO: sometimes fails with EAGAIN, indicating that we need to handle
  // reading and writing separately
  while ((nbytes = sendfile(client_socket, to_serve->fd, &offset, 4096)) > 0)
    ;
  if (nbytes < 0) {
    perror("sendfile");
  }
}

void populate_open_files() {
  // Might get called in a signal so write it is
  write(STDOUT_FILENO, "Repopulating open file descriptors...\n", 38);
  for (size_t i = 0; i != MAX_FILES; ++i) {
    File *f = open_files + i;
    if (!f->name.content) // empty file
      break;
    if (f->fd) {
      close(f->fd);
      f->fd = 0;
    }
    // Open file
    int fd = open(f->path.content, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      char *err_str = strerror(errno);
      printf("Could not open file '%s': '%s'\n", f->path.content, err_str);
      exit(EXIT_FAILURE);
    }
    f->fd = fd;
  }
}

void init_open_files(int argc, char *argv[]) {
  if (argc - 1 > MAX_FILES) {
    fprintf(
        stderr,
        "This server cannot serve that many files. MAX_FILES=%d, requested %d.",
        MAX_FILES, argc - 1);
    exit(EXIT_FAILURE);
  }

  for (int i = 1; i < argc; ++i) {
    char *path = argv[i];
    size_t path_len = strlen(path);
    char *name = strstr(path, "static/");

    size_t name_len = name ? path_len - 7 : path_len;
    name = name ? name + 7 : path;

    //  Register file
    File *f = open_files + i - 1;
    f->name.content = name;
    f->name.len = name_len;
    f->path.content = path;
    f->path.len = path_len;
    printf("Registered file %s with basename %s\n", path, name);
  }
  populate_open_files();
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Usage: %s <files>\n", argv[0]);
    return EXIT_FAILURE;
  }

  struct sigaction sa = {0};
  sa.sa_handler = populate_open_files;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;

  if (sigaction(SIGUSR1, &sa, NULL) < 0) {
    fail("sigaction");
  }

  init_open_files(argc, argv);

  int server_socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (server_socket < 0) {
    fail("socket");
  }

  int option = 1;
  if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                 &option, sizeof(option)) < 0) {
    fail("setsockopt");
  }

  if (setsockopt(server_socket, IPPROTO_TCP, TCP_CORK, &optind,
                 sizeof(option)) < 0) {
    fail("setsockopt TCP");
  }

  puts("Socket created");

  struct sockaddr_in server_addr = {0};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(PORT);
  server_addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(server_socket, (struct sockaddr *)&server_addr,
           sizeof(server_addr)) < 0) {
    fail("bind");
  }

  puts("Socket bound");

  if (listen(server_socket, 5) < 0) {
    fail("listen");
  }

  printf("Server listening on port %d\n", PORT);

  fd_set fds = {0};
  FD_SET(server_socket, &fds);

  // Block SIGUSR1 while selecting()-ing
  // We use it to tell the server to reload all files
  sigset_t smask;
  sigemptyset(&smask);
  sigaddset(&smask, SIGUSR1);

  for (;;) {
    fd_set read_fds = fds;
    if (pselect(FD_SETSIZE, &read_fds, NULL, NULL, NULL, &smask) < 0) {
      fail("select");
    }

    if (FD_ISSET(server_socket, &read_fds)) {
      int client_socket = accept4(server_socket, NULL, NULL, SOCK_NONBLOCK);

      if (client_socket < 0) {
        perror("accept");
      } else {
        FD_SET(client_socket, &fds);
      }
    }

    for (int fd = 0; fd != FD_SETSIZE; ++fd) {
      if (fd == server_socket)
        continue;
      if (FD_ISSET(fd, &read_fds)) {
        handle_request(fd);
        close(fd);
        FD_CLR(fd, &fds);
      }
    }
  }

  close(server_socket);

  for (int i = 0; i != MAX_FILES; ++i) {
    File *f = open_files + i;
    if (!f->name.content)
      break;
    close(f->fd);
  }

  return EXIT_SUCCESS;
}

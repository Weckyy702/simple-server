#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define PORT 1234
#define REQUEST_SIZE 1024
#define MAX_FILES 5

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
    dprintf(client_socket, "HTTP/1.1 %.3d error", code);                       \
    return;                                                                    \
  } while (0)

typedef struct {
  const char *name;
  size_t name_len;
  int fd;
} File;

static File open_files[MAX_FILES] = {0};

// TODO: concurrent requests

void handle_request(int client_socket) {
  char buffer[REQUEST_SIZE] = {0};
  int len = 0;
  if ((len = read(client_socket, buffer, REQUEST_SIZE)) < 0) {
    perror("read from client");
    return;
  }

  // Include terminal null byte
  if (len < 6 || strncmp(buffer, "GET /", 5)) {
    log_error("Invalid HTTP request line!");
    respond_error(400);
  }

  char *filename = buffer + 5;
  char *p = filename;
  for (; *p && *p != ' '; ++p)
    ;
  if (!*p) {
    log_error("Unexpected end of string before end of filename!");
    respond_error(400);
  }

  size_t filename_len = p - filename;
  File *to_serve = NULL;
  for (File *f = open_files; f->name; ++f) {
    if (f->name_len != filename_len)
      continue;
    if (strncmp(f->name, filename, f->name_len))
      continue;
    to_serve = f;
    break;
  }

  if (!to_serve) {
    fprintf(stderr, "I cannot serve requested file '%.*s'\n", (int)filename_len,
            filename);
    respond_error(404);
  }
  printf("Serving file %.*s\n", (int)to_serve->name_len, to_serve->name);

  // Note: after we send the 200 OK, if the splicing fails, we have no way to
  // let the client know
  dprintf(client_socket, "HTTP/1.1 200 OK\r\n\r\n");
  ssize_t nbytes;
  while ((nbytes = sendfile(client_socket, to_serve->fd, NULL, 4096)) > 0)
    ;
  if (nbytes < 0) {
    perror("sendfile");
    return;
  }
  lseek(to_serve->fd, 0, SEEK_SET);
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
    // Open file
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
      char *err_str = strerror(errno);
      printf("Could not open file '%s': '%s'\n", path, err_str);
      exit(EXIT_FAILURE);
    }

    char *name = strstr(path, "static/");
    name = name ? name + 7 : path;

    //  Register file
    File *f = open_files + i - 1;
    f->name = name;
    f->name_len = strlen(name);
    f->fd = fd;
    printf("Registered file %s with basename %s\n", path, name);
  }
}

void refresh_open_files(int argc, char *argv[]) {}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Usage: %s <files>\n", argv[0]);
    return EXIT_FAILURE;
  }

  init_open_files(argc, argv);

  int server_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket < 0) {
    fail("socket");
  }

  int option = 1;
  if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                 &option, sizeof(option)) < 0) {
    fail("setsockopt");
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

  for (;;) {
    struct sockaddr_in client_addr;
    socklen_t client_size = sizeof(client_addr);

    int client_socket =
        accept(server_socket, (struct sockaddr *)&client_addr, &client_size);

    if (client_socket < 0) {
      fail("accept");
    }

    handle_request(client_socket);
    close(client_socket);
  }

  close(server_socket);

  for (int i = 0; i != MAX_FILES; ++i) {
    File *f = open_files + i;
    if (!f->name)
      break;
    close(f->fd);
  }

  return EXIT_SUCCESS;
}

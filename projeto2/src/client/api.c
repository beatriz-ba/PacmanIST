#include "api.h"
#include "debug.h"
#include "protocol.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct Session {
    int id;
    int req_pipe;
    int notif_pipe;
    char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1};

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path,
                   char const *server_pipe_path) {
    unlink(req_pipe_path);
    unlink(notif_pipe_path);

    // Criação das pipes de requisições e notificações
    if (mkfifo(req_pipe_path, 0644) == -1) {
        debug("Failed to create request pipe \n");
        return 1;
    }
    if (mkfifo(notif_pipe_path, 0644) == -1) {
        debug("Failed to create notification pipe \n");
        unlink(req_pipe_path);
        return 1;
    }

    // Abertura do pipe server
    int fd_server = open(server_pipe_path, O_WRONLY);
    if (fd_server == -1)
        return 1;

    // Criação da mensagem para enviar ao servidor
    connect_req req;
    memset(&req, 0, sizeof(req));
    req.opcode = OP_CODE_CONNECT;
    strncpy(req.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
    strncpy(req.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);

    // Envio da mensagem
    int status = write(fd_server, &req, sizeof(req));

    if (status == -1) {
        close(fd_server);
        return 1;
    }

    close(fd_server);

    // Abertura do pipe de notificações
    int fd_notif = open(notif_pipe_path, O_RDONLY);
    if (fd_notif == -1) {
        return 1;
    }
    session.notif_pipe = fd_notif;

    // Leitura da resposta do servidor
    char buffer_resp[2];
    int n = read(fd_notif, buffer_resp, 2);

    // Abertura do pipe de requests
    int fd_req = open(req_pipe_path, O_WRONLY);
    if (fd_req == -1) {
        close(fd_notif);
        return 1;
    }

    session.req_pipe = fd_req;

    strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
    strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
    session.req_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';
    session.notif_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';

    // NOTE: move me up?
    if (n == 2) {
        if (buffer_resp[0] == OP_CODE_CONNECT && buffer_resp[1] == '0') {
            debug("Connection sucesseful \n");
            return 0;
        } else {
            close(fd_notif);
            close(fd_req);
            return 1;
        }
    } else {
        close(fd_notif);
        close(fd_req);
        return 1;
    }
}

void pacman_play(char command) {
    char buffer[2] = {OP_CODE_PLAY, command};

    int status = write(session.req_pipe, buffer, 2);

    if (status == -1) {
        pacman_disconnect();
        return;
    }
    return;
}

int pacman_disconnect() {
    char buffer[2];
    buffer[0] = OP_CODE_DISCONNECT;

    // Escreve request para o servidor
    int n = write(session.req_pipe, buffer, 2);
    if (n == -1)
        return -1;

    // Fecha os pipes de request e notificações
    close(session.req_pipe);
    close(session.notif_pipe);

    // Apaga os pipes
    unlink(session.req_pipe_path);
    unlink(session.notif_pipe_path);

    return 0;
}

Board *receive_board_update(void) {
    char opcode;
    Board *b = (Board *)malloc(sizeof(Board));

    if (read(session.notif_pipe, &opcode, sizeof(opcode)) < 0) {
        free(b);
        pacman_disconnect();
        return NULL;
    }

    if (opcode != OP_CODE_BOARD) {
        free(b);
        pacman_disconnect();
        return NULL;
    }
    debug("received a board\n");
    // Sabemos que o opcode é certo logo ler o resto da mensagem
    (void)!read(session.notif_pipe, &b->width, sizeof(b->width));
    (void)!read(session.notif_pipe, &b->height, sizeof(b->height));
    (void)!read(session.notif_pipe, &b->tempo, sizeof(b->tempo));
    (void)!read(session.notif_pipe, &b->victory, sizeof(b->victory));
    (void)!read(session.notif_pipe, &b->game_over, sizeof(b->game_over));
    (void)!read(session.notif_pipe, &b->accumulated_points,
                sizeof(b->accumulated_points));

    debug("received basic data %d, %d, %d, %d, %d, %d\n", b->height, b->width,
          b->tempo, b->victory, b->game_over, b->accumulated_points);

    int board_size = (b->height * b->width);
    b->data = malloc((board_size + 1) * sizeof(char));

    if (b->data == NULL) {
        free(b);
        pacman_disconnect();
        return NULL;
    }
    debug("reading board data\n");

    // Lê o conteudo do board

    if (read(session.notif_pipe, b->data, board_size) < 0) {
        free(b->data);
        free(b);
        pacman_disconnect();
        return NULL;
    }

    b->data[board_size] = '\0';
    return b;
}

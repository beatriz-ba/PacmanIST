#define MAX_PIPE_PATH_LENGTH 40
#include "board.h"
#include <dirent.h>
#include <string.h>

#ifndef PROTOCOL_H
#define PROTOCOL_H

enum {
    OP_CODE_CONNECT = 1,
    OP_CODE_DISCONNECT = 2,
    OP_CODE_PLAY = 3,
    OP_CODE_BOARD = 4,
};

typedef struct {
    char opcode;
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
} connect_req;

typedef struct {
    int width;
    int height;
    int tempo;
    int victory;
    int game_over;
    int accumulated_points;
    char *data;
} Board;

typedef struct {
    char client_id;
    char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
    int req_pipe;
    int notif_pipe;
    Board board_client;
    board_t board_server;
} info_client;

typedef struct {
    int max_games;
    char *levels_directory;
} info_board;

typedef struct {
    char opcode;
    int width;
    int height;
    int tempo;
    int victory;
    int game_over;
    int accumulated_points;
    char *board_data;
} notif_men;

#endif

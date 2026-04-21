#define MAX_PIPE_PATH_LENGTH 40

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

#endif

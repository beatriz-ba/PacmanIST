#include "parser.h"
#include "board.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int read_level(board_t *board, char *filename, char *dirname) {
    // Constrói o caminho completo do arquivo de nível
    char fullname[MAX_FILENAME];
    strcpy(fullname, dirname);
    strcat(fullname, "/");
    strcat(fullname, filename);

    // Abre o arquivo para leitura
    int fd = open(fullname, O_RDONLY);
    if (fd == -1) {
        debug("Error opening file %s\n", fullname);
        return -1;
    }

    char command[MAX_COMMAND_LENGTH];

    // Inicializa configurações padrão do Pacman (opcional no arquivo)
    board->pacman_file[0] = '\0';
    board->n_pacmans = 1;

    // Define o nome do nível baseado no nome do arquivo (remove extensão)
    strcpy(board->level_name, filename);
    *strrchr(board->level_name, '.') = '\0';

    int read;
    // Loop principal de leitura do cabeçalho do arquivo
    while ((read = read_line(fd, command)) > 0) {

        // Ignora comentários e linhas vazias
        if (command[0] == '#' || command[0] == '\0')
            continue;

        // Tokeniza a linha para identificar o comando
        char *word = strtok(command, " \t\n");
        if (!word)
            continue; // skip empty line

        // Processa as dimensões do tabuleiro (DIM width height)
        if (strcmp(word, "DIM") == 0) {
            char *arg1 = strtok(NULL, " \t\n");
            char *arg2 = strtok(NULL, " \t\n");
            if (arg1 && arg2) {
                board->width = atoi(arg1);
                board->height = atoi(arg2);
                debug("DIM = %d x %d\n", board->width, board->height);
            }
        }

        // Processa o tempo de jogo (TEMPO t)
        else if (strcmp(word, "TEMPO") == 0) {
            char *arg = strtok(NULL, " \t\n");
            if (arg) {
                board->tempo = atoi(arg);
                debug("TEMPO = %d\n", board->tempo);
            }
        }

        // Processa o arquivo de configuração do Pacman (PAC file)
        else if (strcmp(word, "PAC") == 0) {
            char *arg = strtok(NULL, " \t\n");
            if (arg) {
                snprintf(board->pacman_file, sizeof(board->pacman_file),
                         "%s/%s", dirname, arg);
                debug("PAC = %s\n", board->pacman_file);
            }
        }

        // Processa os arquivos de configuração dos Monstros (MON file1 file2
        // ...)
        else if (strcmp(word, "MON") == 0) {
            char *arg;
            int i = 0;
            while ((arg = strtok(NULL, " \t\n")) != NULL) {
                snprintf(board->ghosts_files[i], sizeof(board->ghosts_files[0]),
                         "%s/%s", dirname, arg);
                debug("MON file: %s\n", board->ghosts_files[i]);
                i += 1;
                if (i == MAX_GHOSTS - 1)
                    break;
            }
            board->n_ghosts = i;
        }

        // Se encontrar algo que não é comando de cabeçalho, assume que é o
        // início do mapa
        else {
            break;
        }
    }

    // Valida se as dimensões foram lidas corretamente
    if (!board->width || !board->height) {
        debug("Missing dimensions in level file\n");
        close(fd);
        return -1;
    }

    // Aloca memória para o tabuleiro e entidades
    board->board = calloc(board->width * board->height, sizeof(board_pos_t));
    board->pacmans = calloc(board->n_pacmans, sizeof(pacman_t));
    board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));

    int row = 0;
    // O loop anterior parou na primeira linha do mapa, então processamos ela e
    // as seguintes
    while (read > 0) {
        if (command[0] == '#' || command[0] == '\0')
            continue;
        if (row >= board->height)
            break;

        debug("Line: %s\n", command);

        // Processa cada coluna da linha atual
        for (int col = 0; col < board->width; col++) {
            int idx = row * board->width + col;
            char content = command[col];

            switch (content) {
            case 'X': // Parede
                board->board[idx].content = 'W';
                break;
            case '@': // Portal
                board->board[idx].content = ' ';
                board->board[idx].has_portal = 1;
                break;
            default: // Espaço vazio (com comida)
                board->board[idx].content = ' ';
                board->board[idx].has_dot = 1;
                break;
            }
        }

        row++;
        read = read_line(fd, command);
    }

    if (read == -1) {
        debug("Failed parsing line");
        close(fd);
        return read;
    }

    close(fd);
    return 0;
}

int read_pacman(board_t *board, int points) {
    pacman_t *pacman = &board->pacmans[0];
    pacman->alive = 1;
    pacman->points = points;

    // Caso nenhum arquivo de configuração tenha sido fornecido, usa valores
    // padrão
    if (board->pacman_file[0] == '\0') {
        pacman->passo = 0;
        pacman->waiting = 0;
        pacman->n_moves =
            0; // Controlado pelo usuário (sem movimentos pré-definidos)

        // Posição padrão -> encontra a primeira célula livre no tabuleiro
        for (int i = 0; i < board->height; i++) {
            for (int j = 0; j < board->width; j++) {
                int idx = i * board->width + j;
                if (board->board[idx].content == ' ') {
                    pacman->pos_x = j;
                    pacman->pos_y = i;
                    board->board[idx].content = 'P';
                    goto pacman_inserted;
                }
            }
        }

    pacman_inserted:
        return 0;
    }

    // Abre o arquivo de configuração do Pacman
    int fd = open(board->pacman_file, O_RDONLY);

    int read;
    char command[MAX_COMMAND_LENGTH];

    // Loop para ler o cabeçalho de configuração (PASSO, POS)
    while ((read = read_line(fd, command)) > 0) {
        // Ignora comentários e linhas vazias
        if (command[0] == '#' || command[0] == '\0')
            continue;

        char *word = strtok(command, " \t\n");
        if (!word)
            continue; // skip empty line

        // Define o passo do Pacman
        if (strcmp(word, "PASSO") == 0) {
            char *arg = strtok(NULL, " \t\n");
            if (arg) {
                pacman->passo = atoi(arg);
                pacman->waiting = pacman->passo;
                debug("Pacman passo: %d\n", pacman->passo);
            }
        }
        // Define a posição inicial do Pacman
        else if (strcmp(word, "POS") == 0) {
            char *arg1 = strtok(NULL, " \t\n");
            char *arg2 = strtok(NULL, " \t\n");
            if (arg1 && arg2) {
                pacman->pos_x = atoi(arg1);
                pacman->pos_y = atoi(arg2);
                int idx = pacman->pos_y * board->width + pacman->pos_x;
                board->board[idx].content = 'P';
                debug("Pacman Pos = %d x %d\n", pacman->pos_x, pacman->pos_y);
            }
        }
        // Se encontrar algo que não é configuração, assume início dos
        // movimentos
        else {
            break;
        }
    }

    if (read == -1) {
        debug("Failed reading line\n");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int read_ghosts(board_t *board) {
    // Itera sobre todos os fantasmas definidos no arquivo de nível
    for (int i = 0; i < board->n_ghosts; i++) {
        int fd = open(board->ghosts_files[i], O_RDONLY);
        ghost_t *ghost = &board->ghosts[i];

        int read;
        char command[MAX_COMMAND_LENGTH];

        // Loop para ler o cabeçalho de configuração (PASSO, POS)
        while ((read = read_line(fd, command)) > 0) {
            // Ignora comentários e linhas vazias
            if (command[0] == '#' || command[0] == '\0')
                continue;

            char *word = strtok(command, " \t\n");
            if (!word)
                continue; // skip empty line

            // Define a velocidade/passo do Fantasma
            if (strcmp(word, "PASSO") == 0) {
                char *arg = strtok(NULL, " \t\n");
                if (arg) {
                    ghost->passo = atoi(arg);
                    ghost->waiting = ghost->passo;
                    debug("Ghost passo: %d\n", ghost->passo);
                }
            }
            // Define a posição inicial do Fantasma
            else if (strcmp(word, "POS") == 0) {
                char *arg1 = strtok(NULL, " \t\n");
                char *arg2 = strtok(NULL, " \t\n");
                if (arg1 && arg2) {
                    ghost->pos_x = atoi(arg1);
                    ghost->pos_y = atoi(arg2);
                    int idx = ghost->pos_y * board->width + ghost->pos_x;
                    board->board[idx].content = 'M';
                    debug("Ghost Pos = %d x %d\n", ghost->pos_x, ghost->pos_y);
                }
            }
            // Se encontrar algo que não é configuração, assume início dos
            // movimentos
            else {
                break;
            }
        }

        // O restante do arquivo contém a sequência de movimentos
        ghost->current_move = 0;

        // 'command' aqui ainda contém a linha que quebrou o loop anterior
        // (primeiro movimento)
        int move = 0;
        while (read > 0 && move < MAX_MOVES) {
            if (command[0] == '#' || command[0] == '\0')
                continue;

            // Comandos de movimento simples (1 turno)
            if (command[0] == 'A' || command[0] == 'D' || command[0] == 'W' ||
                command[0] == 'S' || command[0] == 'R' ||
                command[0] == 'C') { // 'C' é o movimento especial (Charge)
                ghost->moves[move].command = command[0];
                ghost->moves[move].turns = 1;
                move += 1;
            }
            // Comando de espera (T <tempo>)
            else if (command[0] == 'T' && command[1] == ' ') {
                int t = atoi(command + 2);
                if (t > 0) {
                    ghost->moves[move].command = command[0];
                    ghost->moves[move].turns = t;
                    ghost->moves[move].turns_left = t;
                    move += 1;
                }
            }
            read = read_line(fd, command);
        }
        ghost->n_moves = move;

        if (read == -1) {
            debug("Failed reading line\n");
            close(fd);
            return -1;
        }
        close(fd);
    }

    return 0;
}

int read_line(int fd, char *buf) {
    int i = 0;
    char c;
    ssize_t n;

    // Lê o arquivo caractere por caractere
    while ((n = read(fd, &c, 1)) == 1) {
        // Ignora o caractere de retorno de carro (comum em arquivos Windows)
        if (c == '\r')
            continue;

        // Se encontrar uma nova linha, termina a leitura da linha atual
        if (c == '\n')
            break;

        // Adiciona o caractere ao buffer e incrementa o índice
        buf[i++] = c;

        // Se o buffer estiver cheio (menos 1 para o terminador nulo), para a
        // leitura
        if (i == MAX_COMMAND_LENGTH - 1)
            break;
    }

    // Adiciona o terminador nulo ao final da string lida
    buf[i] = '\0';

    // Se houve erro na leitura, retorna -1
    if (n == -1)
        return -1;

    // Se chegou ao fim do arquivo (EOF) e nada foi lido, retorna 0
    if (n == 0 && i == 0)
        return 0;

    // Retorna o número de caracteres lidos
    return i;
}

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/protocol.h"

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define BUFFER_SIZE 10

connect_req *buffer_clientes[BUFFER_SIZE]; // buffer produtor-consumidor
info_board info_b;
int buf_in = 0;  // indice de escrita
int buf_out = 0; // indice de leitura

info_client **active_sessions = NULL;
pthread_mutex_t registry_mutex =
    PTHREAD_MUTEX_INITIALIZER; // mutex para a lista de info_c

pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER; // mutex do buffer
sem_t sem_vazios; // espaços livres do buffer
sem_t sem_cheios; // pedidos do buffer

volatile sig_atomic_t continuar_server = 1;
volatile sig_atomic_t top_requested = 0;

typedef struct {
    board_t *board;
    int ghost_index;
    volatile bool *level_running;
} ghost_thread_arg_t;

typedef struct {
    board_t *board;
    info_client *info_client;
    volatile bool *level_running;
} pacman_thread_arg_t;

typedef struct {
    int id;
    int points;
} client_score_t;

// para o qsort
int compare_scores(const void *a, const void *b) {
    return ((client_score_t *)b)->points - ((client_score_t *)a)->points;
}

void handle_usr1() {
    pthread_mutex_lock(&registry_mutex);
    client_score_t *scores = malloc(info_b.max_games * sizeof(client_score_t));
    int count = 0;

    for (int i = 0; i < info_b.max_games; i++) {
        if (active_sessions[i] != NULL) {
            scores[count].id = active_sessions[i]->client_id;
            scores[count].points =
                active_sessions[i]->board_client.accumulated_points;
            count++;
        }
    }

    if (count > 0) {
        qsort(scores, count, sizeof(client_score_t), compare_scores);
    }

    FILE *f = fopen("top_5_clients", "w");
    if (f) {
        int limit = (count < 5) ? count : 5; // se tivermos menos de 5 usar isso
        for (int i = 0; i < limit; i++) {
            // +1 pq 0 indexing
            fprintf(f, "%d. Client ID: %d ; Points: %d\n", i + 1, scores[i].id,
                    scores[i].points);
        }
        fclose(f);
        debug("Criado ficheiro para o top\n");
    }

    free(scores);
    pthread_mutex_unlock(&registry_mutex);

    return;
}

void handle_signal(int sig) {
    switch (sig) {
    case SIGINT:
        continuar_server = 0;
        debug("SIGINT recebido\n");
        break;

        break;
    case SIGUSR1:
        debug("SIGUSR1 recebido");
        top_requested = 1;
        break;
    }
    return;
}

void *pacman_thread(void *arg) {
    pacman_thread_arg_t *pacman_arg = (pacman_thread_arg_t *)arg;
    board_t *board = pacman_arg->board;
    pacman_t *pacman = &board->pacmans[0];
    info_client *info_client = pacman_arg->info_client;

    // Aloca memória para o valor de retorno da thread
    int *retval = malloc(sizeof(int));
    if (!retval)
        return NULL;

    while (true) {
        // Verifica se o Pacman ainda está vivo antes de tentar mover
        if (!pacman->alive || continuar_server == false) {
            *retval = QUIT_GAME;
            *pacman_arg->level_running = false;
            return (void *)retval;
        }

        // Controla a velocidade do Pacman (tempo base * fator de passo)
        sleep_ms(board->tempo * (1 + pacman->passo));

        command_t *play;
        command_t c;
        char buffer[2];
        int n = read(info_client->req_pipe, &buffer, sizeof(buffer));

        if (n == -1 && errno == EAGAIN) {
            continue;
        }

        if (buffer[0] == OP_CODE_DISCONNECT) {
            debug("Client requested disconnect.\n");
            *retval = QUIT_GAME;
            *pacman_arg->level_running = false;
            return (void *)retval;
        }

        if (n <= 0 || buffer[0] != OP_CODE_PLAY) { // Pipe closed or error
            *retval = QUIT_GAME;
            *pacman_arg->level_running = false;
            return (void *)retval;
        }

        // Modo Manual: Lê entrada do teclado
        c.command = buffer[1];

        if (c.command == '\0') {
            continue; // Nenhum input, tenta novamente
        }

        c.turns = 1;
        play = &c;

        debug("KEY %c\n", play->command);

        // QUIT: Sai do jogo
        if (play->command == 'Q') {
            *retval = QUIT_GAME;
            *pacman_arg->level_running = false;
            return (void *)retval;
        }

        // Bloqueia o estado para leitura (rdlock)
        // Permite que outras threads (fantasmas) também leiam/movam-se
        // simultaneamente, mas impede que a thread de desenho (ncurses)
        // atualize a tela durante o movimento.
        pthread_rwlock_wrlock(&board->state_lock);

        // Executa o movimento
        int result = move_pacman(board, 0, play);

        pthread_rwlock_unlock(&board->state_lock);

        // Verifica o resultado do movimento
        if (result == REACHED_PORTAL) {
            // Nível completado
            *retval = NEXT_LEVEL;
            break; // Sai do loop para finalizar a thread e carregar próximo
                   // nível
        }

        if (result == DEAD_PACMAN) {
            // Pacman morreu
            //*retval = LOAD_BACKUP;
            *retval = QUIT_GAME;
            break; // Sai do loop para recarregar backup ou terminar
        }
    }

    // Garante que o lock seja liberado em caso de break
    pthread_rwlock_unlock(&board->state_lock);
    *pacman_arg->level_running = false;
    return (void *)retval;
}

void *ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t *)arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;
    volatile bool *level_running = ghost_arg->level_running;
    free(ghost_arg);

    ghost_t *ghost = &board->ghosts[ghost_ind];

    while (continuar_server) {
        sleep_ms(board->tempo * (1 + ghost->passo));
        if (!continuar_server || !(*level_running)) return NULL;

        pthread_rwlock_wrlock(&board->state_lock);
        if (board->thread_shutdown || !(*level_running)) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }

        int res =
            move_ghost(board, ghost_ind,
                       &ghost->moves[ghost->current_move % ghost->n_moves]);

        if (res == DEAD_PACMAN) {
            *level_running = false;
        }
        pthread_rwlock_unlock(&board->state_lock);
    }
    return NULL;
}

char *board_to_string(board_t *board) {
    if (!board || !board->board || board->width <= 0 || board->height <= 0) {
        return NULL;
    }

    size_t buffer_size = (board->width * board->height) + 1;
    char *result = malloc(buffer_size);
    if (!result)
        return NULL;

    int pos = 0;
    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            board_pos_t *cell = &board->board[y * board->width + x];

            char display_char;
            if (cell->content != '\0' && cell->content != ' ') {
                switch (cell->content) {
                case 'P':
                    display_char = 'C';
                    break;
                case 'M':
                    display_char = 'M';
                    break;
                case 'W':
                    display_char = '#';
                    break;
                default:
                    display_char = cell->content;
                    break;
                }
            } else if (cell->has_dot) {
                display_char = '.';
            } else if (cell->has_portal) {
                display_char = '@';
            } else {
                display_char = ' ';
            }

            result[pos++] = display_char;
        }
    }
    result[pos] = '\0';

    return result;
}

void write_board_to_notif(notif_men *n, Board *b) {
    n->width = b->width;
    n->height = b->height;
    n->tempo = b->tempo;
    n->victory = b->victory;
    n->game_over = b->game_over;
    n->accumulated_points = b->accumulated_points;
    n->board_data = b->data;
}

int send_notif(int fd, notif_men *n) {
    debug("sending notif\n");
    if (write(fd, &n->opcode, sizeof(n->opcode)) == -1)
        return -1;
    if (write(fd, &n->width, sizeof(n->width)) == -1)
        return -1;
    if (write(fd, &n->height, sizeof(n->height)) == -1)
        return -1;
    if (write(fd, &n->tempo, sizeof(n->tempo)) == -1)
        return -1;
    if (write(fd, &n->victory, sizeof(n->victory)) == -1)
        return -1;
    if (write(fd, &n->game_over, sizeof(n->game_over)) == -1)
        return -1;
    if (write(fd, &n->accumulated_points, sizeof(n->accumulated_points)) == -1)
        return -1;
    return 0;
}

int play_game(info_client *info_client) {

    DIR *level_dir = opendir(info_b.levels_directory);

    if (level_dir == NULL) {
        fprintf(stderr, "Failed to open directory: %s\n",
                info_b.levels_directory);
        return 0;
    }

    int accumulated_points = 0;
    bool end_game = false;
    board_t game_board;
    char *last_valid_board_str = NULL;

    struct dirent *entry;
    // Loop principal: itera sobre todos os arquivos do diretório de níveis
    while ((entry = readdir(level_dir)) != NULL && !end_game) {
        // Ignora arquivos ocultos (que começam com '.') e diretórios '.' e '..'
        if (entry->d_name[0] == '.')
            continue;

        // Encontra a extensão do arquivo
        char *dot = strrchr(entry->d_name, '.');
        if (!dot)
            continue; // Pula se não tiver extensão

        // Verifica se é um arquivo de nível (.lvl)
        if (strcmp(dot, ".lvl") == 0) {
            // Carrega o nível, mantendo os pontos acumulados de níveis
            // anteriores
            load_level(&game_board, entry->d_name, info_b.levels_directory,
                       accumulated_points);
            info_client->board_client.width = game_board.width;
            info_client->board_client.height = game_board.height;
            info_client->board_client.tempo = game_board.tempo;
            info_client->board_client.victory = 0;
            info_client->board_client.game_over = 0;

            // Loop de execução do nível
            while (true) {
                pthread_t pacman_tid;
                pthread_t *ghost_tids =
                    malloc(game_board.n_ghosts * sizeof(pthread_t));
                if (!ghost_tids)
                    return 0;

                game_board.thread_shutdown = 0;
                debug("Creating threads\n");
                volatile bool level_running = true;

                // Cria as threads do jogo
                // 1. Thread do Pacman (Lógica do jogador)
                pacman_thread_arg_t *a =
                    (pacman_thread_arg_t *)malloc(sizeof(pacman_thread_arg_t));
                if (!a) {
                    free(ghost_tids);
                    return 0;
                }
                a->info_client = info_client;
                a->board = &game_board;
                a->level_running = &level_running;
                if (pthread_create(&pacman_tid, NULL, pacman_thread,
                                   (void *)a) != 0) {
                    free(a);
                    free(ghost_tids);
                    return 0;
                }

                debug("pacman threads created\n");
                // 2. Threads dos Fantasmas (Lógica dos inimigos)
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    ghost_thread_arg_t *arg =
                        malloc(sizeof(ghost_thread_arg_t));
                    if (!arg) {
                        free(a);
                        free(ghost_tids);
                        return 0;
                    }
                    arg->board = &game_board;
                    arg->ghost_index = i;
                    arg->level_running = &level_running;
                    if (pthread_create(&ghost_tids[i], NULL, ghost_thread,
                                       (void *)arg) != 0) {
                        free(ghost_tids);
                        free(a);
                        free(arg);
                        return 0;
                    }
                }
                debug("monsters threads created\n");
                // Loop de atualização do cliente
                while (level_running) {
                    // Envia atualização do board
                    debug("game running\n");
                    pthread_rwlock_rdlock(&game_board.state_lock);
                    info_client->board_client.data =
                        board_to_string(&game_board);
                    info_client->board_client.accumulated_points =
                        game_board.pacmans[0].points;
                    pthread_rwlock_unlock(&game_board.state_lock);
                    notif_men notif;
                    notif.opcode = OP_CODE_BOARD;
                    write_board_to_notif(&notif, &info_client->board_client);

                    // Envia para o pipe de notificação
                    if (send_notif(info_client->notif_pipe, &notif) == -1) {
                        level_running = false;
                        free(ghost_tids);
                        free(a);
                        break;
                    }
                    if (notif.board_data) {
                        if (write(info_client->notif_pipe, notif.board_data,
                                  strlen(notif.board_data)) == -1) {
                            free(ghost_tids);
                            free(a);
                            break;
                        }
                        free(notif.board_data);
                    }
                    if (!notif.board_data)
                        debug("board data invalid\n");
                    sleep_ms(game_board.tempo);
                }

                // Aguarda o término da thread do Pacman
                int *retval;
                pthread_join(pacman_tid, (void **)&retval);
                debug("pacman thread joined\n");

                // Sinaliza para as outras threads pararem
                pthread_rwlock_wrlock(&game_board.state_lock);
                game_board.thread_shutdown = 1;
                pthread_rwlock_unlock(&game_board.state_lock);

                for (int i = 0; i < game_board.n_ghosts; i++) {
                    pthread_join(ghost_tids[i], NULL);
                }
                debug("monsters thread joined\n");

                free(ghost_tids);
                free(a);

                int result = *retval;
                free(retval);

                // Processa o resultado do nível

                if (result == NEXT_LEVEL) {
                    debug("next level\n");
                    info_client->board_client.victory = 1;
                    info_client->board_server = game_board;
                    info_client->board_client.data =
                        board_to_string(&game_board);

                    // Manda para o cliente o board para display da victory
                    notif_men notif;
                    notif.opcode = OP_CODE_BOARD;
                    write_board_to_notif(&notif, &info_client->board_client);
                    send_notif(info_client->notif_pipe, &notif);

                    if (notif.board_data) {
                        (void)!write(info_client->notif_pipe, notif.board_data,
                                     strlen(notif.board_data));
                        free(notif.board_data);
                    }

                    sleep_ms(500);
                    break;
                }

                if (result == QUIT_GAME) {
                    debug("quit game\n");
                    info_client->board_client.game_over = 1;
                    info_client->board_server = game_board;
                    info_client->board_client.data =
                        board_to_string(&game_board);

                    // Manda para o cliente o board para display do game over
                    notif_men notif;
                    notif.opcode = OP_CODE_BOARD;
                    write_board_to_notif(&notif, &info_client->board_client);
                    send_notif(info_client->notif_pipe, &notif);

                    if (notif.board_data) {
                        (void)!write(info_client->notif_pipe, notif.board_data,
                                     strlen(notif.board_data));
                        free(notif.board_data);
                    }

                    sleep(1);
                    end_game = true;
                    break;
                }
            }
            accumulated_points = game_board.pacmans[0].points;

            pthread_rwlock_rdlock(&game_board.state_lock);
            if (last_valid_board_str) free(last_valid_board_str);
                last_valid_board_str = board_to_string(&game_board);
            pthread_rwlock_unlock(&game_board.state_lock);

            unload_level(&game_board);
        }
    }

    if (entry == NULL) {
        // Manda para o cliente o board para display de end of game
        info_client->board_client.game_over = 1;
        info_client->board_client.victory = 1;
        info_client->board_server = game_board;
        info_client->board_client.data = last_valid_board_str;
        last_valid_board_str = NULL;

        notif_men notif;
        notif.opcode = OP_CODE_BOARD;
        write_board_to_notif(&notif, &info_client->board_client);
        send_notif(info_client->notif_pipe, &notif);

        if (notif.board_data) {
            (void)!write(info_client->notif_pipe, notif.board_data,
                         strlen(notif.board_data));
            free(notif.board_data);
        }

        sleep(1);
    }

    info_client->board_server = game_board;
    if (closedir(level_dir) == -1) {
        fprintf(stderr, "Failed to close directory\n");
        return 0;
    }

    return 1;
}

void *session_thread(void *arg) {
    int index = *(int *)arg;
    free(arg); // Clean up the passed index
    while (true) {

        sem_wait(&sem_cheios); // Espera por pedidos

        pthread_mutex_lock(&buffer_mutex);
        if (!continuar_server && buf_in == buf_out) {
            pthread_mutex_unlock(&buffer_mutex);
            break;
        }

        connect_req *req = buffer_clientes[buf_out];
        buf_out = (buf_out + 1) % BUFFER_SIZE;
        pthread_mutex_unlock(&buffer_mutex);

        sem_post(&sem_vazios); // Avisa que libertamos um espaço no buffer

        info_client *info_c = malloc(sizeof(info_client));
        memset(info_c, 0, sizeof(info_client));

        pthread_mutex_lock(&registry_mutex);
        active_sessions[index] = info_c;
        pthread_mutex_unlock(&registry_mutex);

        strncpy(info_c->req_pipe_path, req->req_pipe_path,
                MAX_PIPE_PATH_LENGTH);
        strncpy(info_c->notif_pipe_path, req->notif_pipe_path,
                MAX_PIPE_PATH_LENGTH);
        info_c->req_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';
        info_c->notif_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';

        char id_str[20];
        sscanf(info_c->req_pipe_path, "/tmp/%[^_]_request", id_str);
        info_c->client_id = atoi(id_str);

        free(req);

        int notif_fd = open(info_c->notif_pipe_path, O_WRONLY);
        if (notif_fd == -1) {
            continue;
        }
        info_c->notif_pipe = notif_fd;

        char response[2] = {OP_CODE_CONNECT, '0'};
        if (write(notif_fd, response, 2) == -1) {
            close(notif_fd);
            continue;
        }

        int req_fd = open(info_c->req_pipe_path, O_RDONLY | O_NONBLOCK);

        if (req_fd == -1) {
            close(info_c->notif_pipe);
            continue;
        }
        info_c->req_pipe = req_fd;

        play_game(info_c);

        pthread_mutex_lock(&registry_mutex);
        active_sessions[index] = NULL;
        pthread_mutex_unlock(&registry_mutex);

        close(info_c->notif_pipe);
        close(info_c->req_pipe);
        free(info_c);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <levels_dir> <max_games> <nome_do_FIFO_de_registo>\n",
               argv[0]);
        return -1;
    }

    info_b.levels_directory = argv[1];
    open_debug_file("debug.log");

    info_b.max_games = atoi(argv[2]);
    active_sessions = calloc(info_b.max_games, sizeof(info_client *));

    const char *register_pipe = argv[3];
    unlink(register_pipe);

    if (mkfifo(register_pipe, 0777) == -1) {
        if (errno != EEXIST) {
            debug("Failed to create register pipe: %s\n", argv[3]);
            return -1;
        }
    }

    sem_init(&sem_vazios, 0, BUFFER_SIZE); // Começa cheio de espaços vazios
    sem_init(&sem_cheios, 0, 0);           // Começa com 0 pedidos

    pthread_t *thread_pool = malloc(info_b.max_games * sizeof(pthread_t));
    if (thread_pool == NULL) {
        debug("Failed memory allocation for threads");
        return -1;
    }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGUSR1);

    // Block signals
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
        debug("error masking...\n");
        return -1;
    }

    // Aloca max_games threads
    for (int i = 0; i < info_b.max_games; i++) {
        int *worker_id = malloc(sizeof(int));
        *worker_id = i;
        if (pthread_create(&thread_pool[i], NULL, session_thread, worker_id) !=
            0) {
            debug("Failed to create worker thread");
            free(thread_pool);
            return -1;
        }
    }

    if (pthread_sigmask(SIG_UNBLOCK, &set, NULL) != 0) {
        debug("error unblocking...\n");
        return -1;
    }

    int fd = open(argv[3], O_RDWR);
    if (fd == -1) {
        debug("Failed to open register pipe: %s\n", argv[3]);
        return -1;
    }

    // prevenir crashar em pipe error
    struct sigaction sa_pipe;
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = 0;
    sigaction(SIGPIPE, &sa_pipe, NULL);

    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);

    while (continuar_server) {
        if (top_requested) { handle_usr1(); top_requested = 0; }
        connect_req req;
        int n = read(fd, &req, sizeof(req));

        if (n == -1) {
            if (errno == EINTR)
                continue;

            break;
        }

        if (n == 0)
            continue;

        if (n > 0 && req.opcode == OP_CODE_CONNECT) {
            connect_req *new_req = malloc(sizeof(connect_req));
            if (!new_req)
                break;
            *new_req = req;

            sem_wait(&sem_vazios); // Espera por espaços livres do buffer

            pthread_mutex_lock(&buffer_mutex);
            buffer_clientes[buf_in] = new_req;
            buf_in = (buf_in + 1) % BUFFER_SIZE;
            pthread_mutex_unlock(&buffer_mutex);

            sem_post(&sem_cheios); // Acorda as threads
        }
    }

    close(fd);
    unlink(register_pipe);

    for (int i = 0; i < info_b.max_games; i++) {
        sem_post(&sem_cheios);
    }

    for (int i = 0; i < info_b.max_games; i++) {
        pthread_join(thread_pool[i], NULL);
    }

    free(thread_pool);
    sem_destroy(&sem_vazios);
    sem_destroy(&sem_cheios);
    pthread_mutex_destroy(&buffer_mutex);
    return 0;
}

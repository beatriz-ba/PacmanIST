# Compiler variables
CC = gcc
CFLAGS = -g -Wall -Wextra -std=c17 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lncurses

# Directory variables
OBJ_DIR = obj
BIN_DIR = bin

CLIENT_DIR = src/client
SERVER_DIR = src/server

CLIENT_INCLUDES = -I src/client/include
SERVER_INCLUDES = -I src/server/include


# executable
SERVER = Pacmanist

#client
CLIENT = client


#Client objects
OBJS_CLIENT = client_main.o debug.o api.o display.o
OBJS_SERVER = server_main.o board.o parser.o

# Dependencies
display.o = display.h
board.o = board.h
parser.o = parser.h
api.o = api.h protocol.h

# Object files path
vpath %.o $(OBJ_DIR)
vpath %.c $(CLIENT_DIR) $(SERVER_DIR)

$(addprefix $(OBJ_DIR)/,$(OBJS_CLIENT)): INCLUDES := $(CLIENT_INCLUDES)
$(addprefix $(OBJ_DIR)/,$(OBJS_SERVER)): INCLUDES := $(SERVER_INCLUDES)

# Make targets
all: client server

client: $(BIN_DIR)/$(CLIENT)
server: $(BIN_DIR)/$(SERVER)

$(BIN_DIR)/$(CLIENT): $(addprefix $(OBJ_DIR)/,$(OBJS_CLIENT)) | folders
	$(CC) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/$(SERVER): $(addprefix $(OBJ_DIR)/,$(OBJS_SERVER)) | folders
	$(CC) $^ -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: %.c | folders
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# # # dont include LDFLAGS in the end, to allow compilation on macos
# $(OBJ_DIR)/%.o: %.c | folders
# 	$(CC) -I $(INCLUDES) $(CFLAGS) -o $(OBJ_DIR)/$@ -c $<

# Create folders
folders:
	mkdir -p $(OBJ_DIR)
	mkdir -p $(BIN_DIR)

# Clean object files and executable
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

-include $(OBJ_DIR)/*.d

# indentify targets that do not create files
.PHONY: all clean run folders server

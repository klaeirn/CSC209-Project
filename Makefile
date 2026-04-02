DEFAULT_PORT = 4242
CFLAGS = -Wall -Wextra -DDEFAULT_SERVER_PORT=$(DEFAULT_PORT)

all: server client

server: server.o connection.o protocol.o
	gcc $(CFLAGS) -o server server.o connection.o protocol.o -lm

client: client.o protocol.o
	gcc $(CFLAGS) -o client client.o protocol.o -lm

server.o: server.c
	gcc $(CFLAGS) -c server.c -o server.o

connection.o: connection.c
	gcc $(CFLAGS) -c connection.c -o connection.o

protocol.o: protocol.c
	gcc $(CFLAGS) -c protocol.c -o protocol.o

client.o: client.c
	gcc $(CFLAGS) -c client.c -o client.o

clean:
	rm -f server client server.o connection.o protocol.o client.o
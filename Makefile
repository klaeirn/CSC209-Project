all: server client

server: server.o connection.o protocol.o
	gcc -Wall -Wextra -o server server.o connection.o protocol.o -lm

client: client.o protocol.o
	gcc -Wall -Wextra -o client client.o protocol.o -lm

server.o: server.c
	gcc -Wall -Wextra -c server.c -o server.o

connection.o: connection.c
	gcc -Wall -Wextra -c connection.c -o connection.o

protocol.o: protocol.c
	gcc -Wall -Wextra -c protocol.c -o protocol.o

client.o: client.c
	gcc -Wall -Wextra -c client.c -o client.o

clean:
	rm -f server client server.o connection.o protocol.o client.o
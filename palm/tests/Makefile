# Desktop test for the wire reader, against replies saved from the live server.
CFLAGS = -std=c89 -pedantic -Wall -Wextra -I.. -I../common

all: fujitzee_test
	./fujitzee_test

fujitzee_test: fujitzee_test.c ../fzwire.c ../fzwire.h ../common/fn_types.h
	$(CC) $(CFLAGS) -o $@ fujitzee_test.c ../fzwire.c

clean:
	rm -f fujitzee_test

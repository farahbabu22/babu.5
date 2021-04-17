CC=gcc
CFLAGS=-Werror -Wall -Wextra -ggdb

default: oss user_proc

oss: oss.c oss.h
	$(CC) $(CFLAGS) oss.c -o oss

user_proc: user_proc.c oss.h
	$(CC) $(CFLAGS) user_proc.c -o user_proc

clean:
	rm -Rf oss user_proc *.o output.log *.dSYM

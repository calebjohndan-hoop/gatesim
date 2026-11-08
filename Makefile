gatesim: gatesim.c
	cc -Wall -Wextra -std=c11 -o gatesim gatesim.c

test: gatesim
	./tests.sh

clean:
	rm -f gatesim

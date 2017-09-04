all: compsize

compsize: compsize.cc
	g++ -Wall -Werror -o $@ $^ -ggdb

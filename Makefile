all: compsize
PREFIX ?= /
CXX=c++
CXXFLAGS ?= "-Wall"

compsize: compsize.cc
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f compsize

install:
	install -Dm755 compsize $(PREFIX)/usr/bin/compsize

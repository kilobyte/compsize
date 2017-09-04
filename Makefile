all: compsize
CXX=c++

compsize: compsize.cc
	$(CXX) -Wall -o $@ $^

clean:
	rm -f compsize

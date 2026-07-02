CXXFLAGS = -g -std=c++11 -Wall
LDFLAGS = -lmymuduo -lpthread

all: testserver benchclient

testserver: testserver.cc
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

benchclient: benchclient.cc
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

.PHONY: clean
clean:
	rm -f testserver benchclient
CXX ?= g++
CXXFLAGS ?= -Wall -std=c++17 -O2
LDLIBS = -lpthread -lnuma

OBJS = migration.o profile.o umigratord.o
HEADERS = migflow.h profile.h ../include/migflow_rb.h

all: umigratord

umigratord: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDLIBS)

%.o: %.cc $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) umigratord

.PHONY: all clean

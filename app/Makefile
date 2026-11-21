CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic

LDFLAGS        := -lext2fs -lcom_err
LDFLAGS_STATIC := -static -lext2fs -lcom_err -lpthread

PREFIX  ?= /usr/local
BINDIR  := $(DESTDIR)$(PREFIX)/bin

TARGETS := backpart.extfs restpart.extfs

.PHONY: build static install uninstall clean

build: $(TARGETS)

backpart.extfs: backpart.extfs.cpp extfs.hpp
	$(CXX) $(CXXFLAGS) backpart.extfs.cpp -o $@ $(LDFLAGS)

restpart.extfs: restpart.extfs.cpp extfs.hpp
	$(CXX) $(CXXFLAGS) restpart.extfs.cpp -o $@ $(LDFLAGS)

static: backpart.extfs.cpp restpart.extfs.cpp extfs.hpp
	$(CXX) $(CXXFLAGS) backpart.extfs.cpp -o backpart.extfs $(LDFLAGS_STATIC)
	$(CXX) $(CXXFLAGS) restpart.extfs.cpp -o restpart.extfs $(LDFLAGS_STATIC)
	strip $(TARGETS)

install: build
	install -d $(BINDIR)
	install -m 755 $(TARGETS) $(BINDIR)/

uninstall:
	rm -f $(addprefix $(BINDIR)/,$(TARGETS))

clean:
	rm -f $(TARGETS)

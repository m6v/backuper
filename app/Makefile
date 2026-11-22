CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic

LDFLAGS_EXTFS   := -lext2fs -lcom_err -lz
LDFLAGS_NTFS    := -lntfs-3g -lz
LDFLAGS_GENERIC := -lz

STATIC_EXTFS    := -static -lext2fs -lcom_err -lz -lpthread
STATIC_NTFS     := -static -lntfs-3g -lz -lpthread
STATIC_GENERIC  := -static -lz

PREFIX  ?= /usr/local
BINDIR  := $(DESTDIR)$(PREFIX)/bin

TARGETS := partback.extfs partback.ntfs partback.exfat partback.dd partrest.all

.PHONY: build static install uninstall clean

build: $(TARGETS)

partback.extfs: partback.extfs.cpp partimg.hpp
	$(CXX) $(CXXFLAGS) partback.extfs.cpp -o $@ $(LDFLAGS_EXTFS)

partback.ntfs: partback.ntfs.cpp partimg.hpp
	$(CXX) $(CXXFLAGS) partback.ntfs.cpp -o $@ $(LDFLAGS_NTFS)

partback.dd: partback.dd.cpp partimg.hpp
	$(CXX) $(CXXFLAGS) partback.dd.cpp -o $@ $(LDFLAGS_GENERIC)

partback.exfat: partback.exfat.cpp partimg.hpp
	$(CXX) $(CXXFLAGS) partback.exfat.cpp -o $@ $(LDFLAGS_GENERIC)

partrest.all: partrest.all.cpp partimg.hpp
	$(CXX) $(CXXFLAGS) partrest.all.cpp -o $@ $(LDFLAGS_GENERIC)

static: partback.extfs.cpp partback.ntfs.cpp partback.dd.cpp partrest.all.cpp partimg.hpp
	$(CXX) $(CXXFLAGS) partback.extfs.cpp -o partback.extfs $(STATIC_EXTFS)
	$(CXX) $(CXXFLAGS) partback.ntfs.cpp -o partback.ntfs $(STATIC_NTFS)
	$(CXX) $(CXXFLAGS) partback.dd.cpp -o partback.dd $(STATIC_GENERIC)
	$(CXX) $(CXXFLAGS) partrest.all.cpp -o partrest.all $(STATIC_GENERIC)
	$(CXX) $(CXXFLAGS) partback.exfat.cpp -o partback.exfat $(STATIC_GENERIC)
	strip $(TARGETS)

install: build
	install -d $(BINDIR)
	install -m 755 $(TARGETS) $(BINDIR)/

uninstall:
	rm -f $(addprefix $(BINDIR)/,$(TARGETS))

clean:
	rm -f $(TARGETS)

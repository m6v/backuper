CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic

PARTBACK_SRCS := partback.cpp backuper_extfs.cpp backuper_ntfs.cpp backuper_exfat.cpp backuper_raw.cpp
PARTBACK_HDRS := sysutil.hpp progress.hpp partimg.hpp imgwriter.hpp backuper.hpp \
                 backuper_extfs.hpp backuper_ntfs.hpp backuper_exfat.hpp backuper_raw.hpp

LDFLAGS_BACK := -lblkid -lext2fs -lcom_err -lntfs-3g -lz
LDFLAGS_REST := -lz

STATIC_BACK  := -static -lblkid -luuid -lext2fs -lcom_err -lntfs-3g -lz -lpthread
STATIC_REST  := -static -lz

PREFIX  ?= /usr/local
BINDIR  := $(DESTDIR)$(PREFIX)/bin

TARGETS := partback partrest

.PHONY: build static install uninstall clean test check

build: $(TARGETS)

partback: $(PARTBACK_SRCS) $(PARTBACK_HDRS)
	$(CXX) $(CXXFLAGS) $(PARTBACK_SRCS) -o $@ $(LDFLAGS_BACK)

partrest: partrest.cpp sysutil.hpp progress.hpp partimg.hpp
	$(CXX) $(CXXFLAGS) partrest.cpp -o $@ $(LDFLAGS_REST)

static: $(PARTBACK_SRCS) $(PARTBACK_HDRS) partrest.cpp
	$(CXX) $(CXXFLAGS) $(PARTBACK_SRCS) -o partback $(STATIC_BACK)
	$(CXX) $(CXXFLAGS) partrest.cpp -o partrest $(STATIC_REST)
	strip $(TARGETS)

install: build
	install -d $(BINDIR)
	install -m 755 $(TARGETS) $(BINDIR)/

uninstall:
	rm -f $(addprefix $(BINDIR)/,$(TARGETS))

test check: build
	PATH="$$PATH:/sbin:/usr/sbin" ./autotests.sh

clean:
	rm -f $(TARGETS)

# Build for Linux, macOS and MinGW. On MSVC use build.bat instead.
#
#   make            build the tool
#   make test       build and run the test suite (offline tiers)
#   make test-online / make test-full   include the network tiers

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -pedantic
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE

SRCDIR  := src
TESTDIR := test
OBJDIR  := build
TESTOBJ := $(OBJDIR)/test

# Everything except the CLI entry point, so the tests can link the same code.
LIBSRC  := $(filter-out $(SRCDIR)/main.c,$(wildcard $(SRCDIR)/*.c))
LIBOBJ  := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(LIBSRC))
MAINOBJ := $(OBJDIR)/main.o
TESTSRC := $(wildcard $(TESTDIR)/*.c)
TESTOBJS := $(patsubst $(TESTDIR)/%.c,$(TESTOBJ)/%.o,$(TESTSRC))

TARGET      := $(OBJDIR)/tabber
TESTTARGET  := $(TESTOBJ)/test_tabber

# HTTP comes from WinHTTP on Windows and from libcurl everywhere else.
ifeq ($(OS),Windows_NT)
  TARGET := $(OBJDIR)/tabber.exe
  TESTTARGET := $(TESTOBJ)/test_tabber.exe
  LDLIBS += -ladvapi32 -lole32 -lshell32 -luuid -lwinhttp -lws2_32
else
  LDLIBS += -lcurl
endif

all: $(TARGET)

$(TARGET): $(MAINOBJ) $(LIBOBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(TESTOBJ)/%.o: $(TESTDIR)/%.c | $(TESTOBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(SRCDIR) -c -o $@ $<

$(TESTTARGET): $(TESTOBJS) $(LIBOBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR) $(TESTOBJ):
	mkdir -p $@

test: $(TESTTARGET)
	$(TESTTARGET)

test-online: $(TESTTARGET)
	$(TESTTARGET) --online

test-full: $(TESTTARGET)
	$(TESTTARGET) --full

clean:
	rm -rf $(OBJDIR)

.PHONY: all test test-online test-full clean

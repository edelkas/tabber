# Build for Linux, macOS and MinGW. On MSVC use build.bat instead.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -pedantic
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE

SRCDIR  := src
OBJDIR  := build
SOURCES := $(wildcard $(SRCDIR)/*.c)
OBJECTS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))
TARGET  := $(OBJDIR)/tabber

# HTTP comes from WinHTTP on Windows and from libcurl everywhere else.
ifeq ($(OS),Windows_NT)
  TARGET := $(OBJDIR)/tabber.exe
  LDLIBS += -ladvapi32 -lole32 -lshell32 -luuid -lwinhttp
else
  LDLIBS += -lcurl
endif

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR)

.PHONY: all clean

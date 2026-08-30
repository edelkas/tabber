# Build for Linux, macOS and MinGW. On MSVC use build.bat instead.
#
#   make            build the tool
#   make test       build and run the test suite (offline tiers)
#   make test-online / make test-full   include the network tiers
#   make gui        build the graphical front-end
#   make all        both of the above, for this machine
#   make all test   ...and the suite after them, since goals are run in order
#   make clean      throw the objects away, so the next build starts over
#
# Only what changed is rebuilt, headers included; see DEPFLAGS below.
#
# Every finished executable is also copied to dist/ under the name it is
# released as, which carries the front-end and the platform. Nothing else
# goes there. Unlike build.bat there is one architecture here, the host's.

CC      ?= cc
CXX     ?= c++
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -pedantic
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE

# Diagnostics in English, whatever language the machine speaks: an error
# message is far easier to look up that way.
export LC_ALL := C

# Only what changed is rebuilt, and a changed header counts as a change: -MMD
# writes beside each object the list of headers it read, which is included at
# the foot of this file. -MP adds a target for each of those headers, so that
# deleting one leaves a rule with nothing to do rather than an error about a
# file some .d still remembers. `make clean` is the way to start over.
DEPFLAGS := -MMD -MP

SRCDIR  := src
TESTDIR := test
GUIDIR  := gui
OBJDIR  := build
TESTOBJ := $(OBJDIR)/test
GUIOBJ  := $(OBJDIR)/gui

# Everything except the CLI entry point, so the tests can link the same code.
LIBSRC  := $(filter-out $(SRCDIR)/main.c,$(wildcard $(SRCDIR)/*.c))
LIBOBJ  := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(LIBSRC))
MAINOBJ := $(OBJDIR)/main.o
TESTSRC := $(wildcard $(TESTDIR)/*.c)
TESTOBJS := $(patsubst $(TESTDIR)/%.c,$(TESTOBJ)/%.o,$(TESTSRC))

TARGET      := $(OBJDIR)/tabber
TESTTARGET  := $(TESTOBJ)/test_tabber
GUITARGET   := $(OBJDIR)/tabber-gui

UNAME_S := $(shell uname -s)

# HTTP comes from WinHTTP on Windows and from libcurl everywhere else.
ifeq ($(OS),Windows_NT)
  EXEEXT := .exe
  TARGET := $(OBJDIR)/tabber$(EXEEXT)
  TESTTARGET := $(TESTOBJ)/test_tabber$(EXEEXT)
  GUITARGET := $(OBJDIR)/tabber-gui$(EXEEXT)
  LDLIBS += -ladvapi32 -lole32 -lshell32 -luuid -lwinhttp -lws2_32
else
  EXEEXT :=
  LDLIBS += -lcurl
endif

# The platform half of a released name, spelled the way the tool's own build
# keys are (src/update.h), so the manifest and the binaries agree.
UNAME_M := $(shell uname -m)

ifeq ($(OS),Windows_NT)
  DIST_OS := windows
else ifeq ($(UNAME_S),Darwin)
  DIST_OS := macos
else
  DIST_OS := linux
endif

ifneq (,$(filter x86_64 amd64,$(UNAME_M)))
  DIST_ARCH := x64
else ifneq (,$(filter aarch64 arm64,$(UNAME_M)))
  DIST_ARCH := arm64
else ifneq (,$(filter i386 i486 i586 i686,$(UNAME_M)))
  DIST_ARCH := x86
else
  DIST_ARCH := $(UNAME_M)
endif

DISTDIR := dist
DISTCLI := $(DISTDIR)/tabber-cli-$(DIST_OS)-$(DIST_ARCH)$(EXEEXT)
DISTGUI := $(DISTDIR)/tabber-gui-$(DIST_OS)-$(DIST_ARCH)$(EXEEXT)

# Bare `make` still builds the tool alone: the GUI wants OpenGL and the window
# system's headers, which a machine that only needs the tool need not have.
.DEFAULT_GOAL := $(TARGET)

all: $(TARGET) $(GUITARGET)

$(TARGET): $(MAINOBJ) $(LIBOBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
	@mkdir -p $(DISTDIR)
	cp -f $@ $(DISTCLI)

# The fresh savefile is built into the binary (src/resource_save.c), so the
# executable is the whole program: nothing has to ship beside it.

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c -o $@ $<

$(TESTOBJ)/%.o: $(TESTDIR)/%.c | $(TESTOBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -I$(SRCDIR) -c -o $@ $<

$(TESTTARGET): $(TESTOBJS) $(LIBOBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR) $(TESTOBJ) $(GUIOBJ):
	mkdir -p $@

test: $(TESTTARGET)
	$(TESTTARGET)

test-online: $(TESTTARGET)
	$(TESTTARGET) --online

test-full: $(TESTTARGET)
	$(TESTTARGET) --full

# ---- The graphical front-end -------------------------------------------------
# Dear ImGui and GLFW are built from their sources under vendor/, not linked
# against a prebuilt library: a .lib or .a is per-compiler and per-architecture,
# and this tree already has three operating systems to keep happy. GLFW picks
# its window backend from a define, one per platform.

IMGUIDIR := vendor/imgui
GLFWDIR  := vendor/glfw

# ForkAwesome is two headers and nothing to compile: the icon font is baked into
# one of them and the names of its codepoints are in the other.
FKDIR    := vendor/forkawesome

IMGUISRC := imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp \
            imgui_demo.cpp
IMGUISRC += backends/imgui_impl_glfw.cpp backends/imgui_impl_opengl3.cpp

# Shared by every backend, the do-nothing one included.
GLFWSRC := context.c init.c input.c monitor.c platform.c vulkan.c window.c \
           egl_context.c osmesa_context.c \
           null_init.c null_monitor.c null_window.c null_joystick.c

ifeq ($(OS),Windows_NT)
  GLFWSRC += win32_init.c win32_joystick.c win32_module.c win32_monitor.c \
             win32_thread.c win32_time.c win32_window.c wgl_context.c
  GUIDEFS := -D_GLFW_WIN32 -DUNICODE -D_UNICODE
  # dwmapi: DwmFlush paces the frames here; see "Pacing the frames" in main.cpp.
  GUILIBS := -lopengl32 -lgdi32 -lshell32 -luser32 -ldwmapi
  # -mwindows: a windowed program, so no console is opened alongside it.
  GUILINK := -mwindows
else ifeq ($(UNAME_S),Darwin)
  GLFWSRC += cocoa_init.m cocoa_joystick.m cocoa_monitor.m cocoa_window.m \
             nsgl_context.m macos_time.c posix_module.c posix_thread.c
  GUIDEFS := -D_GLFW_COCOA
  GUILIBS := -framework Cocoa -framework IOKit -framework QuartzCore \
             -framework OpenGL
else
  # X11 only. Wayland would also need its protocol headers generated by
  # wayland-scanner at build time, which is more than this Makefile should
  # know; under a Wayland session XWayland runs the window all the same.
  GLFWSRC += x11_init.c x11_monitor.c x11_window.c xkb_unicode.c glx_context.c \
             posix_module.c posix_time.c posix_thread.c posix_poll.c
  ifeq ($(UNAME_S),Linux)
    GLFWSRC += linux_joystick.c
  endif
  GUIDEFS := -D_GLFW_X11
  GUILIBS := -lGL -lX11 -lpthread -ldl -lm
endif

GUIINC   := -I$(SRCDIR) -I$(IMGUIDIR) -I$(IMGUIDIR)/backends -I$(GLFWDIR)/include \
            -I$(FKDIR)
# The backend files sit a directory down, but every object lands in one place;
# no two of these share a basename, and none of them clashes with the tool's.
GUIOBJS  := $(addprefix $(GUIOBJ)/,$(notdir $(IMGUISRC:.cpp=.o)))
GUIOBJS  += $(addprefix $(GUIOBJ)/,$(patsubst %.c,%.o,$(patsubst %.m,%.o,$(GLFWSRC))))
GUIOBJS  += $(GUIOBJ)/main.o

# Vendored code is compiled without the project's warning flags: its warnings
# are upstream's to fix, and they would bury ours. Our own GUI code keeps them.
VENDORCFLAGS   := -std=c99 -O2 $(DEPFLAGS) $(GUIDEFS) $(GUIINC)
VENDORCXXFLAGS := -std=c++11 -O2 $(DEPFLAGS) $(GUIINC)

gui: $(GUITARGET)

# The tool's own objects go in too: the GUI is a second front-end onto the same
# library, not a second copy of it. They sit in $(OBJDIR) while the GUI's sit in
# $(GUIOBJ), which is what keeps our platform.o apart from GLFW's.
$(GUITARGET): $(GUIOBJS) $(LIBOBJ)
	$(CXX) -o $@ $^ $(GUILIBS) $(GUILINK) $(LDLIBS)
	@mkdir -p $(DISTDIR)
	cp -f $@ $(DISTGUI)

$(GUIOBJ)/main.o: $(GUIDIR)/main.cpp | $(GUIOBJ)
	$(CXX) $(VENDORCXXFLAGS) -Wall -Wextra -c -o $@ $<

$(GUIOBJ)/%.o: $(IMGUIDIR)/%.cpp | $(GUIOBJ)
	$(CXX) $(VENDORCXXFLAGS) -c -o $@ $<

$(GUIOBJ)/%.o: $(IMGUIDIR)/backends/%.cpp | $(GUIOBJ)
	$(CXX) $(VENDORCXXFLAGS) -c -o $@ $<

$(GUIOBJ)/%.o: $(GLFWDIR)/src/%.c | $(GUIOBJ)
	$(CC) $(VENDORCFLAGS) $(CPPFLAGS) -c -o $@ $<

$(GUIOBJ)/%.o: $(GLFWDIR)/src/%.m | $(GUIOBJ)
	$(CC) $(VENDORCFLAGS) $(CPPFLAGS) -c -o $@ $<

# What each object read, as -MMD left it. They are included last because the
# GUI's objects are only known by here, and missing on the first build, which
# is what the leading dash allows.
DEPFILES := $(MAINOBJ:.o=.d) $(LIBOBJ:.o=.d) $(TESTOBJS:.o=.d) $(GUIOBJS:.o=.d)
-include $(DEPFILES)

clean:
	rm -rf $(OBJDIR)

.PHONY: all test test-online test-full gui clean

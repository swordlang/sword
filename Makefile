CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
PREFIX   ?= $(HOME)/.local
# The front end is shared: the language server is the same lexer, parser and
# checker behind a different mouth.
CORE     := $(filter-out src/main.cpp, $(wildcard src/*.cpp))
COREOBJ  := $(CORE:.cpp=.o)
LSPSRC   := $(wildcard lsp/*.cpp)
LSPOBJ   := $(LSPSRC:.cpp=.o)
OBJ      := $(COREOBJ) src/main.o $(LSPOBJ)
BIN      := shield
LSP      := swordls
RT       := libsword_rt.a
# What a compiled program has to be linked against beyond the archive. Written
# at build time and read by the compiler, so the archive and its link line travel
# together — a program that uses TLS needs libssl, and only the build knows where
# that is on this machine.
RTFLAGS  := libsword_rt.flags
RTOBJ    := rt/sword_rt.o rt/sword_net.o rt/sword_os.o rt/sword_poll.o \
            rt/sword_fs.o rt/sword_proc.o rt/sword_chan.o rt/sword_tls.o \
            rt/sword_dl.o rt/sword_ctx.o

# A section per function, so that a linker outside Apple's can drop what a
# program never calls (ld64 does it by symbol, and the objects come out the same).
RT_SECTIONS := -ffunction-sections -fdata-sections
$(RTOBJ): CXXFLAGS += $(RT_SECTIONS)

# TLS is optional: the protocol is not something to write by hand, so it is
# OpenSSL or nothing. Found here, or the runtime answers "built without TLS" and
# everything else works as before. SWORD_OPENSSL=<prefix> overrides the search;
# SWORD_NO_TLS=1 skips it.
OPENSSL_PREFIX := $(strip $(SWORD_OPENSSL))
ifeq ($(OPENSSL_PREFIX),)
OPENSSL_PREFIX := $(firstword $(wildcard \
    /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3 \
    /opt/homebrew/opt/openssl /usr/local/opt/openssl /usr))
endif
ifeq ($(SWORD_NO_TLS),1)
OPENSSL_PREFIX :=
endif
ifneq ($(strip $(wildcard $(OPENSSL_PREFIX)/include/openssl/ssl.h)),)
# The system's own copy is already on every search path, and naming
# /usr/include with -I moves it ahead of the C++ library's wrappers around the
# C headers, which then cannot find what they wrap.
ifeq ($(OPENSSL_PREFIX),/usr)
TLS_CXXFLAGS := -DSWORD_HAVE_TLS=1
TLS_LDFLAGS  := -lssl -lcrypto
else
TLS_CXXFLAGS := -DSWORD_HAVE_TLS=1 -I$(OPENSSL_PREFIX)/include
TLS_LDFLAGS  := -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
endif
TLS_STAMP    := rt/.tls-on
else
TLS_STAMP    := rt/.tls-off
endif

# The runtime is C++, so a program links against whichever standard library the
# compiler that built it uses: libc++ from Apple's, libstdc++ from GCC's. Asked
# of the compiler rather than guessed from the system, since clang on Linux can
# be either. libstdc++ is named by its directory as well: clang links against the
# newest GCC it finds, and on Ubuntu 24.04 that is a GCC 14 with no libstdc++
# beside it while g++ is 13.
CXXLIB := $(if $(shell $(CXX) -std=c++17 -dM -E -x c++ -include cstddef \
    /dev/null 2>/dev/null | grep _LIBCPP_VERSION),-lc++,-lstdc++)
ifeq ($(CXXLIB),-lstdc++)
CXXLIBDIR := $(dir $(shell $(CXX) -print-file-name=libstdc++.so))
ifneq ($(filter /%,$(CXXLIBDIR)),)
CXXLIB := -L$(CXXLIBDIR) -lstdc++
endif
endif

# std/dl's dlopen is in libc on macOS and on glibc from 2.34; older glibc keeps
# it in libdl, and naming libdl where it is only a stub costs nothing.
DLLIB := $(if $(filter Linux,$(shell uname -s)),-ldl)

all: $(BIN) $(LSP) $(RT) $(RTFLAGS)

$(BIN): $(COREOBJ) src/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(LSP): $(COREOBJ) $(LSPOBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Linked into every compiled program that spawns anything.
$(RT): $(RTOBJ)
	ar rcs $@ $^

# Checked every build, because the answer depends on what is installed on the
# machine rather than on any file make can date it against — but only written when
# it changes, so that the TLS object below is rebuilt exactly when the decision
# does.
$(RTFLAGS): FORCE
	@echo '$(TLS_LDFLAGS) $(DLLIB) $(CXXLIB)' > $@

# The decision is in the file's name, so flipping it leaves a prerequisite that
# does not exist and the object is rebuilt. Comparing timestamps would not do:
# make 3.81, which is what macOS ships, dates files to the second, and a rewrite
# in the same second as the build it should invalidate is invisible to it.
$(TLS_STAMP):
	@rm -f rt/.tls-on rt/.tls-off rt/sword_tls.o
	@touch $@

FORCE:

rt/sword_tls.o: rt/sword_tls.cpp $(TLS_STAMP)
	$(CXX) $(CXXFLAGS) $(TLS_CXXFLAGS) -MMD -MP -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# The standard library's errors, one name a line, for the codes they keep in
# every program (src/types.cpp). A declaration sits at the start of its line.
STDSRC := $(shell find std -name '*.sword' ! -name '*_test.sword')
src/std_errors.inc: $(STDSRC)
	LC_ALL=C sed -n 's/^error[[:space:]][[:space:]]*//p' $(STDSRC) | \
	    sed -e 's|//.*||' -e 's/=.*//' | tr ',' '\n' | tr -d ' \t' | \
	    grep . | LC_ALL=C sort -u | sed 's/.*/"&",/' > $@

src/types.o: src/std_errors.inc

%.o: %.S
	$(CXX) -c $< -o $@

# A runtime for another machine, beside this one's: `make TARGET=<triple>`, after
# which `shield --target=<triple>` links against it. clang is a cross compiler by
# nature; what it needs is the other machine's C and C++ headers and libraries,
# as SYSROOT=<dir> unless they are this machine's own (another Apple
# architecture, say). Built without TLS: where OpenSSL for another machine lives
# is not something to guess. The context switch is written for x86-64 and
# aarch64, so those are the two it builds for.
ifneq ($(TARGET),)
TARGET_RT    := libsword_rt-$(TARGET).a
TARGET_FLAGS := libsword_rt-$(TARGET).flags
TARGET_DIR   := rt/$(TARGET)
TARGET_OBJ   := $(patsubst rt/%,$(TARGET_DIR)/%,$(RTOBJ))
TARGET_CXX   := clang++ --target=$(TARGET) $(if $(SYSROOT),--sysroot=$(SYSROOT))
TARGET_LIBS  ?= $(if $(findstring apple,$(TARGET)),-lc++,-ldl -lstdc++)
TARGET_AR    := $(firstword $(shell command -v llvm-ar 2>/dev/null) ar)

all: $(TARGET_RT) $(TARGET_FLAGS)

$(TARGET_RT): $(TARGET_OBJ)
	$(TARGET_AR) rcs $@ $^

$(TARGET_FLAGS): FORCE
	@echo '$(TARGET_LIBS)' > $@

$(TARGET_DIR)/%.o: rt/%.cpp
	@mkdir -p $(TARGET_DIR)
	$(TARGET_CXX) $(CXXFLAGS) $(RT_SECTIONS) -MMD -MP -c $< -o $@

$(TARGET_DIR)/%.o: rt/%.S
	@mkdir -p $(TARGET_DIR)
	$(TARGET_CXX) -c $< -o $@

-include $(TARGET_OBJ:.o=.d)
endif

-include $(OBJ:.o=.d) $(RTOBJ:.o=.d)

test: $(BIN) $(LSP) $(RT) $(RTFLAGS)
	@./tests/run.sh
	@./tests/lsp.sh
	@./tests/std.sh
	@./tests/indent.sh
	@./tests/grammar.sh
	@./tests/modules.sh

# Installed layout: binaries in bin/, the runtime archive in lib/sword and the
# standard library in share/sword, which is where the compiler looks for them.
# Editor support goes beside it, without what npm and the VS Code extension's test
# download into the tree: the second is a whole copy of VS Code.
install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/lib/sword
	install -d $(DESTDIR)$(PREFIX)/share/sword/std
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -m 755 $(LSP) $(DESTDIR)$(PREFIX)/bin/$(LSP)
	install -m 644 $(RT) $(DESTDIR)$(PREFIX)/lib/sword/$(RT)
	install -m 644 $(RTFLAGS) $(DESTDIR)$(PREFIX)/lib/sword/$(RTFLAGS)
	for f in libsword_rt-*.a libsword_rt-*.flags; do \
	    [ -e "$$f" ] && install -m 644 "$$f" $(DESTDIR)$(PREFIX)/lib/sword/; \
	done; true
	cp -R std/. $(DESTDIR)$(PREFIX)/share/sword/std/
	tar -cf - --exclude node_modules --exclude .vscode-test --exclude '*.vsix' editors | \
	    tar -xf - -C $(DESTDIR)$(PREFIX)/share/sword
	@echo
	@echo "Installed to $(DESTDIR)$(PREFIX)."
	@echo "If 'shield' is not found, add $(PREFIX)/bin to your PATH."

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN) $(DESTDIR)$(PREFIX)/bin/$(LSP)
	rm -rf $(DESTDIR)$(PREFIX)/lib/sword $(DESTDIR)$(PREFIX)/share/sword

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(RTOBJ) $(RTOBJ:.o=.d) $(BIN) $(LSP) $(RT) \
	      $(RTFLAGS) rt/.tls-on rt/.tls-off libsword_rt-*.a libsword_rt-*.flags \
	      src/std_errors.inc
	find rt -mindepth 1 -maxdepth 1 -type d -exec rm -rf {} +

.PHONY: all test install uninstall clean FORCE

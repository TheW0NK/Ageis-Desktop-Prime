CC      := gcc
AS      := nasm
LD      := ld
AR      := ar

BUILD   := build

CFLAGS  := -std=c11 -O2 -Wall -Wextra -ffreestanding -nostdinc \
           -isystem $(shell $(CC) -print-file-name=include) \
           -Ilib/include -I../kernel/head \
           -fpie -fno-stack-protector -fno-asynchronous-unwind-tables \
           -MMD -MP
ASFLAGS := -f elf64
LDFLAGS := -nostdlib -static --no-relax -z max-page-size=0x1000 -z noexecstack -T lib/user.ld

LIB_SRCS := $(wildcard lib/*.c)
LIB_OBJS := $(addprefix $(BUILD)/,$(LIB_SRCS:.c=.o))
CRT0     := $(BUILD)/lib/crt0.o
LIBC     := $(BUILD)/libaegis.a

# BearSSL, built for Aegis: no OS randomness or clock hooks (the TLS library
# supplies entropy from /dev/urandom and the time itself) and no x86
# intrinsics.
SSL_DIR   := ../third_party/bearssl
SSL_SRCS  := $(shell find $(SSL_DIR)/src -name '*.c')
SSL_OBJS  := $(patsubst $(SSL_DIR)/src/%.c,$(BUILD)/bearssl/%.o,$(SSL_SRCS))
SSL_LIB   := $(BUILD)/libbearssl.a
SSL_FLAGS := -std=c11 -O2 -ffreestanding -nostdinc -isystem $(shell $(CC) -print-file-name=include) \
             -Ilib/include/compat -Ilib/include -I../kernel/head -I$(SSL_DIR)/inc -I$(SSL_DIR)/src \
             -fpie -fno-stack-protector -fno-asynchronous-unwind-tables -w \
             -DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_RAND=0 -DBR_USE_WIN32_TIME=0 \
             -DBR_RDRAND=0 -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_POWER8=0 -DBR_64=1 -DBR_LOMUL=1 -DAEGIS_NO_MINMAX
CFLAGS    += -I$(SSL_DIR)/inc -I../third_party/musl-math/include -I../third_party/stb -I../third_party/minimp3

# The math library from musl.
MATH_DIR   := ../third_party/musl-math
MATH_SRCS  := $(wildcard $(MATH_DIR)/src/*.c)
MATH_OBJS  := $(patsubst $(MATH_DIR)/src/%.c,$(BUILD)/libm/%.o,$(MATH_SRCS))
MATH_LIB   := $(BUILD)/libm.a
MATH_FLAGS := -std=c99 -O2 -ffreestanding -nostdinc -isystem $(shell $(CC) -print-file-name=include) \
              -I$(MATH_DIR)/include -I$(MATH_DIR)/src -fpie -fno-stack-protector \
              -fno-asynchronous-unwind-tables -w

# Graphics: drawing, fonts (stb_truetype) and images (stb_image).
GFX_SRCS  := $(wildcard gfx/*.c ui/*.c script/*.c sound/*.c)
GFX_OBJS  := $(addprefix $(BUILD)/,$(GFX_SRCS:.c=.o))
GFX_LIB   := $(BUILD)/libgfx.a
LIBS      := $(LIBC) $(GFX_LIB) $(SSL_LIB) $(MATH_LIB)

TERMINAL_SRCS := $(wildcard terminal/*.c terminal/commands/*.c)
PROGRAMS      := $(basename $(notdir $(wildcard programs/*.c)))

COMPOSITOR_SRCS := $(wildcard compositor/*.c)

# Graphical programs, one directory each: apps/<name> -> /bin/<name> and
# system/<name> -> /sbin/<name>. Their other files (AUI, .app entries) are
# installed by tools/mkrootfs.sh.
APPS   := $(notdir $(wildcard apps/*))
SYSTEM := $(notdir $(wildcard system/*))

TARGETS := $(BUILD)/sbin/init $(BUILD)/bin/terminal $(addprefix $(BUILD)/bin/,$(PROGRAMS)) \
           $(BUILD)/sbin/compositor $(addprefix $(BUILD)/bin/,$(APPS)) $(addprefix $(BUILD)/sbin/,$(SYSTEM))

.PHONY: all clean

all: $(TARGETS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/bearssl/%.o: $(SSL_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SSL_FLAGS) -c $< -o $@

$(SSL_LIB): $(SSL_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/script/%.o: script/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ui/%.o: ui/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/gfx/%.o: gfx/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ilib/include/compat -Wno-sign-compare -Wno-unused-function -Wno-unused-parameter \
		-Wno-implicit-fallthrough -Wno-missing-field-initializers -c $< -o $@

$(BUILD)/sound/%.o: sound/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ilib/include/compat -w -c $< -o $@

$(GFX_LIB): $(GFX_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/libm/%.o: $(MATH_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MATH_FLAGS) -c $< -o $@

$(MATH_LIB): $(MATH_OBJS)
	$(AR) rcs $@ $^

$(CRT0): lib/crt0.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(LIBC): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/sbin/init: $(CRT0) $(BUILD)/init/main.o $(LIBC) $(SSL_LIB)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(CRT0) $(BUILD)/init/main.o --start-group $(LIBC) $(SSL_LIB) --end-group -o $@

$(BUILD)/bin/terminal: $(CRT0) $(addprefix $(BUILD)/,$(TERMINAL_SRCS:.c=.o)) $(LIBC) $(SSL_LIB)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(CRT0) $(addprefix $(BUILD)/,$(TERMINAL_SRCS:.c=.o)) --start-group $(LIBC) $(SSL_LIB) --end-group -o $@

$(BUILD)/sbin/compositor: $(CRT0) $(addprefix $(BUILD)/,$(COMPOSITOR_SRCS:.c=.o)) $(LIBS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(CRT0) $(addprefix $(BUILD)/,$(COMPOSITOR_SRCS:.c=.o)) --start-group $(LIBS) --end-group -o $@

define program_rule
$(BUILD)/$(2)/$(1): $(CRT0) $(patsubst %.c,$(BUILD)/%.o,$(wildcard $(3)/$(1)/*.c)) $(LIBS)
	@mkdir -p $$(dir $$@)
	$$(LD) $$(LDFLAGS) $(CRT0) $(patsubst %.c,$(BUILD)/%.o,$(wildcard $(3)/$(1)/*.c)) --start-group $$(LIBS) --end-group -o $$@
endef
$(foreach a,$(APPS),$(eval $(call program_rule,$(a),bin,apps)))
$(foreach a,$(SYSTEM),$(eval $(call program_rule,$(a),sbin,system)))

$(BUILD)/bin/%: $(CRT0) $(BUILD)/programs/%.o $(LIBS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(CRT0) $(BUILD)/programs/$*.o --start-group $(LIBS) --end-group -o $@

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

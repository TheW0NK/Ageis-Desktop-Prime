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
CFLAGS    += -I$(SSL_DIR)/inc

TERMINAL_SRCS := $(wildcard terminal/*.c terminal/commands/*.c)
PROGRAMS      := $(basename $(notdir $(wildcard programs/*.c)))

TARGETS := $(BUILD)/sbin/init $(BUILD)/bin/terminal $(addprefix $(BUILD)/bin/,$(PROGRAMS))

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

$(CRT0): lib/crt0.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(LIBC): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/sbin/init: $(CRT0) $(BUILD)/init/main.o $(LIBC)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $^ -o $@

$(BUILD)/bin/terminal: $(CRT0) $(addprefix $(BUILD)/,$(TERMINAL_SRCS:.c=.o)) $(LIBC)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $^ -o $@

$(BUILD)/bin/%: $(CRT0) $(BUILD)/programs/%.o $(LIBC) $(SSL_LIB)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(CRT0) $(BUILD)/programs/$*.o --start-group $(LIBC) $(SSL_LIB) --end-group -o $@

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

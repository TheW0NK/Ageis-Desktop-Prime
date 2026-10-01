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
LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T lib/user.ld

LIB_SRCS := $(wildcard lib/*.c)
LIB_OBJS := $(addprefix $(BUILD)/,$(LIB_SRCS:.c=.o))
CRT0     := $(BUILD)/lib/crt0.o
LIBC     := $(BUILD)/libaegis.a

TERMINAL_SRCS := $(wildcard terminal/*.c terminal/commands/*.c)
PROGRAMS      := $(basename $(notdir $(wildcard programs/*.c)))

TARGETS := $(BUILD)/sbin/init $(BUILD)/bin/terminal $(addprefix $(BUILD)/bin/,$(PROGRAMS))

.PHONY: all clean

all: $(TARGETS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

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

$(BUILD)/bin/%: $(CRT0) $(BUILD)/programs/%.o $(LIBC)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

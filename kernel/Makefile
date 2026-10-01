CC      := gcc
AS      := nasm
LD      := ld

BUILD   := build

CFLAGS  := -std=c11 -O2 -Wall -Wextra -ffreestanding -nostdinc \
           -isystem $(shell $(CC) -print-file-name=include) \
           -Ihead -I../boot/btloader \
           -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
           -fno-omit-frame-pointer -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -MMD -MP
ASFLAGS := -f elf64
LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T linker.ld

SRCS    := $(wildcard *.c lib/*.c display/*.c drivers/*.c fs/*.c)
ASMS    := $(wildcard *.asm)
OBJS    := $(addprefix $(BUILD)/,$(SRCS:.c=.o) $(ASMS:.asm=.o))

.PHONY: all clean

all: $(BUILD)/kernel.elf

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)

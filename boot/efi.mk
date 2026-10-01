# Shared rules for building a UEFI application with gnu-efi.
# Set NAME and SRCS, then include this file.

CC      := gcc
LD      := ld
OBJCOPY := objcopy

EFI_INC := /usr/include/efi
EFI_LIB := /usr/lib
BCD_DIR := $(dir $(lastword $(MAKEFILE_LIST)))bcd

BUILD   := build

CFLAGS  := -I$(EFI_INC) -I$(EFI_INC)/x86_64 -I$(BCD_DIR) -std=c11 -O2 \
           -Wall -Wextra -ffreestanding -fpic -fshort-wchar \
           -fno-stack-protector -fno-stack-check -mno-red-zone \
           -maccumulate-outgoing-args -MMD -MP
LDFLAGS := -nostdlib -shared -Bsymbolic -znocombreloc \
           -T $(EFI_LIB)/elf_x86_64_efi.lds -L $(EFI_LIB)
LIBS    := -lgnuefi -lefi

EFI_SECTIONS := -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym \
                -j .rel -j .rela -j '.rel.*' -j '.rela.*' -j .reloc

OBJS    := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))

vpath %.c $(sort $(dir $(SRCS)))

.PHONY: all clean

all: $(BUILD)/$(NAME).efi

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(NAME).so: $(OBJS)
	$(LD) $(LDFLAGS) $(EFI_LIB)/crt0-efi-x86_64.o $^ -o $@ $(LIBS)

$(BUILD)/$(NAME).efi: $(BUILD)/$(NAME).so
	$(OBJCOPY) $(EFI_SECTIONS) --target efi-app-x86_64 --subsystem=10 $< $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)

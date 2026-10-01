# Builds btmngr and btloader and assembles a test ESP in build/esp.
#
#   make                         build both
#   make run                     boot the ESP in QEMU
#   make run KERNEL=path/to.elf  ...with a kernel at \EFI\Aegis\kernel.elf

OVMF_CODE := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS := /usr/share/OVMF/OVMF_VARS_4M.fd
ESP     := build/esp
KERNEL  ?=

.PHONY: all btmngr btloader esp run clean

all: btmngr btloader

btmngr btloader:
	$(MAKE) -C $@

esp: all
	mkdir -p $(ESP)/EFI/BOOT $(ESP)/EFI/Aegis
	cp btmngr/build/btmngr.efi $(ESP)/EFI/BOOT/BOOTX64.EFI
	cp btloader/build/btloader.efi $(ESP)/EFI/Aegis/btloader.efi
	cp bcd/default.bcd $(ESP)/EFI/Aegis/bcd
	$(if $(KERNEL),cp $(KERNEL) $(ESP)/EFI/Aegis/kernel.elf)

build/ovmf_vars.fd:
	mkdir -p build
	cp $(OVMF_VARS) $@

run: esp build/ovmf_vars.fd
	qemu-system-x86_64 -machine q35 -m 256 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=build/ovmf_vars.fd \
		-drive format=raw,file=fat:rw:$(ESP) -net none -serial stdio

clean:
	$(MAKE) -C btmngr clean
	$(MAKE) -C btloader clean
	rm -rf build

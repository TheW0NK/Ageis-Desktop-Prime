# make              build the boot code, kernel and endpoint
# make image        build a bootable GPT disk image in build/aegis.img
# make run          boot the image in QEMU (q35, AHCI disk)
#
# Image options: AEGIS_USER=name AEGIS_PASSWORD=secret (defaults: user / aegis)

AEGIS_USER     ?= user
AEGIS_PASSWORD ?= aegis
IMAGE    := build/aegis.img
OVMF_CODE := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS := /usr/share/OVMF/OVMF_VARS_4M.fd

.PHONY: all boot kernel endpoint image run clean

all: boot kernel endpoint

boot kernel endpoint:
	$(MAKE) -C $@

image: all
	$(MAKE) -C boot esp KERNEL=$(abspath kernel/build/kernel.elf)
	mkdir -p build
	tools/mkrootfs.sh $(IMAGE) boot/build/esp endpoint/build $(AEGIS_USER) $(AEGIS_PASSWORD)

build/ovmf_vars.fd:
	mkdir -p build
	cp $(OVMF_VARS) $@

run: image build/ovmf_vars.fd
	qemu-system-x86_64 -machine q35 -m 512 -smp 2 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=build/ovmf_vars.fd \
		-drive format=raw,file=$(IMAGE) -net none -serial stdio

clean:
	$(MAKE) -C boot clean
	$(MAKE) -C kernel clean
	$(MAKE) -C endpoint clean
	rm -rf build

# make              build the boot code, kernel and endpoint
# make image        build a bootable GPT disk image in build/aegis.img
# make run          boot the image in QEMU (q35, AHCI disk) with the devices below
# make run-serial   the same, without a window (serial console on the terminal)
# make test         boot the image and run a scripted smoke test
#
# Image options: AEGIS_USER=name AEGIS_PASSWORD=secret (defaults: user / aegis)
#
# QEMU device options:
#   QEMU_INPUT=tablet   USB keyboard + absolute USB tablet (default; no mouse grab)
#              usb      USB keyboard + relative USB mouse
#              ps2      PS/2 keyboard and mouse only
#   QEMU_NET=virtio     virtio-net with user-mode networking (default)
#            e1000 | e1000e | none
#   QEMU_AUDIO=hda      Intel HD Audio codec with output and input (default)
#              ac97 | none
#   QEMU_AUDIODEV=pa    host audio backend: pa, pipewire, alsa, sdl, coreaudio, dsound,
#                       wav (writes build/audio.wav), none
#   QEMU_CAMERA=VID:PID pass a real USB webcam through (QEMU has no virtual camera)
#   QEMU_EXTRA=...      any other QEMU arguments

AEGIS_USER     ?= user
AEGIS_PASSWORD ?= aegis
IMAGE    := build/aegis.img
OVMF_CODE := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS := /usr/share/OVMF/OVMF_VARS_4M.fd

.PHONY: all boot kernel endpoint image run run-serial test clean

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

QEMU_INPUT    ?= tablet
QEMU_NET      ?= virtio
QEMU_AUDIO    ?= hda
QEMU_AUDIODEV ?= pa
QEMU_CAMERA   ?=
QEMU_EXTRA    ?=

QEMU_DEVICES := -device qemu-xhci,id=xhci
ifeq ($(QEMU_INPUT),tablet)
QEMU_DEVICES += -device usb-kbd -device usb-tablet
else ifeq ($(QEMU_INPUT),usb)
QEMU_DEVICES += -device usb-kbd -device usb-mouse
endif
ifeq ($(QEMU_NET),none)
QEMU_DEVICES += -nic none
else ifeq ($(QEMU_NET),virtio)
QEMU_DEVICES += -nic user,model=virtio-net-pci
else
QEMU_DEVICES += -nic user,model=$(QEMU_NET)
endif
ifneq ($(QEMU_AUDIO),none)
ifeq ($(QEMU_AUDIODEV),wav)
QEMU_DEVICES += -audiodev wav,id=snd0,path=build/audio.wav
else
QEMU_DEVICES += -audiodev $(QEMU_AUDIODEV),id=snd0
endif
ifeq ($(QEMU_AUDIO),hda)
QEMU_DEVICES += -device intel-hda -device hda-duplex,audiodev=snd0
else ifeq ($(QEMU_AUDIO),ac97)
QEMU_DEVICES += -device AC97,audiodev=snd0
endif
endif
ifneq ($(QEMU_CAMERA),)
QEMU_DEVICES += -device usb-host,vendorid=0x$(word 1,$(subst :, ,$(QEMU_CAMERA))),productid=0x$(word 2,$(subst :, ,$(QEMU_CAMERA)))
endif

QEMU := qemu-system-x86_64 -machine q35 -m 1024 -smp 2 \
	-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	-drive if=pflash,format=raw,file=build/ovmf_vars.fd \
	-drive format=raw,file=$(IMAGE) $(QEMU_DEVICES) $(QEMU_EXTRA)

run: image build/ovmf_vars.fd
	$(QEMU) -serial stdio

run-serial: image build/ovmf_vars.fd
	$(QEMU) -serial stdio -display none

test: image
	tools/qemu-test.py @reject:FAIL @reject:PANIC @reject:crashed @login:$(AEGIS_USER):$(AEGIS_PASSWORD) \
		'ls /dev' @expect:input 'uname' @expect:Aegis 'echo piped | cat' @expect:piped \
		'apprun /usr/share/apprun/selftest.as' @expect:'selftest: ok' \
		'browser --check /usr/share/browser/welcome.html' @expect:'browser: ok' \
		'cred set test-secret s3cr3t-value' 'cred get test-secret' @expect:'s3cr3t-value'
	tools/qemu-test.py --cpus 4 --timeout 180 @reject:FAIL @reject:PANIC \
		@login:$(AEGIS_USER):$(AEGIS_PASSWORD) 'ktest' @expect:'ktest: all passed'
	tools/qemu-test.py --usb --tablet @reject:PANIC @login:$(AEGIS_USER):$(AEGIS_PASSWORD) \
		'whoami' @expect:$(AEGIS_USER)
	tools/qemu-test.py --usb --tablet --timeout 120 @reject:PANIC @expect:'greeter: ready' @sleep:1 \
		@type:$(AEGIS_PASSWORD) @key:ret @expect:'greeter: signed in $(AEGIS_USER)'
	tools/qemu-test.py --net --qemu-arg=-nic --qemu-arg=user,model=virtio-net-pci @reject:PANIC \
		@login:$(AEGIS_USER):$(AEGIS_PASSWORD) @sleep:3 'ifconfig eth0' @expect:10.0.2.15 \
		'ping -c 1 10.0.2.2' @expect:'1 received' 'netbench 8' @expect:'tcp loopback: 8388608'

clean:
	$(MAKE) -C boot clean
	$(MAKE) -C kernel clean
	$(MAKE) -C endpoint clean
	rm -rf build

# LucyK — x86_64 Microkernel
# Phase 1 build system: freestanding kernel ELF + Limine bootable ISO.

CC       := clang
LD       := ld.lld
NASM     ?= nasm
XORRISO  := xorriso
QEMU     := qemu-system-x86_64

LIMINE_BRANCH := v8.x-binary
LIMINE_REPO   := https://github.com/limine-bootloader/limine.git

BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
ISO_ROOT  := $(BUILD_DIR)/iso_root
LIMINE_DIR:= $(BUILD_DIR)/limine
KERNEL    := $(BUILD_DIR)/lucyk.elf
ISO       := $(BUILD_DIR)/os.iso
DISK      := $(BUILD_DIR)/disk.img
DISK_MB   := 32

# Freestanding compiler headers (stdint.h, stddef.h, stdarg.h, ...).
CLANG_INC := $(shell $(CC) -print-resource-dir)/include

CFLAGS := -std=c11 -ffreestanding -nostdlib -nostdinc -isystem $(CLANG_INC) \
          -Wall -Wextra -Werror -O2 -g -MMD -MP \
          -fno-stack-protector -fno-builtin -fno-pic -fno-pie \
          -mcmodel=kernel -mno-red-zone \
          -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
          -I src/include -I $(LIMINE_DIR)

LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -T linker.ld --build-id=none

ASFLAGS := -f elf64 -g -F dwarf

AP_TRAMPOLINE_BIN := $(BUILD_DIR)/ap_trampoline.bin

USER_CC      := clang
USER_LD      := ld.lld
USER_OBJ     := $(BUILD_DIR)/user
USER_CFLAGS  := -std=c11 -ffreestanding -nostdlib -nostdinc -isystem $(CLANG_INC) \
                -Wall -Wextra -Werror -O2 -g \
                -fno-stack-protector -fno-builtin -fno-pic -fno-pie \
                -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
                -I src/include -I user
USER_LDFLAGS := -nostdlib -static -z max-page-size=0x1000 \
                -T user/linker.ld --build-id=none
USER_INIT    := $(BUILD_DIR)/init_test.elf
USER_FAULT   := $(BUILD_DIR)/fault_test.elf
USER_BSD_INIT := $(BUILD_DIR)/bsd_init.elf
USER_BSD_SH   := $(BUILD_DIR)/bsd_sh.elf
USER_BSD_ECHO := $(BUILD_DIR)/bsd_echo.elf
USER_BSD_HDR  := user/bsd/bsd_user.h
USER_BSD_LIB  := $(USER_OBJ)/bsd/lib.o
USER_COMMON  := $(USER_OBJ)/start.o $(USER_OBJ)/runtime.o

INITRD_ROOT := $(BUILD_DIR)/initrd_root
INITRD_TAR  := $(BUILD_DIR)/initrd.tar
EXT2_ROOT   := $(BUILD_DIR)/ext2_root

C_SRCS   := $(shell find src -name '*.c' | sort)
# Stage D1 I/O sources (src/io/pci.c, src/io/virtio_blk.c and
# src/arch/x86_64/ioapic.c) are registered by this recursive discovery.
ASM_SRCS := $(shell find src -name '*.asm' | sort | grep -v ap_trampoline.asm)
C_OBJS   := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(C_SRCS))
ASM_OBJS := $(patsubst src/%.asm,$(OBJ_DIR)/%.asm.o,$(ASM_SRCS))
OBJS     := $(C_OBJS) $(ASM_OBJS)

.PHONY: all clean run run-uefi run-smp1 run-smp4 verify limine

all: $(ISO) $(DISK)

# Writable ext2 root volume. The staging tree is populated without root by
# mke2fs's -d option, then the whole 32 MiB image is formatted in place.
$(DISK): $(USER_BSD_INIT) $(USER_BSD_SH) $(USER_BSD_ECHO) Makefile
	@rm -rf $(EXT2_ROOT)
	@mkdir -p $(EXT2_ROOT)/bin $(EXT2_ROOT)/etc $(EXT2_ROOT)/tmp
	@cp -p $(USER_BSD_INIT) $(EXT2_ROOT)/bin/init
	@cp -p $(USER_BSD_SH) $(EXT2_ROOT)/bin/sh
	@cp -p $(USER_BSD_ECHO) $(EXT2_ROOT)/bin/echo
	@chmod 0755 $(EXT2_ROOT)/bin/init $(EXT2_ROOT)/bin/sh \
		$(EXT2_ROOT)/bin/echo
	@chmod 0777 $(EXT2_ROOT)/tmp
	@printf '%s\n' \
		'LucyK ext2 root filesystem (Kernel Stage D2)' \
		'This /etc/motd lives on the writable ext2 volume.' \
		'Stage D2 verifies indirect blocks, mkdir, unlink and rmdir.' \
		'' > $(EXT2_ROOT)/etc/motd
	@rm -f $@
	@mkfs.ext2 -q -F -b 4096 -O ^dir_index -d $(EXT2_ROOT) $@ \
		$(DISK_MB)M
	@echo "[ext2] formatted $@ ($(DISK_MB) MiB)"

$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# The AP bootstrap is a flat binary, not part of the linked kernel: it runs in
# real mode at physical 0x8000 before paging exists.
$(AP_TRAMPOLINE_BIN): src/arch/x86_64/ap_trampoline.asm
	@mkdir -p $(dir $@)
	$(NASM) -f bin $< -o $@

$(OBJ_DIR)/arch/x86_64/ap_image.asm.o: src/arch/x86_64/ap_image.asm $(AP_TRAMPOLINE_BIN)
	@mkdir -p $(dir $@)
	$(NASM) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/%.asm.o: src/%.asm $(AP_TRAMPOLINE_BIN)
	@mkdir -p $(dir $@)
	$(NASM) $(ASFLAGS) $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Fetch the official Limine binary release branch on demand.
$(LIMINE_DIR)/limine-bios.sys:
	@mkdir -p $(BUILD_DIR)
	git clone --branch=$(LIMINE_BRANCH) --depth=1 $(LIMINE_REPO) $(LIMINE_DIR)

# Any translation unit may include limine.h; ensure the clone exists first.
$(OBJS): | $(LIMINE_DIR)/limine-bios.sys

$(USER_OBJ)/%.o: user/%.c user/user_lib.h src/include/syscall_numbers.h
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_OBJ)/start.o: user/start.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(USER_INIT): $(USER_COMMON) $(USER_OBJ)/init_test.o user/linker.ld
	$(USER_LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON) $(USER_OBJ)/init_test.o

$(USER_FAULT): $(USER_COMMON) $(USER_OBJ)/fault_test.o user/linker.ld
	$(USER_LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON) $(USER_OBJ)/fault_test.o

$(USER_OBJ)/bsd/lib.o: user/bsd/lib.c $(USER_BSD_HDR)
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_OBJ)/bsd/init.o: user/bsd/init.c $(USER_BSD_HDR) user/linker.ld
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_OBJ)/bsd/sh.o: user/bsd/sh.c $(USER_BSD_HDR) user/linker.ld
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_OBJ)/bsd/echo.o: user/bsd/echo.c $(USER_BSD_HDR) user/linker.ld
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BSD_INIT): $(USER_OBJ)/start.o $(USER_OBJ)/bsd/init.o $(USER_BSD_LIB) \
                  user/linker.ld
	$(USER_LD) $(USER_LDFLAGS) -o $@ $(USER_OBJ)/start.o \
		$(USER_OBJ)/bsd/init.o $(USER_BSD_LIB)

$(USER_BSD_SH): $(USER_OBJ)/start.o $(USER_OBJ)/bsd/sh.o $(USER_BSD_LIB) \
                user/linker.ld
	$(USER_LD) $(USER_LDFLAGS) -o $@ $(USER_OBJ)/start.o \
		$(USER_OBJ)/bsd/sh.o $(USER_BSD_LIB)

$(USER_BSD_ECHO): $(USER_OBJ)/start.o $(USER_OBJ)/bsd/echo.o $(USER_BSD_LIB) \
                  user/linker.ld
	$(USER_LD) $(USER_LDFLAGS) -o $@ $(USER_OBJ)/start.o \
		$(USER_OBJ)/bsd/echo.o $(USER_BSD_LIB)

# Read-only USTAR root filesystem: /etc/motd, /bin/init, /bin/sh, /bin/echo.
$(INITRD_TAR): $(USER_BSD_INIT) $(USER_BSD_SH) $(USER_BSD_ECHO) Makefile
	@rm -rf $(INITRD_ROOT)
	@mkdir -p $(INITRD_ROOT)/etc $(INITRD_ROOT)/bin
	@printf '%s\n' \
		'LucyK BSD Personality -- Kernel Stage C' \
		'initrd.tar is a read-only USTAR filesystem mounted at /.' \
		'PID 1 (/bin/init) ran the fork/exec/wait and Stage C test suites.' \
		'Signals, process groups, TLS, mprotect verified.' \
		'Try: ls, cat /etc/motd, /bin/echo hello, exit' \
		> $(INITRD_ROOT)/etc/motd
	cp $(USER_BSD_INIT) $(INITRD_ROOT)/bin/init
	cp $(USER_BSD_SH) $(INITRD_ROOT)/bin/sh
	cp $(USER_BSD_ECHO) $(INITRD_ROOT)/bin/echo
	tar --format=ustar -cf $@ -C $(INITRD_ROOT) etc bin

$(ISO): $(KERNEL) limine.conf $(LIMINE_DIR)/limine-bios.sys \
        $(USER_INIT) $(USER_FAULT) $(INITRD_TAR)
	@rm -rf $(ISO_ROOT)
	@mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL) $(ISO_ROOT)/boot/lucyk.elf
	cp $(USER_INIT) $(ISO_ROOT)/boot/init_test.elf
	cp $(USER_FAULT) $(ISO_ROOT)/boot/fault_test.elf
	cp $(INITRD_TAR) $(ISO_ROOT)/boot/initrd.tar
	cp limine.conf $(ISO_ROOT)/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/limine-bios-cd.bin $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/BOOTX64.EFI
	xorriso -as mkisofs \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_ROOT) -o $(ISO)

run: $(ISO) $(DISK)
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -serial stdio -no-reboot -no-shutdown \
		-drive file=$(DISK),if=none,id=hd0,format=raw \
		-device virtio-blk-pci,drive=hd0

run-smp1: $(ISO) $(DISK)
	$(QEMU) -M q35 -m 512M -smp 1 -cdrom $(ISO) -serial stdio \
		-no-reboot -no-shutdown \
		-drive file=$(DISK),if=none,id=hd0,format=raw \
		-device virtio-blk-pci,drive=hd0

run-smp4: $(ISO) $(DISK)
	$(QEMU) -M q35 -m 512M -cpu max -smp 4 -cdrom $(ISO) -serial stdio \
		-no-reboot -no-shutdown \
		-drive file=$(DISK),if=none,id=hd0,format=raw \
		-device virtio-blk-pci,drive=hd0

run-uefi: $(ISO) $(DISK)
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -serial stdio -no-reboot -no-shutdown \
		-bios /usr/share/OVMF/OVMF_CODE.fd \
		-drive file=$(DISK),if=none,id=hd0,format=raw \
		-device virtio-blk-pci,drive=hd0

clean:
	rm -rf $(BUILD_DIR)

# Full local acceptance gate: clean build, architectural boundary/hygiene scan,
# headless QEMU acceptance boots (-smp 1 and -cpu max -smp 4), and ext2
# filesystem integrity. See scripts/verify.sh.
verify:
	./scripts/verify.sh

# Header dependencies generated by -MMD.
-include $(C_OBJS:.o=.d)

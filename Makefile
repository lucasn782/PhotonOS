CC := gcc
LD := ld
OBJCOPY := objcopy
NASM := nasm

BOOT_BIN := build/boot/boot.bin
KERNEL_ELF := build/photon.elf
KERNEL_BIN := build/photon.bin
KERNEL_MAP := build/photon.map
USER_SHELL_ELF := build/user/shell.elf
USER_SHELL_OBJ := build/user/shell_blob.o
USER_HELLO_ELF := build/user/hello.elf
USER_HELLO_OBJ := build/user/hello_blob.o
USER_UPPER_ELF := build/user/upper.elf
USER_UPPER_OBJ := build/user/upper_blob.o
USER_REV_ELF := build/user/rev.elf
USER_REV_OBJ := build/user/rev_blob.o
USER_HANG_ELF := build/user/hang.elf
USER_HANG_OBJ := build/user/hang_blob.o
USER_SPIN_ELF := build/user/spin.elf
USER_SPIN_OBJ := build/user/spin_blob.o
USER_PING_ELF := build/user/ping.elf
USER_PING_OBJ := build/user/ping_blob.o
IMG := build/photon.img
DISK_IMG := build/disk.img
FLOPPY_BYTES := 1474560
KERNEL_SECTORS := 352
KERNEL_MAX_BYTES := $(shell expr $(KERNEL_SECTORS) \* 512)
KERNEL_OBJS := build/boot/kernel_asm.o build/user/shell_blob.o build/user/hello_blob.o build/user/upper_blob.o \
               build/user/rev_blob.o build/user/hang_blob.o build/user/spin_blob.o build/user/ping_blob.o \
               build/kernel/kernel.o build/kernel/memory.o build/kernel/vmm.o \
               build/drivers/serial.o build/drivers/video.o build/drivers/mouse.o build/kernel/scheduler.o build/kernel/mutex.o build/kernel/heap.o \
               build/kernel/vfs.o build/kernel/initrd.o build/kernel/elf.o build/kernel/net.o build/kernel/tcp.o build/drivers/fat16.o \
               build/drivers/ata.o build/drivers/pci.o build/drivers/e1000.o \
               build/kernel/apic.o build/kernel/smp.o build/kernel/trampoline_blob.o \
               build/fs/ext2.o

CFLAGS := -ffreestanding -m64 -nostdlib -mno-red-zone -fno-pic -fno-pie \
          -fstack-protector-strong -Wall -Wextra -Iinclude
USER_CFLAGS := $(CFLAGS) -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables \
               -mcmodel=large
LDFLAGS := -nostdlib -z max-page-size=0x1000 -T linker.ld -Map=build/photon.map
USER_LDFLAGS := -nostdlib -s -N --no-warn-rwx-segments -z max-page-size=0x1000 \
               -Ttext=0x8000001000 -e _start

.PHONY: all clean run run-fat16 fat16-disk background-test test-net release

all: directories $(IMG)

directories:
	@mkdir -p build/boot build/kernel build/drivers build/user build/fs

$(BOOT_BIN): src/boot/boot.asm
	@mkdir -p $(dir $@) && $(NASM) -f bin $< -o $@

build/boot/kernel_asm.o: src/boot/kernel.asm
	@mkdir -p $(dir $@) && $(NASM) -f elf64 $< -o $@

build/kernel/trampoline.bin: src/kernel/trampoline.asm
	@mkdir -p $(dir $@) && $(NASM) -f bin $< -o $@

build/kernel/trampoline_blob.o: build/kernel/trampoline.bin
	cd build/kernel && $(LD) -r -b binary trampoline.bin -o trampoline_blob.o

build/kernel/apic.o: src/kernel/apic.c include/apic.h include/vmm.h include/memory.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/kernel/smp.o: src/kernel/smp.c include/smp.h include/apic.h include/memory.h include/serial.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/kernel/kernel.o: src/kernel/kernel.c include/*.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/kernel/memory.o: src/kernel/memory.c include/memory.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/kernel/vmm.o: src/kernel/vmm.c include/vmm.h include/memory.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/drivers/serial.o: src/drivers/serial.c include/serial.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/drivers/video.o: src/drivers/video.c include/video.h include/font_8x16.h include/vmm.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/drivers/mouse.o: src/drivers/mouse.c include/mouse.h include/video.h include/serial.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/kernel/scheduler.o: src/kernel/scheduler.c include/scheduler.h include/mutex.h include/task.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/kernel/mutex.o: src/kernel/mutex.c include/mutex.h include/scheduler.h include/task.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/kernel/heap.o: src/kernel/heap.c include/heap.h include/memory.h include/vmm.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/kernel/vfs.o: src/kernel/vfs.c include/vfs.h include/heap.h include/mutex.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/kernel/initrd.o: src/kernel/initrd.c include/initrd.h include/vfs.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/kernel/elf.o: src/kernel/elf.c include/elf.h include/memory.h include/scheduler.h include/task.h include/vfs.h include/vmm.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/kernel/net.o: src/kernel/net.c include/net.h include/tcp.h include/e1000.h include/scheduler.h include/serial.h include/mutex.h include/vmm.h include/sys/socket.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/kernel/tcp.o: src/kernel/tcp.c include/tcp.h include/net.h include/heap.h include/mutex.h include/scheduler.h include/serial.h include/sys/socket.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/drivers/fat16.o: src/drivers/fat16.c include/fat16.h include/ata.h include/heap.h include/serial.h include/vfs.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/drivers/ata.o: src/drivers/ata.c include/ata.h include/fat16.h include/serial.h include/vfs.h include/fs/ext2.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/fs/ext2.o: src/fs/ext2.c include/fs/ext2.h include/ata.h include/heap.h include/serial.h include/vfs.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/drivers/pci.o: src/drivers/pci.c include/pci.h include/e1000.h include/serial.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/drivers/e1000.o: src/drivers/e1000.c include/e1000.h include/heap.h include/memory.h include/mutex.h include/serial.h include/vmm.h
	@mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c $< -o $@

build/user/shell.o: src/user/shell.c include/proc.h
	@mkdir -p $(dir $@) && $(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_SHELL_ELF): build/user/shell.o build/user/ulibc.o
	@mkdir -p $(dir $@) && $(LD) $(USER_LDFLAGS) $^ -o $@

$(USER_SHELL_OBJ): $(USER_SHELL_ELF)
	cd build/user && $(LD) -r -b binary $(notdir $(USER_SHELL_ELF)) -o $(notdir $(USER_SHELL_OBJ))

build/user/hello.o: src/user/hello.c
	@mkdir -p $(dir $@) && $(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_HELLO_ELF): build/user/hello.o
	@mkdir -p $(dir $@) && $(LD) $(USER_LDFLAGS) $< -o $@

$(USER_HELLO_OBJ): $(USER_HELLO_ELF)
	cd build/user && $(LD) -r -b binary $(notdir $(USER_HELLO_ELF)) -o $(notdir $(USER_HELLO_OBJ))

build/user/upper.o: src/user/upper.c
	@mkdir -p $(dir $@) && $(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_UPPER_ELF): build/user/upper.o
	@mkdir -p $(dir $@) && $(LD) $(USER_LDFLAGS) $< -o $@

$(USER_UPPER_OBJ): $(USER_UPPER_ELF)
	cd build/user && $(LD) -r -b binary $(notdir $(USER_UPPER_ELF)) -o $(notdir $(USER_UPPER_OBJ))

build/user/ulibc.o: src/user/ulibc.c include/ulibc.h include/proc.h include/sys/socket.h
	@mkdir -p $(dir $@) && $(CC) $(USER_CFLAGS) -c $< -o $@

build/user/rev.o: src/user/rev.c include/ulibc.h
	@mkdir -p $(dir $@) && $(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_REV_ELF): build/user/rev.o build/user/ulibc.o
	@mkdir -p $(dir $@) && $(LD) $(USER_LDFLAGS) $^ -o $@

$(USER_REV_OBJ): $(USER_REV_ELF)
	cd build/user && $(LD) -r -b binary $(notdir $(USER_REV_ELF)) -o $(notdir $(USER_REV_OBJ))

build/user/hang.o: src/user/hang.c include/ulibc.h
	@mkdir -p $(dir $@) && $(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_HANG_ELF): build/user/hang.o build/user/ulibc.o
	@mkdir -p $(dir $@) && $(LD) $(USER_LDFLAGS) $^ -o $@

$(USER_HANG_OBJ): $(USER_HANG_ELF)
	cd build/user && $(LD) -r -b binary $(notdir $(USER_HANG_ELF)) -o $(notdir $(USER_HANG_OBJ))

build/user/spin.o: src/user/spin.c
	@mkdir -p $(dir $@) && $(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_SPIN_ELF): build/user/spin.o
	@mkdir -p $(dir $@) && $(LD) $(USER_LDFLAGS) $< -o $@

$(USER_SPIN_OBJ): $(USER_SPIN_ELF)
	cd build/user && $(LD) -r -b binary $(notdir $(USER_SPIN_ELF)) -o $(notdir $(USER_SPIN_OBJ))

build/user/ping.o: src/user/ping.c include/ulibc.h include/sys/socket.h
	@mkdir -p $(dir $@) && $(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_PING_ELF): build/user/ping.o build/user/ulibc.o
	@mkdir -p $(dir $@) && $(LD) $(USER_LDFLAGS) $^ -o $@

$(USER_PING_OBJ): $(USER_PING_ELF)
	cd build/user && $(LD) -r -b binary $(notdir $(USER_PING_ELF)) -o $(notdir $(USER_PING_OBJ))

$(KERNEL_ELF): $(KERNEL_OBJS) linker.ld
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o $@

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
	test $$(stat -c%s $@) -le $(KERNEL_MAX_BYTES)

$(IMG): $(BOOT_BIN) $(KERNEL_BIN)
	cat $(BOOT_BIN) $(KERNEL_BIN) > $@
	truncate -s $(FLOPPY_BYTES) $@

run: $(IMG)
	qemu-system-x86_64 -smp 4 -serial stdio -drive format=raw,file=$(IMG)

fat16-disk: $(USER_SHELL_ELF) $(USER_HELLO_ELF) $(USER_UPPER_ELF) $(USER_REV_ELF) $(USER_HANG_ELF) $(USER_SPIN_ELF) $(USER_PING_ELF)
	DISK_IMG=$(DISK_IMG) USER_DIR=build/user bash scripts/create_fat16_disk.sh

run-fat16: $(IMG) fat16-disk
	qemu-system-x86_64 -smp 4 -drive format=raw,file=$(IMG),if=floppy -drive format=raw,file=build/disk.img,if=ide,index=0,media=disk -boot a -serial stdio

background-test:
	bash scripts/background_test.sh

map: $(KERNEL_MAP)
	@cat $(KERNEL_MAP)

clean:
	rm -rf build/ $(IMG)

test-net: all
	@echo "=================================================="
	@echo " Inicializando Esteira Automatizada de Rede "
	@echo "=================================================="
	sudo bash scripts/test_network.sh

release:
	@bash scripts/publish_release.sh $(VERSION)

# RISC-V OS Kernel

## Overview

A UNIX-like operating system kernel built from scratch in C and RISC-V assembly for UIUC ECE 391 (Computer Systems Engineering). The kernel runs on the RISC-V ISA and is emulated via QEMU, demonstrating full-stack systems programming from hardware-level interrupt handling up to user-space process execution.

> **Course:** ECE 391 — Computer Systems Engineering, University of Illinois Urbana-Champaign **Language:** C, RISC-V Assembly **Platform:** RISC-V (rv64), QEMU `virt` machine

## Features

- **Virtual Memory & Paging** — Sv39/Sv48 page-table setup, kernel/user address-space separation, and page-fault handling via the trap mechanism
- **Trap & Interrupt Handling** — Trap vector in RISC-V assembly routes exceptions (syscalls, page faults, illegal instructions) and hardware interrupts to the correct kernel handler
- **System Calls** — `ecall`-based syscall interface bridging user-mode programs to privileged kernel services
- **Process Management** — Process creation, context switching, and execution in user mode (U-mode) with supervisor-mode (S-mode) kernel
- **Scheduler** — Preemptive or cooperative scheduling of user processes
- **File System** — Read-only or simple writable file-system support allowing user programs to open, read, and close files
- **Device I/O** — UART console driver for serial I/O; interaction with QEMU's `virt` platform devices
- **User-Space Programs** — Sample user programs (`user/`) demonstrating the syscall ABI and runtime environment
- **Utility Library** — Shared helper routines (`util/`) used by both kernel and user code

## Repository Structure

```
riscv-os-kernel/
├── kern/        # Kernel source — boot, traps, memory, scheduler, syscalls, drivers
├── user/        # User-space programs that run on the kernel
└── util/        # Shared utility library (string ops, printf, etc.)
```

The `kern/` directory is the heart of the project. It contains the trap vector (written in RISC-V assembly), the kernel entry point, virtual-memory setup, the system-call dispatch table, process control blocks, and device drivers. `user/` holds small programs that exercise the syscall interface. `util/` supplies libc-style helpers shared between kernel and user code.

## Build & Run

### Prerequisites

- RISC-V cross-compiler toolchain: `riscv64-unknown-elf-gcc`
- QEMU with RISC-V support: `qemu-system-riscv64`

On macOS (Homebrew):

```bash
brew install riscv-gnu-toolchain qemu
```

On Ubuntu/Debian:

```bash
sudo apt install gcc-riscv64-unknown-elf qemu-system-misc
```

### Build

```bash
git clone https://github.com/yanzi222/riscv-os-kernel.git
cd riscv-os-kernel
make
```

### Run in QEMU

```bash
make qemu
```

This boots the kernel on a simulated RISC-V `virt` machine and drops to the UART console.

### Debug with GDB

```bash
make qemu-gdb        # start QEMU paused, listening on :1234
# in a second terminal:
riscv64-unknown-elf-gdb kernel.elf
(gdb) target remote :1234
(gdb) continue
```

## Technical Highlights

This project demonstrates skills that are directly relevant to systems and low-level software engineering roles:

| Skill | Where it shows up |
| --- | --- |
| **C systems programming** | Kernel data structures, process control blocks, memory allocator |
| **RISC-V ISA & assembly** | Trap vector, context-save/restore, `ecall` dispatch, CSR manipulation |
| **Virtual memory** | Page-table construction, `satp` register setup, TLB management |
| **Interrupt & exception handling** | `stvec`/`mtvec` trap handlers, timer interrupts, fault recovery |
| **Operating system internals** | Scheduler, syscall interface, privilege-level transitions (U → S mode) |
| **Hardware bring-up & drivers** | UART driver, QEMU `virt` MMIO device interaction |
| **Debugging** | GDB + QEMU remote debugging of bare-metal kernel code |
| **Build systems** | Makefile cross-compilation for a bare-metal RISC-V target |

Building this kernel required reading the RISC-V Privileged Specification, reasoning about hardware/software contracts at the ISA level, and debugging without an OS underneath — skills that transfer directly to embedded systems, hypervisor, and OS/kernel engineering roles.

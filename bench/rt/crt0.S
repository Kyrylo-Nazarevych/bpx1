# BPX-1 bench runtime: bare-metal RV32IM start-up for Spike. No OS, no CSR access, no compressed code.
#
#   _start: set gp and sp, zero .bss, call main, fall into _exit.
#   _exit:  a0 = exit code. Writes (code << 1) | 1 to the low word of the HTIF "tohost" doubleword
#           (the high word stays 0), which makes Spike stop with process status (code & 0xff).
#           Spike only polls tohost every few thousand instructions, so the program spins after
#           the store. rvtrace ends the trace at the store, which it finds through the symbol
#           __tohost_store; it is the only place in the runtime that writes tohost.

    .section .text.init, "ax", @progbits
    .globl  _start
    .type   _start, @function
_start:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, __stack_top
    la      t0, __bss_start             # .bss is padded to a multiple of 16 bytes (link.ld)
    la      t1, __bss_end
    bgeu    t0, t1, 2f
1:  sw      zero, 0(t0)
    sw      zero, 4(t0)
    sw      zero, 8(t0)
    sw      zero, 12(t0)
    addi    t0, t0, 16
    bltu    t0, t1, 1b
2:  call    main
    .size   _start, . - _start

    .globl  _exit
    .type   _exit, @function
_exit:
    slli    a0, a0, 1
    ori     a0, a0, 1
    la      t0, tohost
    .globl  __tohost_store
__tohost_store:
    sw      a0, 0(t0)
3:  j       3b
    .size   _exit, . - _exit

    .section .tohost, "aw", @progbits
    .balign 64
    .globl  tohost
tohost:   .dword 0
    .globl  fromhost
fromhost: .dword 0

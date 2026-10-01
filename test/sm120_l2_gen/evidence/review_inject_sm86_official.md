| function | instr | maxreg | BSSY | BSYNC | BRA | BREAK | BAR | LDC | STL | LDL | LD/ST.E | BPT | NOP | EXIT | RET.ABS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| jmp_to_target | 9 | 5 | 0 | 0 | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 1 | 0 | 1 |
| relocate_func_call | 6 | 21 | 0 | 0 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| trampoline | 8 | 21 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | 2 | 0 | 0 | 0 | 0 | 0 |
| check_preempt_trap | 40 | 11 | 2 | 2 | 4 | 0 | 0 | 5 | 0 | 0 | 8 | 0 | 0 | 2 | 0 |
| check_preempt | 53 | 11 | 4 | 4 | 7 | 1 | 1 | 4 | 0 | 0 | 11 | 0 | 0 | 1 | 0 |
| restore_exec | 32 | 21 | 1 | 1 | 2 | 0 | 1 | 4 | 0 | 0 | 2 | 0 | 10 | 1 | 1 |
| exit_if_idempotent | 32 | 20 | 0 | 0 | 2 | 0 | 0 | 5 | 2 | 2 | 3 | 0 | 4 | 1 | 1 |

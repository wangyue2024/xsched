	.target	sm_120

	.elftype	@"ET_EXEC"


//--------------------- .text.nop_func            --------------------------
	.section	.text.nop_func,"ax",@progbits
	.align	128
        .global         nop_func
        .type           nop_func,@function
        .size           nop_func,(.L_x_31 - nop_func)
nop_func:
.text.nop_func:
        /*0000*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*0010*/                   RET.ABS.NODEC R20 0x0                                                 ?trans5;
.L_x_2:
        /*0020*/                   BRA `(.L_x_2);
        /*0030*/                   NOP;
        /*0040*/                   NOP;
        /*0050*/                   NOP;
        /*0060*/                   NOP;
        /*0070*/                   NOP;
        /*0080*/                   NOP;
        /*0090*/                   NOP;
        /*00a0*/                   NOP;
        /*00b0*/                   NOP;
        /*00c0*/                   NOP;
        /*00d0*/                   NOP;
        /*00e0*/                   NOP;
        /*00f0*/                   NOP;
.L_x_31:


//--------------------- .text._Z3nopv             --------------------------
	.section	.text._Z3nopv,"ax",@progbits
	.align	128
        .global         _Z3nopv
        .type           _Z3nopv,@function
        .size           _Z3nopv,(.L_x_32 - _Z3nopv)
_Z3nopv:
.text._Z3nopv:
        /*0000*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*0010*/                   RET.ABS.NODEC R20 0x0                                                 ?trans5;
.L_x_3:
        /*0020*/                   BRA `(.L_x_3);
        /*0030*/                   NOP;
        /*0040*/                   NOP;
        /*0050*/                   NOP;
        /*0060*/                   NOP;
        /*0070*/                   NOP;
        /*0080*/                   NOP;
        /*0090*/                   NOP;
        /*00a0*/                   NOP;
        /*00b0*/                   NOP;
        /*00c0*/                   NOP;
        /*00d0*/                   NOP;
        /*00e0*/                   NOP;
        /*00f0*/                   NOP;
.L_x_32:


//--------------------- .text.dummy               --------------------------
	.section	.text.dummy,"ax",@progbits
	.align	128
        .global         dummy
        .type           dummy,@function
        .size           dummy,(.L_x_33 - dummy)
dummy:
.text.dummy:
        /*0000*/                   IADD3 R1, PT, PT, R1, -0x10, RZ                                       ?trans1;
        /*0010*/                   MOV R9, R5                                                            ?trans1;
        /*0020*/                   MOV R8, R4                                                            ?WAIT3_END_GROUP;
        /*0030*/                   STL [R1+0xc], R21                                                     ?trans4;
        /*0040*/                   STL [R1+0x8], R20                                  &rd=0x0            ?trans1;
        /*0050*/                   LDC R6, c[0x0][0x2f8]                              &wr=0x1            ?trans3;
        /*0060*/                   STL.64 [R1], R8                                    &rd=0x2            ?trans1;
        /*0070*/                   MOV R4, R1                                                            ?trans1;
        /*0080*/                   HFMA2 R5, -RZ, RZ, 0, 0                                               ?WAIT3_END_GROUP;
        /*0090*/                   LDC R7, c[0x0][0x2fc]                              &wr=0x1            ?trans2;
        /*00a0*/                   IADD.64 R6, R4, R6                                 &req={1}           ?trans2;
        /*00b0*/                   MOV R4, 32@lo($str)                                                   ?trans1;
        /*00c0*/                   MOV R5, 32@hi($str)                                                   ?trans1;
        /*00d0*/                   MOV R20, 32@lo((dummy + .L_x_0@srel))              &req={0}           ?trans1;
        /*00e0*/                   MOV R21, 32@hi((dummy + .L_x_0@srel))              &req={2}           ?WAIT7_END_GROUP;
        /*00f0*/                   CALL.ABS.NOINC `(vprintf)                                             ?trans5;
.L_x_0:
        /*0100*/                   LDL R20, [R1+0x8]                                  &wr=0x2            ?trans4;
        /*0110*/                   LDL R21, [R1+0xc]                                  &rd=0x0 &wr=0x2    ?trans2;
        /*0120*/                   IADD3 R1, PT, PT, R1, 0x10, RZ                     &req={0}           ?trans1;
        /*0130*/                   RET.ABS.NODEC R20 0x0                              &req={2}           ?trans6;
.L_x_4:
        /*0140*/                   BRA `(.L_x_4);
        /*0150*/                   NOP;
        /*0160*/                   NOP;
        /*0170*/                   NOP;
        /*0180*/                   NOP;
        /*0190*/                   NOP;
        /*01a0*/                   NOP;
        /*01b0*/                   NOP;
        /*01c0*/                   NOP;
        /*01d0*/                   NOP;
        /*01e0*/                   NOP;
        /*01f0*/                   NOP;
.L_x_33:


//--------------------- .text.jmp_to_target       --------------------------
	.section	.text.jmp_to_target,"ax",@progbits
	.align	128
        .global         jmp_to_target
        .type           jmp_to_target,@function
        .size           jmp_to_target,(.L_x_34 - jmp_to_target)
jmp_to_target:
.text.jmp_to_target:
        /*0000*/                   RET.ABS.NODEC R20 0x0                                                 ?trans5;
.L_x_5:
        /*0010*/                   BRA `(.L_x_5);
        /*0020*/                   NOP;
        /*0030*/                   NOP;
        /*0040*/                   NOP;
        /*0050*/                   NOP;
        /*0060*/                   NOP;
        /*0070*/                   NOP;
        /*0080*/                   NOP;
        /*0090*/                   NOP;
        /*00a0*/                   NOP;
        /*00b0*/                   NOP;
        /*00c0*/                   NOP;
        /*00d0*/                   NOP;
        /*00e0*/                   NOP;
        /*00f0*/                   NOP;
.L_x_34:


//--------------------- .text.jmp_to_target_helper --------------------------
	.section	.text.jmp_to_target_helper,"ax",@progbits
	.align	128
        .global         jmp_to_target_helper
        .type           jmp_to_target_helper,@function
        .size           jmp_to_target_helper,(.L_x_35 - jmp_to_target_helper)
jmp_to_target_helper:
.text.jmp_to_target_helper:
        /*0000*/                   BSSY.RECONVERGENT B0, `(.L_x_6)                                       ?trans1;
        /*0010*/                   ISETP.NE.S64.AND P0, PT, R4, RZ, PT                                   ?WAIT14_END_GROUP;
        /*0020*/               @P0 EXIT                                                                  ?trans5;
        /*0030*/                   BRA `(.L_x_7)                                                         ?trans5;
.L_x_7:
        /*0040*/                   BSYNC.RECONVERGENT B0                                                 ?trans5;
.L_x_6:
        /*0050*/                   RET.ABS.NODEC R20 0x0                                                 ?trans5;
.L_x_8:
        /*0060*/                   BRA `(.L_x_8);
        /*0070*/                   NOP;
        /*0080*/                   NOP;
        /*0090*/                   NOP;
        /*00a0*/                   NOP;
        /*00b0*/                   NOP;
        /*00c0*/                   NOP;
        /*00d0*/                   NOP;
        /*00e0*/                   NOP;
        /*00f0*/                   NOP;
.L_x_35:


//--------------------- .text._Z4exitv            --------------------------
	.section	.text._Z4exitv,"ax",@progbits
	.align	128
        .global         _Z4exitv
        .type           _Z4exitv,@function
        .size           _Z4exitv,(.L_x_36 - _Z4exitv)
_Z4exitv:
.text._Z4exitv:
        /*0000*/                   EXIT                                                                  ?trans5;
.L_x_9:
        /*0010*/                   BRA `(.L_x_9);
        /*0020*/                   NOP;
        /*0030*/                   NOP;
        /*0040*/                   NOP;
        /*0050*/                   NOP;
        /*0060*/                   NOP;
        /*0070*/                   NOP;
        /*0080*/                   NOP;
        /*0090*/                   NOP;
        /*00a0*/                   NOP;
        /*00b0*/                   NOP;
        /*00c0*/                   NOP;
        /*00d0*/                   NOP;
        /*00e0*/                   NOP;
        /*00f0*/                   NOP;
.L_x_36:


//--------------------- .text.relocate_func_call  --------------------------
	.section	.text.relocate_func_call,"ax",@progbits
	.align	128
        .global         relocate_func_call
        .type           relocate_func_call,@function
        .size           relocate_func_call,(.L_x_37 - relocate_func_call)
relocate_func_call:
.text.relocate_func_call:
        /*0000*/                   IADD3 R1, PT, PT, R1, -0x8, RZ                                        ?WAIT5_END_GROUP;
        /*0010*/                   STL [R1+0x4], R21                                                     ?trans4;
        /*0020*/                   STL [R1], R20                                      &rd=0x0            ?trans1;
        /*0030*/                   IADD.64 R4, R4, 0x12345678                                            ?trans2;
        /*0040*/                   MOV R20, 32@lo((relocate_func_call + .L_x_1@srel)) &req={0}           ?trans1;
        /*0050*/                   MOV R21, 32@hi((relocate_func_call + .L_x_1@srel))                    ?WAIT7_END_GROUP;
        /*0060*/                   CALL.ABS.NOINC `(dummy)                                               ?trans5;
.L_x_1:
        /*0070*/                   LDL R20, [R1]                                      &wr=0x2            ?trans4;
        /*0080*/                   LDL R21, [R1+0x4]                                  &rd=0x0 &wr=0x2    ?trans2;
        /*0090*/                   IADD3 R1, PT, PT, R1, 0x8, RZ                      &req={0}           ?trans1;
        /*00a0*/                   RET.ABS.NODEC R20 0x0                              &req={2}           ?trans6;
.L_x_10:
        /*00b0*/                   BRA `(.L_x_10);
        /*00c0*/                   NOP;
        /*00d0*/                   NOP;
        /*00e0*/                   NOP;
        /*00f0*/                   NOP;
        /*0100*/                   NOP;
        /*0110*/                   NOP;
        /*0120*/                   NOP;
        /*0130*/                   NOP;
        /*0140*/                   NOP;
        /*0150*/                   NOP;
        /*0160*/                   NOP;
        /*0170*/                   NOP;
.L_x_37:


//--------------------- .text.check_preempt       --------------------------
	.section	.text.check_preempt,"ax",@progbits
	.align	128
        .global         check_preempt
        .type           check_preempt,@function
        .size           check_preempt,(.L_x_38 - check_preempt)
check_preempt:
.text.check_preempt:
        /*0000*/                   BSSY.RECONVERGENT B0, `(.L_x_11)                                      ?trans4;
        /*0010*/                   LDC R4, c[0x0][0x170];
        /*0020*/                   LDC R5, c[0x0][0x174];
        /*0030*/                   S2R R0, SR_CTAID.X                                 &wr=0x0            ?trans1;
        /*0040*/                   LDC R3, c[0x0][0x374]                              &wr=0x0            ?trans1;
        /*0050*/                   S2R R8, SR_CTAID.Y                                 &wr=0x0            ?trans1;
        /*0060*/                   S2R R10, SR_TID.X                                  &wr=0x1            ?trans1;
        /*0070*/                   S2R R11, SR_TID.Y                                  &wr=0x1            ?trans1;
        /*0080*/                   S2R R9, SR_CTAID.Z                                 &wr=0x2            ?trans1;
        /*0090*/                   IMAD R0, R0, R3, R8                                &req={0}           ?trans2;
        /*00a0*/                   S2R R8, SR_TID.Z                                   &wr=0x1            ?trans1;
        /*00b0*/                   LDC R3, c[0x0][0x378]                              &wr=0x2            ?trans2;
        /*00c0*/                   IMAD R0, R0, R3, R9                                &req={2}           ?WAIT5_END_GROUP;
        /*00d0*/                   IADD3 R9, PT, PT, R0, 0x4, R0                                         ?trans2;
        /*00e0*/                   LOP3.LUT P0, RZ, R8, R10, R11, 0xfe, !PT           &req={1}           ?WAIT3_END_GROUP;
        /*00f0*/                   IMAD.WIDE.U32 R8, R9, 0x4, R4                                         ?WAIT10_END_GROUP;
        /*0100*/               @P0 BRA `(.L_x_12)                                                        ?trans5;
        /*0110*/                   LD.E R0, [R4]                                      &wr=0x2            ?trans1;
        /*0120*/                   BSSY.RECONVERGENT B1, `(.L_x_13)                                      ?trans1;
        /*0130*/                   ISETP.NE.AND P0, PT, R0, RZ, PT                    &req={2}           ?WAIT13_END_GROUP;
        /*0140*/              @!P0 ST.E [R8], RZ                                      &rd=0x0            ?trans1;
        /*0150*/              @!P0 BRA `(.L_x_14)                                                        ?trans5;
        /*0160*/                   LDC R6, c[0x0][0x180];
        /*0170*/                   LDC R7, c[0x0][0x184];
        /*0180*/                   HFMA2 R3, -RZ, RZ, 0, 5.9604644775390625e-08                          ?WAIT5_END_GROUP;
        /*0190*/                   ST.E [R8], R3                                      &rd=0x1            ?trans4;
        /*01a0*/                   LD.E.64 R10, [R4+0x8]                              &wr=0x2            ?trans1;
        /*01b0*/                   BSSY.RELIABLE B2, `(.L_x_15)                                          ?trans1;
        /*01c0*/                   ISETP.NE.S64.AND P0, PT, R10, RZ, PT               &req={2}           ?WAIT14_END_GROUP;
        /*01d0*/              @!P0 BRA `(.L_x_16)                                     &req={1}           ?trans5;
        /*01e0*/                   ISETP.NE.S64.AND P0, PT, R10, R6, PT                                  ?WAIT14_END_GROUP;
        /*01f0*/               @P0 BREAK.RELIABLE B2                                                     ?trans5;
        /*0200*/              @!P0 BRA `(.L_x_17)                                                        ?trans5;
        /*0210*/                   BRA `(.L_x_14)                                                        ?trans5;
.L_x_16:
        /*0220*/                   ST.E.64 [R4+0x8], R6                               &rd=0x1            ?trans2;
.L_x_17:
        /*0230*/                   BSYNC.RELIABLE B2                                                     ?trans5;
.L_x_15:
        /*0240*/                   ST.E [R8+0x4], R3                                  &rd=0x2            ?trans2;
.L_x_14:
        /*0250*/                   BSYNC.RECONVERGENT B1                                                 ?trans5;
.L_x_13:
        /*0260*/              @!PT LDS RZ, [RZ]                                                          ?trans1;
        /*0270*/              @!PT LDS RZ, [RZ]                                                          ?trans1;
        /*0280*/              @!PT LDS RZ, [RZ]                                                          ?trans1;
        /*0290*/              @!PT LDS RZ, [RZ]                                                          ?trans1;
        /*02a0*/                   MEMBAR.SC.CTA                                                         ?trans6;
.L_x_12:
        /*02b0*/                   WARPSYNC.ALL                                                          ?trans5;
        /*02c0*/                   NOP                                                                   ?trans1;
        /*02d0*/                   BAR.SYNC.DEFER_BLOCKING 0x0                                           ?trans6;
        /*02e0*/                   LD.E R8, [R8]                                      &req={2,0} &wr=0x2 ?trans2;
        /*02f0*/                   ISETP.NE.AND P0, PT, R8, RZ, PT                    &req={2}           ?WAIT13_END_GROUP;
        /*0300*/               @P0 EXIT                                                                  ?trans5;
        /*0310*/                   BSYNC.RECONVERGENT B0                                                 ?trans5;
.L_x_11:
        /*0320*/                   RET.ABS.NODEC R20 0x0                              &req={1}           ?trans5;
.L_x_18:
        /*0330*/                   BRA `(.L_x_18);
        /*0340*/                   NOP;
        /*0350*/                   NOP;
        /*0360*/                   NOP;
        /*0370*/                   NOP;
        /*0380*/                   NOP;
        /*0390*/                   NOP;
        /*03a0*/                   NOP;
        /*03b0*/                   NOP;
        /*03c0*/                   NOP;
        /*03d0*/                   NOP;
        /*03e0*/                   NOP;
        /*03f0*/                   NOP;
.L_x_38:


//--------------------- .text._Z11get_blockidv    --------------------------
	.section	.text._Z11get_blockidv,"ax",@progbits
	.align	128
        .global         _Z11get_blockidv
        .type           _Z11get_blockidv,@function
        .size           _Z11get_blockidv,(.L_x_39 - _Z11get_blockidv)
_Z11get_blockidv:
.text._Z11get_blockidv:
        /*0000*/                   S2R R4, SR_CTAID.X                                 &wr=0x0            ?trans1;
        /*0010*/                   LDC R5, c[0x0][0x374]                              &wr=0x0            ?trans1;
        /*0020*/                   S2R R6, SR_CTAID.Y                                 &wr=0x0            ?trans1;
        /*0030*/                   S2R R0, SR_CTAID.Z                                 &wr=0x1            ?trans6;
        /*0040*/                   LDC R3, c[0x0][0x378]                              &wr=0x1            ?trans1;
        /*0050*/                   IMAD R4, R4, R5, R6                                &req={0}           ?WAIT4_END_GROUP;
        /*0060*/                   IMAD R4, R4, R3, R0                                &req={1}           ?trans1;
        /*0070*/                   RET.ABS.NODEC R20 0x0                                                 ?trans6;
.L_x_19:
        /*0080*/                   BRA `(.L_x_19);
        /*0090*/                   NOP;
        /*00a0*/                   NOP;
        /*00b0*/                   NOP;
        /*00c0*/                   NOP;
        /*00d0*/                   NOP;
        /*00e0*/                   NOP;
        /*00f0*/                   NOP;
        /*0100*/                   NOP;
        /*0110*/                   NOP;
        /*0120*/                   NOP;
        /*0130*/                   NOP;
        /*0140*/                   NOP;
        /*0150*/                   NOP;
        /*0160*/                   NOP;
        /*0170*/                   NOP;
.L_x_39:


//--------------------- .text.check_preempt_trap  --------------------------
	.section	.text.check_preempt_trap,"ax",@progbits
	.align	128
        .global         check_preempt_trap
        .type           check_preempt_trap,@function
        .size           check_preempt_trap,(.L_x_40 - check_preempt_trap)
check_preempt_trap:
.text.check_preempt_trap:
        /*0000*/                   BSSY.RECONVERGENT B0, `(.L_x_20)                                      ?trans4;
        /*0010*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*0020*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*0030*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*0040*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*0050*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*0060*/                   ISETP.NE.AND P0, PT, R8, RZ, PT                                       ?trans1;
        /*0070*/              @!PT LDS RZ, [RZ]                                                          ?trans1;
        /*0080*/              @!PT LDS RZ, [RZ]                                                          ?trans1;
        /*0090*/              @!PT LDS RZ, [RZ]                                                          ?trans1;
        /*00a0*/              @!PT LDS RZ, [RZ]                                                          ?trans1;
        /*00b0*/                   MEMBAR.ALL.CTA;
        /*00c0*/                   MEMBAR.SC.GPU                                                         ?trans6;
        /*00d0*/                   ERRBAR;
        /*00e0*/                   CGAERRBAR                                                             ?trans6;
        /*00f0*/                   CCTL.IVALL                                                            ?trans1;
        /*0100*/              @!P0 BRA `(.L_x_21)                                                        ?trans5;
        /*0110*/                   LD.E R0, [R4]                                      &wr=0x2            ?trans2;
        /*0120*/                   ISETP.NE.AND P0, PT, R0, RZ, PT                    &req={2}           ?WAIT13_END_GROUP;
        /*0130*/              @!P0 BRA `(.L_x_21)                                                        ?trans5;
        /*0140*/                   LD.E.64 R8, [R4+0x8]                               &wr=0x2            ?trans1;
        /*0150*/                   BSSY.RECONVERGENT B1, `(.L_x_22)                                      ?trans1;
        /*0160*/                   ISETP.NE.S64.AND P0, PT, R8, RZ, PT                &req={2}           ?WAIT14_END_GROUP;
        /*0170*/              @!P0 BRA `(.L_x_23)                                                        ?trans5;
        /*0180*/                   ISETP.NE.S64.AND P0, PT, R8, R6, PT                                   ?WAIT14_END_GROUP;
        /*0190*/              @!P0 BRA `(.L_x_24)                                                        ?trans5;
        /*01a0*/                   EXIT                                                                  ?trans5;
.L_x_23:
        /*01b0*/                   ST.E.64 [R4+0x8], R6                               &rd=0x0            ?trans2;
.L_x_24:
        /*01c0*/                   BSYNC.RECONVERGENT B1                                                 ?trans5;
.L_x_22:
        /*01d0*/                   S2R R0, SR_CTAID.Y                                 &wr=0x1            ?trans1;
        /*01e0*/                   LDC R3, c[0x0][0x374]                              &wr=0x1            ?trans1;
        /*01f0*/                   S2R R6, SR_CTAID.X                                 &req={0} &wr=0x1   ?trans1;
        /*0200*/                   S2R R8, SR_CTAID.Z                                 &wr=0x0            ?trans6;
        /*0210*/                   LDC R7, c[0x0][0x378]                              &wr=0x0            ?trans1;
        /*0220*/                   IMAD R0, R3, R6, R0                                &req={1}           ?WAIT4_END_GROUP;
        /*0230*/                   IMAD R0, R0, R7, R8                                &req={0}           ?trans2;
        /*0240*/                   HFMA2 R7, -RZ, RZ, 0, 5.9604644775390625e-08                          ?WAIT3_END_GROUP;
        /*0250*/                   IADD3 R3, PT, PT, R0, 0x4, R0                                         ?WAIT5_END_GROUP;
        /*0260*/                   IMAD.WIDE.U32 R4, R3, 0x4, R4                                         ?WAIT5_END_GROUP;
        /*0270*/                   ST.E [R4+0x4], R7                                                     ?trans1;
        /*0280*/                   EXIT                                                                  ?trans5;
.L_x_21:
        /*0290*/                   BSYNC.RECONVERGENT B0                                                 ?trans5;
.L_x_20:
        /*02a0*/                   RET.ABS.NODEC R20 0x0                                                 ?trans5;
.L_x_25:
        /*02b0*/                   BRA `(.L_x_25);
        /*02c0*/                   NOP;
        /*02d0*/                   NOP;
        /*02e0*/                   NOP;
        /*02f0*/                   NOP;
        /*0300*/                   NOP;
        /*0310*/                   NOP;
        /*0320*/                   NOP;
        /*0330*/                   NOP;
        /*0340*/                   NOP;
        /*0350*/                   NOP;
        /*0360*/                   NOP;
        /*0370*/                   NOP;
.L_x_40:


//--------------------- .text.restore_exec        --------------------------
	.section	.text.restore_exec,"ax",@progbits
	.align	128
        .global         restore_exec
        .type           restore_exec,@function
        .size           restore_exec,(.L_x_41 - restore_exec)
restore_exec:
.text.restore_exec:
        /*0000*/                   BSSY.RECONVERGENT B0, `(.L_x_26)                                      ?trans4;
        /*0010*/                   LDC R4, c[0x0][0x170];
        /*0020*/                   LDC R5, c[0x0][0x174];
        /*0030*/                   S2R R0, SR_CTAID.X                                 &wr=0x0            ?trans1;
        /*0040*/                   LDC R3, c[0x0][0x374]                              &wr=0x0            ?trans1;
        /*0050*/                   S2R R6, SR_CTAID.Y                                 &wr=0x0            ?trans1;
        /*0060*/                   S2R R8, SR_CTAID.Z                                 &wr=0x1            ?trans6;
        /*0070*/                   LDC R7, c[0x0][0x378]                              &wr=0x1            ?trans1;
        /*0080*/                   IMAD R0, R0, R3, R6                                &req={0}           ?WAIT4_END_GROUP;
        /*0090*/                   IMAD R0, R0, R7, R8                                &req={1}           ?WAIT5_END_GROUP;
        /*00a0*/                   IADD3 R3, PT, PT, R0, 0x5, R0                                         ?WAIT5_END_GROUP;
        /*00b0*/                   IMAD.WIDE.U32 R4, R3, 0x4, R4                                         ?WAIT5_END_GROUP;
        /*00c0*/                   LD.E R0, [R4]                                      &wr=0x2            ?trans2;
        /*00d0*/                   ISETP.NE.AND P0, PT, R0, RZ, PT                    &req={2}           ?WAIT13_END_GROUP;
        /*00e0*/              @!P0 EXIT                                                                  ?trans5;
        /*00f0*/                   WARPSYNC.ALL                                                          ?trans5;
        /*0100*/                   NOP                                                                   ?trans1;
        /*0110*/                   BAR.SYNC.DEFER_BLOCKING 0x0                                           ?trans6;
        /*0120*/                   ST.E [R4], RZ                                      &rd=0x0            ?trans1;
        /*0130*/                   LDC R20, c[0x0][0x178];
        /*0140*/                   LDC R21, c[0x0][0x17c];
        /*0150*/                   BSYNC.RECONVERGENT B0                                                 ?trans5;
.L_x_26:
        /*0160*/                   RET.ABS.NODEC R20 0x0                              &req={0}           ?trans5;
.L_x_27:
        /*0170*/                   BRA `(.L_x_27);
        /*0180*/                   NOP;
        /*0190*/                   NOP;
        /*01a0*/                   NOP;
        /*01b0*/                   NOP;
        /*01c0*/                   NOP;
        /*01d0*/                   NOP;
        /*01e0*/                   NOP;
        /*01f0*/                   NOP;
.L_x_41:


//--------------------- .text.exit_if_idempotent  --------------------------
	.section	.text.exit_if_idempotent,"ax",@progbits
	.align	128
        .global         exit_if_idempotent
        .type           exit_if_idempotent,@function
        .size           exit_if_idempotent,(.L_x_42 - exit_if_idempotent)
exit_if_idempotent:
.text.exit_if_idempotent:
        /*0000*/                   BSSY.RECONVERGENT B0, `(.L_x_28)                                      ?trans4;
        /*0010*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*0020*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*0030*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*0040*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*0050*/                   LD.E R0, [R4]                                      &wr=0x2            ?trans2;
        /*0060*/                   ISETP.NE.AND P0, PT, R0, RZ, PT                    &req={2}           ?WAIT13_END_GROUP;
        /*0070*/              @!P0 BRA `(.L_x_29)                                                        ?trans5;
        /*0080*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*0090*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*00a0*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*00b0*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*00c0*/                   S2R R0, SR_CTAID.X                                 &wr=0x0            ?trans1;
        /*00d0*/                   LDC R3, c[0x0][0x374]                              &wr=0x0            ?trans1;
        /*00e0*/                   S2R R8, SR_CTAID.Y                                 &wr=0x0            ?trans1;
        /*00f0*/                   S2R R10, SR_CTAID.Z                                &wr=0x1            ?trans6;
        /*0100*/                   LDC R9, c[0x0][0x378]                              &wr=0x1            ?trans1;
        /*0110*/                   ST.E.64 [R4+0x8], R6                                                  ?trans1;
        /*0120*/                   IMAD R0, R0, R3, R8                                &req={0}           ?WAIT2_END_GROUP;
        /*0130*/                   HFMA2 R3, -RZ, RZ, 0, 5.9604644775390625e-08                          ?trans2;
        /*0140*/                   IMAD R0, R0, R9, R10                               &req={1}           ?WAIT3_END_GROUP;
        /*0150*/                   ST.E [R4], R3                                                         ?trans2;
        /*0160*/                   IADD3 R9, PT, PT, R0, 0x5, R0                                         ?WAIT5_END_GROUP;
        /*0170*/                   IMAD.WIDE.U32 R8, R9, 0x4, R4                                         ?WAIT5_END_GROUP;
        /*0180*/                   ST.E [R8], R3                                                         ?trans1;
        /*0190*/                   EXIT                                                                  ?trans5;
.L_x_29:
        /*01a0*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*01b0*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*01c0*/                   BPT.TRAP 0x1                                                          ?trans5;
        /*01d0*/                   BSYNC.RECONVERGENT B0                                                 ?trans5;
.L_x_28:
        /*01e0*/                   RET.ABS.NODEC R20 0x0                                                 ?trans5;
.L_x_30:
        /*01f0*/                   BRA `(.L_x_30);
        /*0200*/                   NOP;
        /*0210*/                   NOP;
        /*0220*/                   NOP;
        /*0230*/                   NOP;
        /*0240*/                   NOP;
        /*0250*/                   NOP;
        /*0260*/                   NOP;
        /*0270*/                   NOP;
.L_x_42:


//--------------------- SYMBOLS --------------------------

	.type		.nv.reservedSmem.offset0,@object
	.size		.nv.reservedSmem.offset0,0x4
	.type		vprintf,@function

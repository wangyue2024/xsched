	.headerflags	@"EF_CUDA_TEXMODE_UNIFIED EF_CUDA_64BIT_ADDRESS EF_CUDA_SM86 EF_CUDA_VIRTUAL_SM(EF_CUDA_SM86)"
	.elftype	@"ET_EXEC"


//--------------------- .text.nop_func            --------------------------
	.section	.text.nop_func,"ax",@progbits
	.sectioninfo	@"SHI_REGISTERS=24"
	.align	128
        .global         nop_func
        .type           nop_func,@function
        .size           nop_func,(.L_x_32 - nop_func)
nop_func:
.text.nop_func:
        /*0000*/                   BPT.TRAP 0x1 ;
        /*0010*/                   RET.ABS.NODEC R20 0x0 ;
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
.L_x_32:


//--------------------- .text._Z3nopv             --------------------------
	.section	.text._Z3nopv,"ax",@progbits
	.sectioninfo	@"SHI_REGISTERS=24"
	.align	128
        .global         _Z3nopv
        .type           _Z3nopv,@function
        .size           _Z3nopv,(.L_x_33 - _Z3nopv)
_Z3nopv:
.text._Z3nopv:
        /*0000*/                   BPT.TRAP 0x1 ;
        /*0010*/                   RET.ABS.NODEC R20 0x0 ;
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
.L_x_33:


//--------------------- .text.dummy               --------------------------
	.section	.text.dummy,"ax",@progbits
	.sectioninfo	@"SHI_REGISTERS=24"
	.align	128
        .global         dummy
        .type           dummy,@function
        .size           dummy,(.L_x_34 - dummy)
dummy:
.text.dummy:
        /*0000*/                   IADD3 R1, R1, -0x10, RZ ;
        /*0010*/                   IMAD.MOV.U32 R9, RZ, RZ, R5 ;
        /*0020*/                   MOV R8, R4 ;
        /*0030*/                   STL [R1+0xc], R21 ;
        /*0040*/                   STL [R1+0x8], R20 ;
        /*0050*/                   STL.64 [R1], R8 ;
        /*0060*/                   IADD3 R6, P0, R1, c[0x0][0x20], RZ ;
        /*0070*/                   MOV R4, 32@lo($str) ;
        /*0080*/                   MOV R5, 32@hi($str) ;
        /*0090*/                   IMAD.X R7, RZ, RZ, c[0x0][0x24], P0 ;
        /*00a0*/                   MOV R20, 32@lo((dummy + .L_x_0@srel)) ;
        /*00b0*/                   MOV R21, 32@hi((dummy + .L_x_0@srel)) ;
        /*00c0*/                   CALL.ABS.NOINC `(vprintf) ;
.L_x_0:
        /*00d0*/                   LDL R20, [R1+0x8] ;
        /*00e0*/                   LDL R21, [R1+0xc] ;
        /*00f0*/                   IADD3 R1, R1, 0x10, RZ ;
        /*0100*/                   RET.ABS.NODEC R20 0x0 ;
.L_x_4:
        /*0110*/                   BRA `(.L_x_4);
        /*0120*/                   NOP;
        /*0130*/                   NOP;
        /*0140*/                   NOP;
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
.L_x_34:


//--------------------- .text.jmp_to_target       --------------------------
	.section	.text.jmp_to_target,"ax",@progbits
	.sectioninfo	@"SHI_REGISTERS=24"
	.align	128
        .global         jmp_to_target
        .type           jmp_to_target,@function
        .size           jmp_to_target,(.L_x_35 - jmp_to_target)
jmp_to_target:
.text.jmp_to_target:
        /*0000*/                   RET.ABS.NODEC R20 0x0 ;
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
.L_x_35:


//--------------------- .text.jmp_to_target_helper --------------------------
	.section	.text.jmp_to_target_helper,"ax",@progbits
	.sectioninfo	@"SHI_REGISTERS=24"
	.align	128
        .global         jmp_to_target_helper
        .type           jmp_to_target_helper,@function
        .size           jmp_to_target_helper,(.L_x_36 - jmp_to_target_helper)
jmp_to_target_helper:
.text.jmp_to_target_helper:
        /*0000*/                   BSSY B0, `(.L_x_6) ;
        /*0010*/                   ISETP.NE.U32.AND P0, PT, R4, RZ, PT ;
        /*0020*/                   ISETP.NE.AND.EX P0, PT, R5, RZ, PT, P0 ;
        /*0030*/               @P0 EXIT ;
        /*0040*/                   BRA `(.L_x_7) ;
.L_x_7:
        /*0050*/                   BSYNC B0 ;
.L_x_6:
        /*0060*/                   RET.ABS.NODEC R20 0x0 ;
.L_x_8:
        /*0070*/                   BRA `(.L_x_8);
        /*0080*/                   NOP;
        /*0090*/                   NOP;
        /*00a0*/                   NOP;
        /*00b0*/                   NOP;
        /*00c0*/                   NOP;
        /*00d0*/                   NOP;
        /*00e0*/                   NOP;
        /*00f0*/                   NOP;
.L_x_36:


//--------------------- .text._Z4exitv            --------------------------
	.section	.text._Z4exitv,"ax",@progbits
	.sectioninfo	@"SHI_REGISTERS=24"
	.align	128
        .global         _Z4exitv
        .type           _Z4exitv,@function
        .size           _Z4exitv,(.L_x_37 - _Z4exitv)
_Z4exitv:
.text._Z4exitv:
        /*0000*/                   EXIT ;
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
.L_x_37:


//--------------------- .text.relocate_func_call  --------------------------
	.section	.text.relocate_func_call,"ax",@progbits
	.sectioninfo	@"SHI_REGISTERS=24"
	.align	128
        .global         relocate_func_call
        .type           relocate_func_call,@function
        .size           relocate_func_call,(.L_x_38 - relocate_func_call)
relocate_func_call:
.text.relocate_func_call:
        /*0000*/                   IADD3 R1, R1, -0x8, RZ ;
        /*0010*/                   STL [R1+0x4], R21 ;
        /*0020*/                   STL [R1], R20 ;
        /*0030*/                   IADD3 R4, P0, R4, 0x12345678, RZ ;
        /*0040*/                   IADD3.X R5, RZ, R5, RZ, P0, !PT ;
        /*0050*/                   MOV R20, 32@lo((relocate_func_call + .L_x_1@srel)) ;
        /*0060*/                   MOV R21, 32@hi((relocate_func_call + .L_x_1@srel)) ;
        /*0070*/                   CALL.ABS.NOINC `(dummy) ;
.L_x_1:
        /*0080*/                   LDL R20, [R1] ;
        /*0090*/                   LDL R21, [R1+0x4] ;
        /*00a0*/                   IADD3 R1, R1, 0x8, RZ ;
        /*00b0*/                   RET.ABS.NODEC R20 0x0 ;
.L_x_10:
        /*00c0*/                   BRA `(.L_x_10);
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
.L_x_38:


//--------------------- .text.check_preempt       --------------------------
	.section	.text.check_preempt,"ax",@progbits
	.sectionflags	@"SHF_BARRIERS=1"
	.sectioninfo	@"SHI_REGISTERS=24"
	.align	128
        .global         check_preempt
        .type           check_preempt,@function
        .size           check_preempt,(.L_x_39 - check_preempt)
check_preempt:
.text.check_preempt:
        /*0000*/                   BSSY B0, `(.L_x_11) ;
        /*0010*/                   BPT.TRAP 0x1 ;
        /*0020*/                   BPT.TRAP 0x1 ;
        /*0030*/                   S2R R8, SR_TID.Y ;
        /*0040*/                   S2R R11, SR_TID.X ;
        /*0050*/                   S2R R10, SR_TID.Z ;
        /*0060*/                   S2R R0, SR_CTAID.X ;
        /*0070*/                   S2R R3, SR_CTAID.Y ;
        /*0080*/                   S2R R9, SR_CTAID.Z ;
        /*0090*/                   LOP3.LUT P0, RZ, R10, R11, R8, 0xfe, !PT ;
        /*00a0*/                   IMAD R0, R0, c[0x0][0x10], R3 ;
        /*00b0*/                   IMAD R0, R0, c[0x0][0x14], R9 ;
        /*00c0*/                   LEA R9, R0, 0x4, 0x1 ;
        /*00d0*/                   IMAD.WIDE.U32 R8, R9, 0x4, R4 ;
        /*00e0*/               @P0 BRA `(.L_x_12) ;
        /*00f0*/                   LD.E R0, [R4] ;
        /*0100*/                   BSSY B1, `(.L_x_13) ;
        /*0110*/                   ISETP.NE.AND P0, PT, R0, RZ, PT ;
        /*0120*/              @!P0 BRA `(.L_x_14) ;
        /*0130*/                   BPT.TRAP 0x1 ;
        /*0140*/                   BPT.TRAP 0x1 ;
        /*0150*/                   IMAD.MOV.U32 R3, RZ, RZ, 0x1 ;
        /*0160*/                   ST.E [R8], R3 ;
        /*0170*/                   LD.E.64 R10, [R4+0x8] ;
        /*0180*/                   BSSY B2, `(.L_x_15) ;
        /*0190*/                   ISETP.NE.U32.AND P0, PT, R10, RZ, PT ;
        /*01a0*/                   ISETP.NE.AND.EX P0, PT, R11, RZ, PT, P0 ;
        /*01b0*/              @!P0 BRA `(.L_x_16) ;
        /*01c0*/                   ISETP.NE.U32.AND P0, PT, R10, R6, PT ;
        /*01d0*/                   ISETP.NE.AND.EX P0, PT, R11, R7, PT, P0 ;
        /*01e0*/               @P0 BREAK B2 ;
        /*01f0*/              @!P0 BRA `(.L_x_17) ;
        /*0200*/                   BRA `(.L_x_18) ;
.L_x_16:
        /*0210*/                   ST.E.64 [R4+0x8], R6 ;
.L_x_17:
        /*0220*/                   BSYNC B2 ;
.L_x_15:
        /*0230*/                   ST.E [R8+0x4], R3 ;
        /*0240*/                   BRA `(.L_x_18) ;
.L_x_14:
        /*0250*/                   ST.E [R8], RZ ;
.L_x_18:
        /*0260*/                   BSYNC B1 ;
.L_x_13:
        /*0270*/              @!PT LDS RZ, [RZ] ;
        /*0280*/              @!PT LDS RZ, [RZ] ;
        /*0290*/              @!PT LDS RZ, [RZ] ;
        /*02a0*/              @!PT LDS RZ, [RZ] ;
        /*02b0*/                   MEMBAR.SC.CTA ;
.L_x_12:
        /*02c0*/                   WARPSYNC 0xffffffff ;
        /*02d0*/                   BAR.SYNC.DEFER_BLOCKING 0x0 ;
        /*02e0*/                   LD.E R8, [R8] ;
        /*02f0*/                   ISETP.NE.AND P0, PT, R8, RZ, PT ;
        /*0300*/               @P0 EXIT ;
        /*0310*/                   BSYNC B0 ;
.L_x_11:
        /*0320*/                   RET.ABS.NODEC R20 0x0 ;
.L_x_19:
        /*0330*/                   BRA `(.L_x_19);
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
.L_x_39:


//--------------------- .text._Z11get_blockidv    --------------------------
	.section	.text._Z11get_blockidv,"ax",@progbits
	.sectioninfo	@"SHI_REGISTERS=24"
	.align	128
        .global         _Z11get_blockidv
        .type           _Z11get_blockidv,@function
        .size           _Z11get_blockidv,(.L_x_40 - _Z11get_blockidv)
_Z11get_blockidv:
.text._Z11get_blockidv:
        /*0000*/                   S2R R4, SR_CTAID.X ;
        /*0010*/                   S2R R5, SR_CTAID.Y ;
        /*0020*/                   S2R R3, SR_CTAID.Z ;
        /*0030*/                   IMAD R4, R4, c[0x0][0x10], R5 ;
        /*0040*/                   IMAD R4, R4, c[0x0][0x14], R3 ;
        /*0050*/                   RET.ABS.NODEC R20 0x0 ;
.L_x_20:
        /*0060*/                   BRA `(.L_x_20);
        /*0070*/                   NOP;
        /*0080*/                   NOP;
        /*0090*/                   NOP;
        /*00a0*/                   NOP;
        /*00b0*/                   NOP;
        /*00c0*/                   NOP;
        /*00d0*/                   NOP;
        /*00e0*/                   NOP;
        /*00f0*/                   NOP;
.L_x_40:


//--------------------- .text.check_preempt_trap  --------------------------
	.section	.text.check_preempt_trap,"ax",@progbits
	.sectioninfo	@"SHI_REGISTERS=24"
	.align	128
        .global         check_preempt_trap
        .type           check_preempt_trap,@function
        .size           check_preempt_trap,(.L_x_41 - check_preempt_trap)
check_preempt_trap:
.text.check_preempt_trap:
        /*0000*/                   BSSY B0, `(.L_x_21) ;
        /*0010*/                   BPT.TRAP 0x1 ;
        /*0020*/                   BPT.TRAP 0x1 ;
        /*0030*/                   BPT.TRAP 0x1 ;
        /*0040*/                   BPT.TRAP 0x1 ;
        /*0050*/                   BPT.TRAP 0x1 ;
        /*0060*/                   S2R R0, SR_CTAID.X ;
        /*0070*/                   ISETP.NE.AND P0, PT, R8, RZ, PT ;
        /*0080*/                   S2R R3, SR_CTAID.Y ;
        /*0090*/                   S2R R11, SR_CTAID.Z ;
        /*00a0*/              @!PT LDS RZ, [RZ] ;
        /*00b0*/              @!PT LDS RZ, [RZ] ;
        /*00c0*/              @!PT LDS RZ, [RZ] ;
        /*00d0*/              @!PT LDS RZ, [RZ] ;
        /*00e0*/                   MEMBAR.SC.GPU ;
        /*00f0*/                   ERRBAR;
        /*0100*/                   CCTL.IVALL ;
        /*0110*/              @!P0 BRA `(.L_x_22) ;
        /*0120*/                   LD.E R8, [R4] ;
        /*0130*/                   ISETP.NE.AND P0, PT, R8, RZ, PT ;
        /*0140*/              @!P0 BRA `(.L_x_22) ;
        /*0150*/                   LD.E.64 R8, [R4+0x8] ;
        /*0160*/                   IMAD R0, R0, c[0x0][0x10], R3 ;
        /*0170*/                   BSSY B1, `(.L_x_23) ;
        /*0180*/                   IMAD R0, R0, c[0x0][0x14], R11 ;
        /*0190*/                   LEA R11, R0, 0x4, 0x1 ;
        /*01a0*/                   IMAD.WIDE.U32 R10, R11, 0x4, R4 ;
        /*01b0*/                   ISETP.NE.U32.AND P0, PT, R8, RZ, PT ;
        /*01c0*/                   ISETP.NE.AND.EX P0, PT, R9, RZ, PT, P0 ;
        /*01d0*/              @!P0 BRA `(.L_x_24) ;
        /*01e0*/                   ISETP.NE.U32.AND P0, PT, R8, R6, PT ;
        /*01f0*/                   ISETP.NE.AND.EX P0, PT, R9, R7, PT, P0 ;
        /*0200*/              @!P0 BRA `(.L_x_25) ;
        /*0210*/                   EXIT ;
.L_x_24:
        /*0220*/                   ST.E.64 [R4+0x8], R6 ;
.L_x_25:
        /*0230*/                   BSYNC B1 ;
.L_x_23:
        /*0240*/                   MOV R3, 0x1 ;
        /*0250*/                   ST.E [R10+0x4], R3 ;
        /*0260*/                   EXIT ;
.L_x_22:
        /*0270*/                   BSYNC B0 ;
.L_x_21:
        /*0280*/                   RET.ABS.NODEC R20 0x0 ;
.L_x_26:
        /*0290*/                   BRA `(.L_x_26);
        /*02a0*/                   NOP;
        /*02b0*/                   NOP;
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
.L_x_41:


//--------------------- .text.restore_exec        --------------------------
	.section	.text.restore_exec,"ax",@progbits
	.sectionflags	@"SHF_BARRIERS=1"
	.sectioninfo	@"SHI_REGISTERS=24"
	.align	128
        .global         restore_exec
        .type           restore_exec,@function
        .size           restore_exec,(.L_x_42 - restore_exec)
restore_exec:
.text.restore_exec:
        /*0000*/                   BSSY B0, `(.L_x_27) ;
        /*0010*/                   BPT.TRAP 0x1 ;
        /*0020*/                   BPT.TRAP 0x1 ;
        /*0030*/                   S2R R0, SR_CTAID.X ;
        /*0040*/                   S2R R3, SR_CTAID.Y ;
        /*0050*/                   S2R R7, SR_CTAID.Z ;
        /*0060*/                   IMAD R0, R0, c[0x0][0x10], R3 ;
        /*0070*/                   IMAD R0, R0, c[0x0][0x14], R7 ;
        /*0080*/                   LEA R3, R0, 0x5, 0x1 ;
        /*0090*/                   IMAD.WIDE.U32 R4, R3, 0x4, R4 ;
        /*00a0*/                   LD.E R0, [R4] ;
        /*00b0*/                   ISETP.NE.AND P0, PT, R0, RZ, PT ;
        /*00c0*/              @!P0 EXIT ;
        /*00d0*/                   WARPSYNC 0xffffffff ;
        /*00e0*/                   BAR.SYNC.DEFER_BLOCKING 0x0 ;
        /*00f0*/                   ST.E [R4], RZ ;
        /*0100*/                   BPT.TRAP 0x1 ;
        /*0110*/                   BPT.TRAP 0x1 ;
        /*0120*/                   BSYNC B0 ;
.L_x_27:
        /*0130*/                   RET.ABS.NODEC R20 0x0 ;
.L_x_28:
        /*0140*/                   BRA `(.L_x_28);
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
.L_x_42:


//--------------------- .text.exit_if_idempotent  --------------------------
	.section	.text.exit_if_idempotent,"ax",@progbits
	.sectioninfo	@"SHI_REGISTERS=24"
	.align	128
        .global         exit_if_idempotent
        .type           exit_if_idempotent,@function
        .size           exit_if_idempotent,(.L_x_43 - exit_if_idempotent)
exit_if_idempotent:
.text.exit_if_idempotent:
        /*0000*/                   BSSY B0, `(.L_x_29) ;
        /*0010*/                   BPT.TRAP 0x1 ;
        /*0020*/                   BPT.TRAP 0x1 ;
        /*0030*/                   BPT.TRAP 0x1 ;
        /*0040*/                   BPT.TRAP 0x1 ;
        /*0050*/                   LD.E R0, [R4] ;
        /*0060*/                   ISETP.NE.AND P0, PT, R0, RZ, PT ;
        /*0070*/              @!P0 BRA `(.L_x_30) ;
        /*0080*/                   BPT.TRAP 0x1 ;
        /*0090*/                   BPT.TRAP 0x1 ;
        /*00a0*/                   BPT.TRAP 0x1 ;
        /*00b0*/                   BPT.TRAP 0x1 ;
        /*00c0*/                   S2R R0, SR_CTAID.X ;
        /*00d0*/                   S2R R3, SR_CTAID.Y ;
        /*00e0*/                   S2R R9, SR_CTAID.Z ;
        /*00f0*/                   ST.E.64 [R4+0x8], R6 ;
        /*0100*/                   IMAD R0, R0, c[0x0][0x10], R3 ;
        /*0110*/                   MOV R3, 0x1 ;
        /*0120*/                   IMAD R0, R0, c[0x0][0x14], R9 ;
        /*0130*/                   ST.E [R4], R3 ;
        /*0140*/                   LEA R9, R0, 0x5, 0x1 ;
        /*0150*/                   IMAD.WIDE.U32 R8, R9, 0x4, R4 ;
        /*0160*/                   ST.E [R8], R3 ;
        /*0170*/                   EXIT ;
.L_x_30:
        /*0180*/                   BPT.TRAP 0x1 ;
        /*0190*/                   BPT.TRAP 0x1 ;
        /*01a0*/                   BPT.TRAP 0x1 ;
        /*01b0*/                   BSYNC B0 ;
.L_x_29:
        /*01c0*/                   RET.ABS.NODEC R20 0x0 ;
.L_x_31:
        /*01d0*/                   BRA `(.L_x_31);
        /*01e0*/                   NOP;
        /*01f0*/                   NOP;
        /*0200*/                   NOP;
        /*0210*/                   NOP;
        /*0220*/                   NOP;
        /*0230*/                   NOP;
        /*0240*/                   NOP;
        /*0250*/                   NOP;
        /*0260*/                   NOP;
        /*0270*/                   NOP;
.L_x_43:


//--------------------- SYMBOLS --------------------------

	.type		vprintf,@function

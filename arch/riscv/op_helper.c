/*
 *  RISC-V emulation helpers
 *
 *  Author: Sagar Karandikar, sagark@eecs.berkeley.edu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include <stdlib.h>
#include "cpu.h"
#include "arch_callbacks.h"


int validate_priv(target_ulong priv)
{
    return priv == PRV_U || priv == PRV_S || priv == PRV_M;
}

static int validate_vm(target_ulong vm)
{
    return vm == VM_SV32 || vm == VM_SV39 || vm == VM_SV48 || vm == VM_MBARE;
}

void __attribute__ ((__noreturn__)) cpu_loop_exit_restore(CPUState *cpu, uintptr_t pc)
{
    TranslationBlock *tb;
    if (pc) {
        tb = tb_find_pc(pc);
        if(tb)
        {
            cpu_restore_state(cpu, tb, pc);
        }
    }
    longjmp(cpu->jmp_env, 1);
}

static inline uint64_t cpu_riscv_read_instret(CPUState *env)
{
    uint64_t retval = env->instructions_count_total_value;
    return retval;
}

/* Exceptions processing helpers */
static inline void __attribute__ ((__noreturn__)) do_raise_exception_err(CPUState *env,
                                          uint32_t exception, uintptr_t pc)
{
    env->exception_index = exception;
    cpu_loop_exit_restore(env, pc);
}

void helper_raise_exception(CPUState *env, uint32_t exception)
{
    do_raise_exception_err(env, exception, 0);
}

void helper_raise_exception_debug(CPUState *env)
{
    do_raise_exception_err(env, EXCP_DEBUG, 0);
}

void helper_raise_exception_mbadaddr(CPUState *env, uint32_t exception,
        target_ulong bad_pc) {
    env->badaddr = bad_pc;
    do_raise_exception_err(env, exception, 0);
}

void helper_wfi(CPUState *env)
{
    env->exception_index = EXCP_WFI;
    env->wfi = 1;
    cpu_loop_exit(env);
}

void helper_tlb_flush(CPUState *env);

static inline uint64_t get_minstret_current(CPUState *env)
{
    return cpu_riscv_read_instret(env) - env->minstret_snapshot + env->minstret_snapshot_offset;
}

static inline uint64_t get_mcycles_current(CPUState *env)
{
    return cpu_riscv_read_instret(env) - env->mcycle_snapshot + env->mcycle_snapshot_offset;
}

/*
 * Handle writes to CSRs and any resulting special behavior
 *
 * Adapted from Spike's processor_t::set_csr
 */
inline void csr_write_helper(CPUState *env, target_ulong val_to_write,
        target_ulong csrno)
{
    uint64_t delegable_ints = MIP_SSIP | MIP_STIP | MIP_SEIP | (1 << IRQ_COP);
    uint64_t all_ints = delegable_ints | MIP_MSIP | MIP_MTIP | MIP_MEIP;

    switch (csrno) {
    case CSR_FFLAGS:
        env->mstatus |= MSTATUS_FS | MSTATUS64_SD;
        env->fflags = val_to_write & (FSR_AEXC >> FSR_AEXC_SHIFT);
        break;
    case CSR_FRM:
        env->mstatus |= MSTATUS_FS | MSTATUS64_SD;
        env->frm = val_to_write & (FSR_RD >> FSR_RD_SHIFT);
        break;
    case CSR_FCSR:
        env->mstatus |= MSTATUS_FS | MSTATUS64_SD;
        env->fflags = (val_to_write & FSR_AEXC) >> FSR_AEXC_SHIFT;
        env->frm = (val_to_write & FSR_RD) >> FSR_RD_SHIFT;
        break;
    case CSR_MSTATUS: {
        target_ulong mstatus = env->mstatus;
        if ((val_to_write ^ mstatus) &
            (MSTATUS_VM | MSTATUS_MPP | MSTATUS_MPRV | MSTATUS_PUM |
             MSTATUS_MXR)) {
            helper_tlb_flush(env);
        }

        /* no extension support */
        target_ulong mask = MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_MIE
            | MSTATUS_MPIE | MSTATUS_SPP | MSTATUS_FS | MSTATUS_MPRV
            | MSTATUS_PUM | MSTATUS_MPP | MSTATUS_MXR;

        if (validate_vm(get_field(val_to_write, MSTATUS_VM))) {
            mask |= MSTATUS_VM;
        }

        mstatus = (mstatus & ~mask) | (val_to_write & mask);

        int dirty = (mstatus & MSTATUS_FS) == MSTATUS_FS;
        dirty |= (mstatus & MSTATUS_XS) == MSTATUS_XS;
        mstatus = set_field(mstatus, MSTATUS64_SD, dirty);
        env->mstatus = mstatus;
        break;
    }
    case CSR_MIP: {
        target_ulong mask = MIP_SSIP | MIP_STIP;
        tlib_set_mip((env->mip & ~mask) | (val_to_write & mask));
        break;
    }
    case CSR_MIE: {
        env->mie = (env->mie & ~all_ints) |
            (val_to_write & all_ints);
        break;
    }
    case CSR_MIDELEG:
        env->mideleg = (env->mideleg & ~delegable_ints)
                                | (val_to_write & delegable_ints);
        break;
    case CSR_MEDELEG: {
        target_ulong mask = 0;
        mask |= 1ULL << (RISCV_EXCP_INST_ADDR_MIS);
        mask |= 1ULL << (RISCV_EXCP_INST_ACCESS_FAULT);
        mask |= 1ULL << (RISCV_EXCP_ILLEGAL_INST);
        mask |= 1ULL << (RISCV_EXCP_BREAKPOINT);
        mask |= 1ULL << (RISCV_EXCP_LOAD_ADDR_MIS);
        mask |= 1ULL << (RISCV_EXCP_LOAD_ACCESS_FAULT);
        mask |= 1ULL << (RISCV_EXCP_STORE_AMO_ADDR_MIS);
        mask |= 1ULL << (RISCV_EXCP_STORE_AMO_ACCESS_FAULT);
        mask |= 1ULL << (RISCV_EXCP_U_ECALL);
        mask |= 1ULL << (RISCV_EXCP_S_ECALL);
        mask |= 1ULL << (RISCV_EXCP_H_ECALL);
        mask |= 1ULL << (RISCV_EXCP_M_ECALL);
        env->medeleg = (env->medeleg & ~mask)
                                | (val_to_write & mask);
        break;
    }
    case CSR_MUCOUNTEREN:
        env->mucounteren = val_to_write;
        break;
    case CSR_MSCOUNTEREN:
        env->mscounteren = val_to_write;
        break;
    case CSR_SSTATUS: {
        target_ulong ms = env->mstatus;
        target_ulong mask = SSTATUS_SIE | SSTATUS_SPIE | SSTATUS_SPP
                            | SSTATUS_FS | SSTATUS_XS | SSTATUS_PUM;
        ms = (ms & ~mask) | (val_to_write & mask);
        csr_write_helper(env, ms, CSR_MSTATUS);
        break;
    }
    case CSR_SIP: {
        target_ulong next_mip = (env->mip & ~env->mideleg)
                                | (val_to_write & env->mideleg);
        csr_write_helper(env, next_mip, CSR_MIP);
        /* note: stw_phys should be done by the call to set MIP if necessary, */
        /* so we don't do it here */
        break;
    }
    case CSR_SIE: {
        target_ulong next_mie = (env->mie & ~env->mideleg)
                                | (val_to_write & env->mideleg);
        csr_write_helper(env, next_mie, CSR_MIE);
        break;
    }
    case CSR_SPTBR: {
        env->sptbr = val_to_write & (((target_ulong)1 <<
                              (TARGET_PHYS_ADDR_SPACE_BITS - PGSHIFT)) - 1);
        break;
    }
    case CSR_SEPC:
        env->sepc = val_to_write;
        break;
    case CSR_STVEC:
        env->stvec = val_to_write >> 2 << 2;
        break;
    case CSR_SSCRATCH:
        env->sscratch = val_to_write;
        break;
    case CSR_SCAUSE:
        env->scause = val_to_write;
        break;
    case CSR_SBADADDR:
        env->sbadaddr = val_to_write;
        break;
    case CSR_MEPC:
        env->mepc = val_to_write;
        break;
    case CSR_MTVEC:
        env->mtvec = val_to_write >> 2 << 2;
        break;
    case CSR_MSCRATCH:
        env->mscratch = val_to_write;
        break;
    case CSR_MCAUSE:
        env->mcause = val_to_write;
        break;
    case CSR_MBADADDR:
        env->mbadaddr = val_to_write;
        break;
    case CSR_MISA: {
        if (!(val_to_write & (1L << ('F' - 'A')))) {
            val_to_write &= ~(1L << ('D' - 'A'));
        }

        // allow MAFDC bits in MISA to be modified
        target_ulong mask = 0;
        mask |= 1L << ('M' - 'A');
        mask |= 1L << ('A' - 'A');
        mask |= 1L << ('F' - 'A');
        mask |= 1L << ('D' - 'A');
        mask |= 1L << ('C' - 'A');
        mask &= env->max_isa;

        env->misa = (val_to_write & mask) | (env->misa & ~mask);
        break;
    }
    case CSR_TSELECT:
        // TSELECT is hardwired in this implementation
        break;
    case CSR_TDATA1:
        tlib_abort("CSR_TDATA1 write not implemented");
        break;
    case CSR_TDATA2:
        tlib_abort("CSR_TDATA2 write not implemented");
        break;
    case CSR_DCSR:
        tlib_abort("CSR_DCSR write not implemented");
        break;
    case CSR_MCYCLE:
#if defined(TARGET_RISCV32)
        env->mcycle_snapshot_offset = (get_mcycles_current(env) & 0xFFFFFFFF00000000) | val_to_write;
#else
        env->mcycle_snapshot_offset = get_mcycles_current(env)
#endif
        env->mcycle_snapshot = cpu_riscv_read_instret(env);
        break;
    case CSR_MCYCLEH:
#if defined(TARGET_RISCV32)
        env->mcycle_snapshot_offset = (get_mcycles_current(env) & 0x00000000FFFFFFFF) | ((uint64_t)val_to_write << 32);
        env->mcycle_snapshot = cpu_riscv_read_instret(env);
#endif
        break;
    case CSR_MINSTRET:
#if defined(TARGET_RISCV32)
        env->minstret_snapshot_offset = (get_minstret_current(env) & 0xFFFFFFFF00000000) | val_to_write;
#else
        env->minstret_snapshot_offset = get_minstret_current(env)
#endif
        env->minstret_snapshot = cpu_riscv_read_instret(env);
        break;
    case CSR_MINSTRETH:
#if defined(TARGET_RISCV32)
        env->minstret_snapshot_offset = (get_minstret_current(env) & 0x00000000FFFFFFFF) | ((uint64_t)val_to_write << 32);
        env->minstret_snapshot = cpu_riscv_read_instret(env);
#endif
        break;
    default:
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }
}

/*
 * Handle reads to CSRs and any resulting special behavior
 *
 * Adapted from Spike's processor_t::get_csr
 */
static inline target_ulong csr_read_helper(CPUState *env, target_ulong csrno)
{
    target_ulong ctr_en = env->priv == PRV_U ? env->mucounteren :
                   env->priv == PRV_S ? env->mscounteren : -1U;
    target_ulong ctr_ok = (ctr_en >> (csrno & 31)) & 1;

    if (ctr_ok) {
        if (csrno >= CSR_HPMCOUNTER3 && csrno <= CSR_HPMCOUNTER31) {
            return 0;
        }
#if defined(TARGET_RISCV32)
        if (csrno >= CSR_HPMCOUNTER3H && csrno <= CSR_HPMCOUNTER31H) {
            return 0;
        }
#endif
    }
    if (csrno >= CSR_MHPMCOUNTER3 && csrno <= CSR_MHPMCOUNTER31) {
        return 0;
    }
#if defined(TARGET_RISCV32)
    if (csrno >= CSR_MHPMCOUNTER3 && csrno <= CSR_MHPMCOUNTER31) {
        return 0;
    }
#endif
    if (csrno >= CSR_MHPMEVENT3 && csrno <= CSR_MHPMEVENT31) {
        return 0;
    }

    switch (csrno) {
    case CSR_FFLAGS:
        return env->fflags;
    case CSR_FRM:
        return env->frm;
    case CSR_FCSR:
        return env->fflags << FSR_AEXC_SHIFT |
               env->frm << FSR_RD_SHIFT;
        /* TODO fix TIME, INSTRET, CYCLE in user mode */
        /* 32-bit TIMEH, CYCLEH, INSTRETH, other H stuff */
    case CSR_INSTRET:
    case CSR_CYCLE:
        if (ctr_ok) {
            return cpu_riscv_read_instret(env);
        }
        break;
    case CSR_MINSTRET:
        return get_minstret_current(env);
    case CSR_MCYCLE:
        return get_mcycles_current(env);
    case CSR_MINSTRETH:
#if defined(TARGET_RISCV32)
        return get_minstret_current(env) >> 32;
#endif
        break;
    case CSR_MCYCLEH:
#if defined(TARGET_RISCV32)
        return get_mcycles_current(env) >> 32;
#endif
        break;
    case CSR_MUCOUNTEREN:
        return env->mucounteren;
    case CSR_MSCOUNTEREN:
        return env->mscounteren;
    case CSR_SSTATUS: {
        target_ulong mask = SSTATUS_SIE | SSTATUS_SPIE | SSTATUS_SPP
                            | SSTATUS_FS | SSTATUS_XS | SSTATUS_PUM;
        target_ulong sstatus = env->mstatus & mask;
        if ((sstatus & SSTATUS_FS) == SSTATUS_FS ||
                (sstatus & SSTATUS_XS) == SSTATUS_XS) {
            sstatus |= SSTATUS64_SD;
        }
        return sstatus;
    }
    case CSR_SIP:
        return env->mip & env->mideleg;
    case CSR_SIE:
        return env->mie & env->mideleg;
    case CSR_SEPC:
        return env->sepc;
    case CSR_SBADADDR:
        return env->sbadaddr;
    case CSR_STVEC:
        return env->stvec;
    case CSR_SCAUSE:
        return env->scause;
    case CSR_SPTBR:
        return env->sptbr;
    case CSR_SSCRATCH:
        return env->sscratch;
    case CSR_MSTATUS:
        return env->mstatus;
    case CSR_MIP:
        return env->mip;
    case CSR_MIE:
        return env->mie;
    case CSR_MEPC:
        return env->mepc;
    case CSR_MSCRATCH:
        return env->mscratch;
    case CSR_MCAUSE:
        return env->mcause;
    case CSR_MBADADDR:
        return env->mbadaddr;
    case CSR_MISA:
        return env->misa;
    case CSR_MARCHID:
        return 0; /* as spike does */
    case CSR_MIMPID:
        return 0; /* as spike does */
    case CSR_MVENDORID:
        return 0; /* as spike does */
    case CSR_MHARTID:
        return 0;
    case CSR_MTVEC:
        return env->mtvec;
    case CSR_MEDELEG:
        return env->medeleg;
    case CSR_MIDELEG:
        return env->mideleg;
    case CSR_TSELECT:
        // indicate only usable in debug mode (which we don't have)
        // i.e. software can't use it
        // see: https://dev.sifive.com/documentation/risc-v-external-debug-support-0-11/
        return (1L << (TARGET_LONG_BITS - 5));
    case CSR_TDATA1:
        tlib_abort("CSR_TDATA1 read not implemented");
        break;
    case CSR_TDATA2:
        tlib_abort("CSR_TDATA2 read not implemented");
        break;
    case CSR_TDATA3:
        tlib_abort("CSR_TDATA3 read not implemented");
        break;
    case CSR_DCSR:
        tlib_abort("CSR_DCSR read not implemented");
        break;
    }
    /* used by e.g. MTIME read */
    helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    return 0;
}

/*
 * Check that CSR access is allowed.
 *
 * Adapted from Spike's decode.h:validate_csr
 */
void validate_csr(CPUState *env, uint64_t which, uint64_t write)
{
    unsigned csr_priv = get_field((which), 0x300);
    unsigned csr_read_only = get_field((which), 0xC00) == 3;
    if (((write) && csr_read_only) || (env->priv < csr_priv)) {
        do_raise_exception_err(env, RISCV_EXCP_ILLEGAL_INST, env->pc);
    }
}

target_ulong helper_csrrw(CPUState *env, target_ulong src,
        target_ulong csr)
{
    validate_csr(env, csr, 1);
    uint64_t csr_backup = csr_read_helper(env, csr);
    csr_write_helper(env, src, csr);
    return csr_backup;
}

target_ulong helper_csrrs(CPUState *env, target_ulong src,
        target_ulong csr, target_ulong rs1_pass)
{
    validate_csr(env, csr, rs1_pass != 0);
    uint64_t csr_backup = csr_read_helper(env, csr);
    if (rs1_pass != 0) {
        csr_write_helper(env, src | csr_backup, csr);
    }
    return csr_backup;
}

target_ulong helper_csrrc(CPUState *env, target_ulong src,
        target_ulong csr, target_ulong rs1_pass)
{
    validate_csr(env, csr, rs1_pass != 0);
    uint64_t csr_backup = csr_read_helper(env, csr);
    if (rs1_pass != 0) {
        csr_write_helper(env, (~src) & csr_backup, csr);
    }
    return csr_backup;
}


void set_privilege(CPUState *env, target_ulong newpriv)
{
    if (!(newpriv <= PRV_M)) {
        tlib_abort("INVALID PRIV SET");
    }
    if (newpriv == PRV_H) {
        newpriv = PRV_U;
    }
    helper_tlb_flush(env);
    env->priv = newpriv;
}

target_ulong helper_sret(CPUState *env, target_ulong cpu_pc_deb)
{
    if (!(env->priv >= PRV_S)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }

    target_ulong retpc = env->sepc;
    if (retpc & 0x3) {
        helper_raise_exception(env, RISCV_EXCP_INST_ADDR_MIS);
    }

    target_ulong mstatus = env->mstatus;
    target_ulong prev_priv = get_field(mstatus, MSTATUS_SPP);
    mstatus = set_field(mstatus, MSTATUS_UIE << prev_priv,
                        get_field(mstatus, MSTATUS_SPIE));
    mstatus = set_field(mstatus, MSTATUS_SPIE, 0);
    mstatus = set_field(mstatus, MSTATUS_SPP, PRV_U);
    set_privilege(env, prev_priv);
    csr_write_helper(env, mstatus, CSR_MSTATUS);

    return retpc;
}

target_ulong helper_mret(CPUState *env, target_ulong cpu_pc_deb)
{
    if (!(env->priv >= PRV_M)) {
        helper_raise_exception(env, RISCV_EXCP_ILLEGAL_INST);
    }

    target_ulong retpc = env->mepc;
    target_ulong mstatus = env->mstatus;
    target_ulong prev_priv = get_field(mstatus, MSTATUS_MPP);
    mstatus = set_field(mstatus, MSTATUS_UIE << prev_priv,
                        get_field(mstatus, MSTATUS_MPIE));
    mstatus = set_field(mstatus, MSTATUS_MPIE, 0);
    mstatus = set_field(mstatus, MSTATUS_MPP, PRV_U);
    set_privilege(env, prev_priv);
    csr_write_helper(env, mstatus, CSR_MSTATUS);

    return retpc;
}


void helper_fence_i(CPUState *env)
{
    /* Flush QEMU's TLB */
    tlb_flush(env, 1);
    /* ARM port seems to not know if this is okay inside a TB
       But we need to do it */
    tb_flush(env);
}

void helper_tlb_flush(CPUState *env)
{
    tlb_flush(env, 1);
}

extern CPUState *env;
#include "dyngen-exec.h"
#include "softmmu_exec.h"

/* called to fill tlb */
void tlb_fill(CPUState *env, target_ulong addr, int is_write, int mmu_idx,
              void* retaddr)
{
    int ret;
    ret = cpu_riscv_handle_mmu_fault(env, addr, is_write, mmu_idx);
    if (ret == TRANSLATE_FAIL) {
        do_raise_exception_err(env, env->exception_index, 0);
    }
}

/*
 * %CopyrightBegin%
 *
 * Copyright Ericsson AB 2020-2022. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * %CopyrightEnd%
 */

#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <algorithm>
#include <cmath>

#ifndef ASMJIT_ASMJIT_H_INCLUDED
#    include <asmjit/asmjit.hpp>
#endif

extern "C"
{
#ifdef HAVE_CONFIG_H
#    include "config.h"
#endif

#include "sys.h"
#include "erl_vm.h"
#include "global.h"
#include "beam_catches.h"
#include "big.h"

#include "beam_asm.h"
}

#include "beam_jit_common.hpp"

/* On Windows, the min and max macros may be defined. */
#undef min
#undef max

using namespace asmjit;

class BeamAssembler : public ErrorHandler {
protected:
    /* Holds code and relocation information. */
    CodeHolder code;

    /* TODO: Want to change this to x86::Builder in order to be able to patch
     * the correct I into the code after code generation */
    x86::Assembler a;

    FileLogger logger;

    Section *rodata = nullptr;

    /* * * * * * * * * */

    /* Points at x_reg_array inside an ErtsSchedulerRegisters struct, allowing
     * the aux_regs field to be addressed with an 8-bit displacement. */
    const x86::Gp registers = x86::rbx;

#ifdef NATIVE_ERLANG_STACK
    /* The Erlang stack pointer, note that it uses RSP and is therefore invalid
     * when running on the runtime stack. */
    const x86::Gp E = x86::rsp;

#    ifdef ERLANG_FRAME_POINTERS
    /* Current frame pointer, used when we emit native stack frames (e.g. to
     * better support `perf`). */
    const x86::Gp frame_pointer = x86::rbp;
#    endif

    /* When we're not using frame pointers, we can keep the Erlang stack in
     * RBP when running on the runtime stack, which is slightly faster than
     * reading and writing from c_p->stop. */
    const x86::Gp E_saved = x86::rbp;
#else
    const x86::Gp E = x86::rbp;
#endif

    const x86::Gp c_p = x86::r13;
    const x86::Gp FCALLS = x86::r14;
    const x86::Gp HTOP = x86::r15;

    /* Local copy of the active code index.
     *
     * This is set to ERTS_SAVE_CALLS_CODE_IX when save_calls is active, which
     * routes us to a common handler routine that calls save_calls before
     * jumping to the actual code. */
    const x86::Gp active_code_ix = x86::r12;

#ifdef ERTS_MSACC_EXTENDED_STATES
    const x86::Mem erts_msacc_cache = getSchedulerRegRef(
            offsetof(ErtsSchedulerRegisters, aux_regs.d.erts_msacc_cache));
#endif

    /* * * * * * * * * */
#ifdef WIN32
    const x86::Gp ARG1 = x86::rcx;
    const x86::Gp ARG2 = x86::rdx;
    const x86::Gp ARG3 = x86::r8;
    const x86::Gp ARG4 = x86::r9;
    const x86::Gp ARG5 = x86::r10;
    const x86::Gp ARG6 = x86::r11;

    const x86::Gp ARG1d = x86::ecx;
    const x86::Gp ARG2d = x86::edx;
    const x86::Gp ARG3d = x86::r8d;
    const x86::Gp ARG4d = x86::r9d;
    const x86::Gp ARG5d = x86::r10d;
    const x86::Gp ARG6d = x86::r11d;
#else
    const x86::Gp ARG1 = x86::rdi;
    const x86::Gp ARG2 = x86::rsi;
    const x86::Gp ARG3 = x86::rdx;
    const x86::Gp ARG4 = x86::rcx;
    const x86::Gp ARG5 = x86::r8;
    const x86::Gp ARG6 = x86::r9;

    const x86::Gp ARG1d = x86::edi;
    const x86::Gp ARG2d = x86::esi;
    const x86::Gp ARG3d = x86::edx;
    const x86::Gp ARG4d = x86::ecx;
    const x86::Gp ARG5d = x86::r8d;
    const x86::Gp ARG6d = x86::r9d;
#endif

    const x86::Gp RET = x86::rax;
    const x86::Gp RETd = x86::eax;
    const x86::Gp RETb = x86::al;

    const x86::Mem TMP_MEM1q = getSchedulerRegRef(
            offsetof(ErtsSchedulerRegisters, aux_regs.d.TMP_MEM[0]));
    const x86::Mem TMP_MEM2q = getSchedulerRegRef(
            offsetof(ErtsSchedulerRegisters, aux_regs.d.TMP_MEM[1]));
    const x86::Mem TMP_MEM3q = getSchedulerRegRef(
            offsetof(ErtsSchedulerRegisters, aux_regs.d.TMP_MEM[2]));
    const x86::Mem TMP_MEM4q = getSchedulerRegRef(
            offsetof(ErtsSchedulerRegisters, aux_regs.d.TMP_MEM[3]));
    const x86::Mem TMP_MEM5q = getSchedulerRegRef(
            offsetof(ErtsSchedulerRegisters, aux_regs.d.TMP_MEM[4]));

    const x86::Mem TMP_MEM1d = getSchedulerRegRef(
            offsetof(ErtsSchedulerRegisters, aux_regs.d.TMP_MEM[0]),
            sizeof(Uint32));
    const x86::Mem TMP_MEM2d = getSchedulerRegRef(
            offsetof(ErtsSchedulerRegisters, aux_regs.d.TMP_MEM[1]),
            sizeof(Uint32));
    const x86::Mem TMP_MEM3d = getSchedulerRegRef(
            offsetof(ErtsSchedulerRegisters, aux_regs.d.TMP_MEM[2]),
            sizeof(Uint32));
    const x86::Mem TMP_MEM4d = getSchedulerRegRef(
            offsetof(ErtsSchedulerRegisters, aux_regs.d.TMP_MEM[3]),
            sizeof(Uint32));
    const x86::Mem TMP_MEM5d = getSchedulerRegRef(
            offsetof(ErtsSchedulerRegisters, aux_regs.d.TMP_MEM[4]),
            sizeof(Uint32));

    enum Distance { dShort, dLong };

public:
    static bool hasCpuFeature(uint32_t featureId);

    BeamAssembler();
    BeamAssembler(const std::string &log);

    ~BeamAssembler();

    void *getBaseAddress();
    size_t getOffset();

protected:
    void _codegen(JitAllocator *allocator,
                  const void **executable_ptr,
                  void **writable_ptr);

    void *getCode(Label label);
    byte *getCode(char *labelName);

    void handleError(Error err, const char *message, BaseEmitter *origin);

    constexpr x86::Mem getRuntimeStackRef() const {
        int base = offsetof(ErtsSchedulerRegisters, aux_regs.d.runtime_stack);

        return getSchedulerRegRef(base);
    }

#if !defined(NATIVE_ERLANG_STACK)
#    ifdef JIT_HARD_DEBUG
    constexpr x86::Mem getInitialSPRef() const {
        int base = offsetof(ErtsSchedulerRegisters, initial_sp);

        return getSchedulerRegRef(base);
    }
#    endif

    constexpr x86::Mem getCPRef() const {
        return x86::qword_ptr(E);
    }
#endif

    constexpr x86::Mem getSchedulerRegRef(int offset,
                                          size_t size = sizeof(UWord)) const {
        const int x_reg_offset =
                offsetof(ErtsSchedulerRegisters, x_reg_array.d);

        /* The entire aux_reg field should be addressable with an 8-bit
         * displacement. */
        ERTS_CT_ASSERT(x_reg_offset <= 128);

        return x86::Mem(registers, offset - x_reg_offset, size);
    }

    constexpr x86::Mem getFRef(int index, size_t size = sizeof(UWord)) const {
        int base = offsetof(ErtsSchedulerRegisters, f_reg_array.d);
        int offset = index * sizeof(FloatDef);

        ASSERT(index >= 0 && index <= 1023);
        return getSchedulerRegRef(base + offset, size);
    }

    constexpr x86::Mem getXRef(int index, size_t size = sizeof(UWord)) const {
        int base = offsetof(ErtsSchedulerRegisters, x_reg_array.d);
        int offset = index * sizeof(Eterm);

        ASSERT(index >= 0 && index < ERTS_X_REGS_ALLOCATED);
        return getSchedulerRegRef(base + offset, size);
    }

    constexpr x86::Mem getYRef(int index, size_t size = sizeof(UWord)) const {
        ASSERT(index >= 0 && index <= 1023);

#ifdef NATIVE_ERLANG_STACK
        return x86::Mem(E, index * sizeof(Eterm), size);
#else
        return x86::Mem(E, (index + CP_SIZE) * sizeof(Eterm), size);
#endif
    }

    constexpr x86::Mem getCARRef(x86::Gp Src,
                                 size_t size = sizeof(UWord)) const {
        return x86::Mem(Src, -TAG_PRIMARY_LIST, size);
    }

    constexpr x86::Mem getCDRRef(x86::Gp Src,
                                 size_t size = sizeof(UWord)) const {
        return x86::Mem(Src, -TAG_PRIMARY_LIST + sizeof(Eterm), size);
    }

    void align_erlang_cp() {
        /* Align so that the current address forms a valid CP. */
        ERTS_CT_ASSERT(_CPMASK == 3);
        a.align(AlignMode::kCode, 4);
        ASSERT(is_CP(a.offset()));
    }

    void load_x_reg_array(x86::Gp reg) {
        /* By definition. */
        a.mov(reg, registers);
    }

    void load_erl_bits_state(x86::Gp reg) {
        int offset =
                offsetof(ErtsSchedulerRegisters, aux_regs.d.erl_bits_state);

        a.lea(reg, getSchedulerRegRef(offset));
    }

    /* Ensure that the Erlang stack is used and the redzone is unused.
     * We combine those test to minimize the number of instructions.
     */
    void emit_assert_redzone_unused() {
#ifdef JIT_HARD_DEBUG
        const int REDZONE_BYTES = S_REDZONE * sizeof(Eterm);
        Label ok = a.newLabel(), crash = a.newLabel();

        /* We modify the stack pointer to avoid spilling into a register,
         * TMP_MEM, or using the stack. */
        a.sub(E, imm(REDZONE_BYTES));
        a.cmp(HTOP, E);
        a.short_().ja(crash);
        a.cmp(E, x86::qword_ptr(c_p, offsetof(Process, hend)));
        a.short_().jle(ok);

        a.bind(crash);
        comment("Redzone touched");
        a.ud2();

        a.bind(ok);
        a.add(E, imm(REDZONE_BYTES));
#endif
    }

    /*
     * Calls an Erlang function.
     */
    template<typename Any>
    void erlang_call(Any Target, const x86::Gp &spill) {
#ifdef NATIVE_ERLANG_STACK
        /* We use the Erlang stack as the native stack. We can use a
         * native `call` instruction. */
        emit_assert_redzone_unused();
        aligned_call(Target);
#else
        Label next = a.newLabel();

        /* Save the return CP on the stack. */
        a.lea(spill, x86::qword_ptr(next));
        a.mov(getCPRef(), spill);

        a.jmp(Target);

        /* Need to align this label in order for it to be recognized as
         * is_CP. */
        align_erlang_cp();
        a.bind(next);
#endif
    }

    /*
     * Calls the given address in shared fragment, ensuring that the
     * redzone is unused and that the return address forms a valid
     * CP.
     */
    template<typename Any>
    void fragment_call(Any Target) {
        emit_assert_redzone_unused();

#if defined(JIT_HARD_DEBUG) && !defined(NATIVE_ERLANG_STACK)
        /* Verify that the stack has not grown. */
        Label next = a.newLabel();
        a.cmp(x86::rsp, getInitialSPRef());
        a.short_().je(next);
        comment("The stack has grown");
        a.ud2();
        a.bind(next);
#endif

        aligned_call(Target);
    }

    /*
     * Calls the given function pointer. In a debug build with
     * JIT_HARD_DEBUG defined, it will be enforced that the redzone is
     * unused.
     *
     * The return will NOT be aligned, and thus will not form a valid
     * CP. That means that call code must not scan the stack in any
     * way. That means, for example, that the called code must not
     * throw an exception, do a garbage collection, or cause a context
     * switch.
     */
    void safe_fragment_call(void (*Target)()) {
        emit_assert_redzone_unused();
        a.call(imm(Target));
    }

    template<typename FuncPtr>
    void aligned_call(FuncPtr(*target)) {
        /* Calls to absolute addresses (encoded in the address table) are
         * always 6 bytes long. */
        aligned_call(imm(target), 6);
    }

    void aligned_call(Label target) {
        /* Relative calls are always 5 bytes long. */
        aligned_call(target, 5);
    }

    template<typename OperandType>
    void aligned_call(OperandType target) {
        /* Other calls are variable size. While it would be nice to use this
         * method for pointer/label calls too, `asmjit` writes relocations into
         * the code buffer itself and overwriting them causes all kinds of
         * havoc. */
        size_t call_offset, call_size;

        call_offset = a.offset();
        a.call(target);

        call_size = a.offset() - call_offset;
        a.setOffset(call_offset);

        aligned_call(target, call_size);
    }

    /* Calls the given address, ensuring that the return address forms a valid
     * CP. */
    template<typename OperandType>
    void aligned_call(OperandType target, size_t size) {
        /* The return address must be 4-byte aligned to form a valid CP, so
         * we'll align according to the size of the call instruction. */
        ssize_t next_address = (a.offset() + size);

        ERTS_CT_ASSERT(_CPMASK == 3);
        if (next_address % 4) {
            ssize_t nop_count = 4 - next_address % 4;

            a.embed(nops[nop_count - 1], nop_count);
        }

#ifdef JIT_HARD_DEBUG
        /* TODO: When frame pointers are in place, assert (at runtime) that the
         * destination has a `push rbp; mov rbp, rsp` sequence. */
#endif

        a.call(target);
        ASSERT(is_CP(a.offset()));
    }

    /* Canned instruction sequences for multi-byte NOPs */
    static const uint8_t *nops[3];
    static const uint8_t nop1[1];
    static const uint8_t nop2[2];
    static const uint8_t nop3[3];

    void runtime_call(x86::Gp func, unsigned args) {
        ASSERT(args < 5);

        emit_assert_runtime_stack();

#ifdef WIN32
        a.sub(x86::rsp, imm(4 * sizeof(UWord)));
        a.call(func);
        a.add(x86::rsp, imm(4 * sizeof(UWord)));
#else
        a.call(func);
#endif
    }

    template<typename T>
    struct function_arity;
    template<typename T, typename... Args>
    struct function_arity<T(Args...)>
            : std::integral_constant<int, sizeof...(Args)> {};

    template<int expected_arity, typename T>
    void runtime_call(T(*func)) {
        static_assert(expected_arity == function_arity<T>());

        emit_assert_runtime_stack();

#ifdef WIN32
        unsigned pushed;
        switch (expected_arity) {
        case 6:
        case 5:
            /* We push ARG6 to keep the stack aligned even when we only have 5
             * arguments. It does no harm, and is slightly more compact than
             * sub/push/sub. */
            a.push(ARG6);
            a.push(ARG5);
            a.sub(x86::rsp, imm(4 * sizeof(UWord)));
            pushed = 6;
            break;
        default:
            a.sub(x86::rsp, imm(4 * sizeof(UWord)));
            pushed = 4;
        }

#endif

        a.call(imm(func));

#ifdef WIN32
        a.add(x86::rsp, imm(pushed * sizeof(UWord)));
#endif
    }

    template<typename T>
    void abs_jmp(T(*addr)) {
        a.jmp(imm(addr));
    }

    /* Explicitly position-independent absolute jump, for use in fragments that
     * need to be memcpy'd for performance reasons (e.g. NIF stubs) */
    template<typename T>
    void pic_jmp(T(*addr)) {
        a.mov(ARG6, imm(addr));
        a.jmp(ARG6);
    }

    constexpr x86::Mem getArgRef(const ArgRegister &arg,
                                 size_t size = sizeof(UWord)) const {
        if (arg.isXRegister()) {
            return getXRef(arg.as<ArgXRegister>().get(), size);
        } else if (arg.isYRegister()) {
            return getYRef(arg.as<ArgYRegister>().get(), size);
        }

        return getFRef(arg.as<ArgFRegister>().get(), size);
    }

    /* Returns the current code address for the `Export` or `ErlFunEntry` in
     * `Src`.
     *
     * Export tracing, save_calls, etc are implemented by shared fragments that
     * assume that the respective entry is in RET, so we have to copy it over
     * if it isn't already. */
    x86::Mem emit_setup_dispatchable_call(const x86::Gp &Src) {
        return emit_setup_dispatchable_call(Src, active_code_ix);
    }

    x86::Mem emit_setup_dispatchable_call(const x86::Gp &Src,
                                          const x86::Gp &CodeIndex) {
        if (RET != Src) {
            a.mov(RET, Src);
        }

        ERTS_CT_ASSERT(offsetof(ErlFunEntry, dispatch) == 0);
        ERTS_CT_ASSERT(offsetof(Export, dispatch) == 0);

        return x86::qword_ptr(RET,
                              CodeIndex,
                              3,
                              offsetof(ErtsDispatchable, addresses));
    }

    void emit_assert_runtime_stack() {
#ifdef JIT_HARD_DEBUG
        Label crash = a.newLabel(), next = a.newLabel();

#    ifdef NATIVE_ERLANG_STACK
        /* Ensure that we are using the runtime stack. */
        int end_offs, start_offs;

        end_offs = offsetof(ErtsSchedulerRegisters, runtime_stack_end);
        start_offs = offsetof(ErtsSchedulerRegisters, runtime_stack_start);

        a.cmp(E, getSchedulerRegRef(end_offs));
        a.short_().jbe(crash);
        a.cmp(E, getSchedulerRegRef(start_offs));
        a.short_().ja(crash);
#    endif

        /* Are we 16-byte aligned? */
        a.test(x86::rsp, (16 - 1));
        a.short_().je(next);

        a.bind(crash);
        comment("Runtime stack is corrupt");
        a.ud2();

        a.bind(next);
#endif
    }

    void emit_assert_erlang_stack() {
#ifdef JIT_HARD_DEBUG
        Label crash = a.newLabel(), next = a.newLabel();

        /* Are we term-aligned? */
        a.test(E, imm(sizeof(Eterm) - 1));
        a.short_().jne(crash);

        a.cmp(E, x86::qword_ptr(c_p, offsetof(Process, heap)));
        a.short_().jl(crash);
        a.cmp(E, x86::qword_ptr(c_p, offsetof(Process, hend)));
        a.short_().jle(next);

        a.bind(crash);
        comment("Erlang stack is corrupt");
        a.ud2();
        a.bind(next);
#endif
    }

    enum Update : int {
        eStack = (1 << 0),
        eHeap = (1 << 1),
        eReductions = (1 << 2),
        eCodeIndex = (1 << 3)
    };

    void emit_enter_frame() {
#ifdef NATIVE_ERLANG_STACK
        if (ERTS_UNLIKELY(erts_frame_layout == ERTS_FRAME_LAYOUT_FP_RA)) {
#    ifdef ERLANG_FRAME_POINTERS
            a.push(frame_pointer);
            a.mov(frame_pointer, E);
#    endif
        } else {
            ASSERT(erts_frame_layout == ERTS_FRAME_LAYOUT_RA);
        }
#endif
    }

    void emit_leave_frame() {
#ifdef NATIVE_ERLANG_STACK
        if (ERTS_UNLIKELY(erts_frame_layout == ERTS_FRAME_LAYOUT_FP_RA)) {
            a.leave();
        } else {
            ASSERT(erts_frame_layout == ERTS_FRAME_LAYOUT_RA);
        }
#endif
    }

    void emit_unwind_frame() {
        emit_assert_erlang_stack();

        emit_leave_frame();
        a.add(x86::rsp, imm(sizeof(UWord)));
    }

    template<int Spec = 0>
    void emit_enter_runtime() {
        emit_assert_erlang_stack();

        ERTS_CT_ASSERT((Spec & (Update::eReductions | Update::eStack |
                                Update::eHeap)) == Spec);

        if (ERTS_LIKELY(erts_frame_layout == ERTS_FRAME_LAYOUT_RA)) {
            if ((Spec & (Update::eHeap | Update::eStack)) ==
                (Update::eHeap | Update::eStack)) {
                /* To update both heap and stack we use sse instructions like
                 * gcc -O3 does. Basically it is this function run through
                 * gcc -O3:
                 *
                 *    struct a { long a; long b; long c; };
                 *    void test(long a, long b, long c, struct a *s) {
                 *      s->a = a;
                 *      s->b = b;
                 *      s->c = c;
                 *    } */
                ERTS_CT_ASSERT((offsetof(Process, stop) -
                                offsetof(Process, htop)) == sizeof(Eterm *));
                a.movq(x86::xmm0, HTOP);
                a.movq(x86::xmm1, E);
                a.punpcklqdq(x86::xmm0, x86::xmm1);
                a.movups(x86::xmmword_ptr(c_p, offsetof(Process, htop)),
                         x86::xmm0);
            } else if (Spec & Update::eHeap) {
                a.mov(x86::qword_ptr(c_p, offsetof(Process, htop)), HTOP);
            } else if (Spec & Update::eStack) {
                a.mov(x86::qword_ptr(c_p, offsetof(Process, stop)), E);
            }

#ifdef NATIVE_ERLANG_STACK
            if (!(Spec & Update::eStack)) {
                a.mov(E_saved, E);
            }
#endif
        } else {
#ifdef ERLANG_FRAME_POINTERS
            ASSERT(erts_frame_layout == ERTS_FRAME_LAYOUT_FP_RA);

            if (Spec & Update::eStack) {
                ERTS_CT_ASSERT((offsetof(Process, frame_pointer) -
                                offsetof(Process, stop)) == sizeof(Eterm *));
                a.movq(x86::xmm0, E);
                a.movq(x86::xmm1, frame_pointer);
                a.punpcklqdq(x86::xmm0, x86::xmm1);
                a.movups(x86::xmmword_ptr(c_p, offsetof(Process, stop)),
                         x86::xmm0);
            } else {
                /* We can skip updating the frame pointer whenever the process
                 * doesn't have to inspect the stack. We still need to update
                 * the stack pointer to switch stacks, though, since we don't
                 * have enough spare callee-save registers. */
                a.mov(x86::qword_ptr(c_p, offsetof(Process, stop)), E);
            }

            if (Spec & Update::eHeap) {
                a.mov(x86::qword_ptr(c_p, offsetof(Process, htop)), HTOP);
            }
#endif
        }

        if (Spec & Update::eReductions) {
            a.mov(x86::qword_ptr(c_p, offsetof(Process, fcalls)), FCALLS);
        }

#ifdef NATIVE_ERLANG_STACK
        a.lea(E, getRuntimeStackRef());
#else
        /* Keeping track of stack alignment across shared fragments would be
         * too much of a maintenance burden, so we stash and align the stack
         * pointer at runtime instead. */
        a.mov(getRuntimeStackRef(), x86::rsp);

        a.sub(x86::rsp, imm(15));
        a.and_(x86::rsp, imm(-16));
#endif
    }

    template<int Spec = 0>
    void emit_leave_runtime() {
        emit_assert_runtime_stack();

        ERTS_CT_ASSERT((Spec & (Update::eReductions | Update::eStack |
                                Update::eHeap | Update::eCodeIndex)) == Spec);

        if (ERTS_LIKELY(erts_frame_layout == ERTS_FRAME_LAYOUT_RA)) {
            if (Spec & Update::eStack) {
                a.mov(E, x86::qword_ptr(c_p, offsetof(Process, stop)));
            } else {
#ifdef NATIVE_ERLANG_STACK
                a.mov(E, E_saved);
#endif
            }
        } else {
#ifdef ERLANG_FRAME_POINTERS
            ASSERT(erts_frame_layout == ERTS_FRAME_LAYOUT_FP_RA);

            a.mov(E, x86::qword_ptr(c_p, offsetof(Process, stop)));

            if (Spec & Update::eStack) {
                a.mov(frame_pointer,
                      x86::qword_ptr(c_p, offsetof(Process, frame_pointer)));
            }
#endif
        }

        if (Spec & Update::eHeap) {
            a.mov(HTOP, x86::qword_ptr(c_p, offsetof(Process, htop)));
        }

        if (Spec & Update::eReductions) {
            a.mov(FCALLS, x86::qword_ptr(c_p, offsetof(Process, fcalls)));
        }

        if (Spec & Update::eCodeIndex) {
            /* Updates the local copy of the active code index, retaining
             * save_calls if active. */
            a.mov(ARG1, imm(&the_active_code_index));
            a.mov(ARG1d, x86::dword_ptr(ARG1));

            a.cmp(active_code_ix, imm(ERTS_SAVE_CALLS_CODE_IX));
            a.cmovne(active_code_ix, ARG1);
        }

#if !defined(NATIVE_ERLANG_STACK)
        /* Restore the unaligned stack pointer we saved on enter. */
        a.mov(x86::rsp, getRuntimeStackRef());
#endif
    }

    void emit_test_boxed(x86::Gp Src) {
        /* Use the shortest possible instruction depending on the source
         * register. */
        if (Src == x86::rax || Src == x86::rdi || Src == x86::rsi ||
            Src == x86::rcx || Src == x86::rdx) {
            a.test(Src.r8(), imm(_TAG_PRIMARY_MASK - TAG_PRIMARY_BOXED));
        } else {
            a.test(Src.r32(), imm(_TAG_PRIMARY_MASK - TAG_PRIMARY_BOXED));
        }
    }

    void emit_is_boxed(Label Fail, x86::Gp Src, Distance dist = dLong) {
        emit_test_boxed(Src);
        if (dist == dShort) {
            a.short_().jne(Fail);
        } else {
            a.jne(Fail);
        }
    }

    void emit_is_not_boxed(Label Fail, x86::Gp Src, Distance dist = dLong) {
        emit_test_boxed(Src);
        if (dist == dShort) {
            a.short_().je(Fail);
        } else {
            a.je(Fail);
        }
    }

    x86::Gp emit_ptr_val(x86::Gp Dst, x86::Gp Src) {
#if !defined(TAG_LITERAL_PTR)
        return Src;
#else
        if (Dst != Src) {
            a.mov(Dst, Src);
        }

        /* We intentionally skip TAG_PTR_MASK__ here, as we want to use
         * plain `emit_boxed_val` when we know the argument can't be a literal,
         * such as in bit-syntax matching.
         *
         * This comes at very little cost as `emit_boxed_val` nearly always has
         * a displacement. */
        a.and_(Dst, imm(~TAG_LITERAL_PTR));
        return Dst;
#endif
    }

    constexpr x86::Mem emit_boxed_val(x86::Gp Src,
                                      int32_t bytes = 0,
                                      size_t size = sizeof(UWord)) const {
        ASSERT(bytes % sizeof(Eterm) == 0);
        return x86::Mem(Src, bytes - TAG_PRIMARY_BOXED, size);
    }

    void emit_test_the_non_value(x86::Gp Reg) {
        if (THE_NON_VALUE == 0) {
            a.test(Reg.r32(), Reg.r32());
        } else {
            a.cmp(Reg, imm(THE_NON_VALUE));
        }
    }

    /* Set the Z flag if Reg1 and Reg2 are definitely not equal based on their
     * tags alone. (They may still be equal if both are immediates and all other
     * bits are equal too.) */
    void emit_is_unequal_based_on_tags(x86::Gp Reg1, x86::Gp Reg2) {
        ASSERT(Reg1 != RET && Reg2 != RET);
        emit_is_unequal_based_on_tags(Reg1, Reg2, RET);
    }

    void emit_is_unequal_based_on_tags(x86::Gp Reg1,
                                       x86::Gp Reg2,
                                       const x86::Gp &spill) {
        ERTS_CT_ASSERT(TAG_PRIMARY_IMMED1 == _TAG_PRIMARY_MASK);
        ERTS_CT_ASSERT((TAG_PRIMARY_LIST | TAG_PRIMARY_BOXED) ==
                       TAG_PRIMARY_IMMED1);
        a.mov(RETd, Reg1.r32());
        a.or_(RETd, Reg2.r32());
        a.and_(RETb, imm(_TAG_PRIMARY_MASK));

        /* RET will be now be TAG_PRIMARY_IMMED1 if either one or both
         * registers are immediates, or if one register is a list and the other
         * a boxed. */
        a.cmp(RETb, imm(TAG_PRIMARY_IMMED1));
    }

    /*
     * Generate the shortest instruction for setting a register to an immediate
     * value. May clear flags.
     */
    template<typename T>
    void mov_imm(x86::Gp to, T value) {
        static_assert(std::is_integral<T>::value || std::is_pointer<T>::value);
        if (value) {
            /* Generate the shortest instruction to set the register to an
             * immediate.
             *
             *   48 c7 c0 2a 00 00 00    mov    rax, 42
             *   b8 2a 00 00 00          mov    eax, 42
             *
             *   49 c7 c0 2a 00 00 00    mov    r8, 42
             *   41 b8 2a 00 00 00       mov    r8d, 42
             */
            if (Support::isUInt32((Uint)value)) {
                a.mov(to.r32(), imm(value));
            } else {
                a.mov(to, imm(value));
            }
        } else {
            /*
             * Generate the shortest instruction to set the register to zero.
             *
             *   48 c7 c0 00 00 00 00    mov    rax, 0
             *   b8 00 00 00 00          mov    eax, 0
             *   31 c0                   xor    eax, eax
             *
             * Thus, "xor eax, eax" is five bytes shorter than "mov rax, 0".
             *
             * Note: xor clears ZF and CF; mov does not change any flags.
             */
            a.xor_(to.r32(), to.r32());
        }
    }

    void mov_imm(x86::Gp to, std::nullptr_t value) {
        (void)value;
        mov_imm(to, 0);
    }

    /* Copies `count` words from `from` to `to`.
     *
     * Clobbers `spill` and the first vector register (xmm0, ymm0 etc). */
    void emit_copy_words(x86::Mem from,
                         x86::Mem to,
                         Sint32 count,
                         x86::Gp spill) {
        ASSERT(!from.hasIndex() && !to.hasIndex());
        ASSERT(count >= 0 && count < (ERTS_SINT32_MAX / (Sint32)sizeof(UWord)));
        ASSERT(from.offset() < ERTS_SINT32_MAX - count * (Sint32)sizeof(UWord));
        ASSERT(to.offset() < ERTS_SINT32_MAX - count * (Sint32)sizeof(UWord));

        /* We're going to mix sizes pretty wildly below, so it's easiest to
         * turn off size validation. */
        from.setSize(0);
        to.setSize(0);

        using vectors = std::initializer_list<std::tuple<x86::Vec,
                                                         Sint32,
                                                         x86::Inst::Id,
                                                         CpuFeatures::X86::Id>>;
        for (const auto &spec : vectors{{x86::zmm0,
                                         8,
                                         x86::Inst::kIdVmovups,
                                         CpuFeatures::X86::kAVX512_VL},
                                        {x86::zmm0,
                                         8,
                                         x86::Inst::kIdVmovups,
                                         CpuFeatures::X86::kAVX512_F},
                                        {x86::ymm0,
                                         4,
                                         x86::Inst::kIdVmovups,
                                         CpuFeatures::X86::kAVX},
                                        {x86::xmm0,
                                         2,
                                         x86::Inst::kIdMovups,
                                         CpuFeatures::X86::kSSE}}) {
            const auto &[vector_reg, vector_size, vector_inst, feature] = spec;

            if (!hasCpuFeature(feature)) {
                continue;
            }

            /* Copy the words inline if we can, otherwise use a loop with the
             * largest vector size we're capable of. */
            if (count <= vector_size * 4) {
                while (count >= vector_size) {
                    a.emit(vector_inst, vector_reg, from);
                    a.emit(vector_inst, to, vector_reg);

                    from.addOffset(sizeof(UWord) * vector_size);
                    to.addOffset(sizeof(UWord) * vector_size);
                    count -= vector_size;
                }
            } else {
                Sint32 loop_iterations, loop_size;
                Label copy_next = a.newLabel();

                loop_iterations = count / vector_size;
                loop_size = loop_iterations * vector_size * sizeof(UWord);

                from.addOffset(loop_size);
                to.addOffset(loop_size);
                from.setIndex(spill);
                to.setIndex(spill);

                mov_imm(spill, -loop_size);
                a.bind(copy_next);
                {
                    a.emit(vector_inst, vector_reg, from);
                    a.emit(vector_inst, to, vector_reg);

                    a.add(spill, imm(vector_size * sizeof(UWord)));
                    a.short_().jne(copy_next);
                }

                from.resetIndex();
                to.resetIndex();

                count %= vector_size;
            }
        }

        if (count == 1) {
            a.mov(spill, from);
            a.mov(to, spill);

            count -= 1;
        }

        ASSERT(count == 0);
        (void)count;
    }

public:
    void embed_rodata(const char *labelName, const char *buff, size_t size);
    void embed_bss(const char *labelName, size_t size);
    void embed_zeros(size_t size);

    void setLogger(std::string log);
    void setLogger(FILE *log);

    void comment(const char *format) {
        if (logger.file()) {
            a.commentf("# %s", format);
        }
    }

    template<typename... Ts>
    void comment(const char *format, Ts... args) {
        if (logger.file()) {
            char buff[1024];
            erts_snprintf(buff, sizeof(buff), format, args...);
            a.commentf("# %s", buff);
        }
    }

    struct AsmRange {
        ErtsCodePtr start;
        ErtsCodePtr stop;
        const std::string name;

        struct LineData {
            ErtsCodePtr start;
            const std::string file;
            unsigned line;
        };

        const std::vector<LineData> lines;
    };
};

#include "beam_asm_global.hpp"

class BeamModuleAssembler : public BeamAssembler {
    typedef unsigned BeamLabel;

    /* Map of label number to asmjit Label */
    typedef std::unordered_map<BeamLabel, const Label> LabelMap;
    LabelMap rawLabels;

    struct patch {
        Label where;
        int64_t ptr_offs;
        int64_t val_offs;
    };

    struct patch_catch {
        struct patch patch;
        Label handler;
    };
    std::vector<struct patch_catch> catches;

    /* Map of import entry to patch labels and mfa */
    struct patch_import {
        std::vector<struct patch> patches;
        ErtsCodeMFA mfa;
    };
    typedef std::unordered_map<unsigned, struct patch_import> ImportMap;
    ImportMap imports;

    /* Map of fun entry to trampoline labels and patches */
    struct patch_lambda {
        std::vector<struct patch> patches;
        Label trampoline;
    };
    typedef std::unordered_map<unsigned, struct patch_lambda> LambdaMap;
    LambdaMap lambdas;

    /* Map of literals to patch labels */
    struct patch_literal {
        std::vector<struct patch> patches;
    };
    typedef std::unordered_map<unsigned, struct patch_literal> LiteralMap;
    LiteralMap literals;

    /* All string patches */
    std::vector<struct patch> strings;

    /* All functions that have been seen so far */
    std::vector<BeamLabel> functions;

    /* The BEAM file we've been loaded from, if any. */
    const BeamFile *beam;

    BeamGlobalAssembler *ga;

    Label codeHeader;

    /* Used by emit to populate the labelToMFA map */
    Label currLabel;

    /* Special shared fragments that must reside in each module. */
    Label funcInfo;
    Label genericBPTramp;
    Label yieldReturn;
    Label yieldEnter;

    /* The module's on_load function, if any. */
    Label on_load;

    /* The end of the last function. */
    Label code_end;

    Eterm mod;

    /* Save the last PC for an error. */
    size_t last_error_offset = 0;

public:
    BeamModuleAssembler(BeamGlobalAssembler *ga,
                        Eterm mod,
                        int num_labels,
                        const BeamFile *file = NULL);
    BeamModuleAssembler(BeamGlobalAssembler *ga,
                        Eterm mod,
                        int num_labels,
                        int num_functions,
                        const BeamFile *file = NULL);

    bool emit(unsigned op, const Span<ArgVal> &args);

    void codegen(JitAllocator *allocator,
                 const void **executable_ptr,
                 void **writable_ptr,
                 const BeamCodeHeader *in_hdr,
                 const BeamCodeHeader **out_exec_hdr,
                 BeamCodeHeader **out_rw_hdr);

    void codegen(JitAllocator *allocator,
                 const void **executable_ptr,
                 void **writable_ptr);

    void codegen(char *buff, size_t len);

    void register_metadata(const BeamCodeHeader *header);

    ErtsCodePtr getCode(unsigned label);
    ErtsCodePtr getLambda(unsigned index);

    void *getCode(Label label) {
        return BeamAssembler::getCode(label);
    }
    byte *getCode(char *labelName) {
        return BeamAssembler::getCode(labelName);
    }

    Label embed_vararg_rodata(const Span<ArgVal> &args, int y_offset);

    unsigned getCodeSize() {
        ASSERT(code.hasBaseAddress());
        return code.codeSize();
    }

    void copyCodeHeader(BeamCodeHeader *hdr);
    BeamCodeHeader *getCodeHeader(void);
    const ErtsCodeInfo *getOnLoad(void);

    unsigned patchCatches(char *rw_base);
    void patchLambda(char *rw_base, unsigned index, BeamInstr I);
    void patchLiteral(char *rw_base, unsigned index, Eterm lit);
    void patchImport(char *rw_base, unsigned index, BeamInstr I);
    void patchStrings(char *rw_base, const byte *string);

protected:
    int getTypeUnion(const ArgSource &arg) const {
        auto typeIndex =
                arg.isRegister() ? arg.as<ArgRegister>().typeIndex() : 0;

        ASSERT(typeIndex < beam->types.count);
        return beam->types.entries[typeIndex].type_union;
    }

    auto getClampedRange(const ArgSource &arg) const {
        if (arg.isSmall()) {
            Sint value = arg.as<ArgSmall>().getSigned();
            return std::make_pair(value, value);
        } else {
            auto typeIndex =
                    arg.isRegister() ? arg.as<ArgRegister>().typeIndex() : 0;

            ASSERT(typeIndex < beam->types.count);
            const auto &entry = beam->types.entries[typeIndex];
            if (entry.min <= entry.max) {
                return std::make_pair(entry.min, entry.max);
            } else if (IS_SSMALL(entry.min) && !IS_SSMALL(entry.max)) {
                return std::make_pair(entry.min, MAX_SMALL);
            } else if (!IS_SSMALL(entry.min) && IS_SSMALL(entry.max)) {
                return std::make_pair(MIN_SMALL, entry.max);
            } else {
                return std::make_pair(MIN_SMALL, MAX_SMALL);
            }
        }
    }

    int getSizeUnit(const ArgSource &arg) const {
        auto typeIndex =
                arg.isRegister() ? arg.as<ArgRegister>().typeIndex() : 0;

        ASSERT(typeIndex < beam->types.count);
        return beam->types.entries[typeIndex].size_unit;
    }

    bool hasLowerBound(const ArgSource &arg) const {
        auto typeIndex =
                arg.isRegister() ? arg.as<ArgRegister>().typeIndex() : 0;
        ASSERT(typeIndex < beam->types.count);
        const auto &entry = beam->types.entries[typeIndex];
        return IS_SSMALL(entry.min) && !IS_SSMALL(entry.max);
    }

    bool hasUpperBound(const ArgSource &arg) const {
        auto typeIndex =
                arg.isRegister() ? arg.as<ArgRegister>().typeIndex() : 0;
        ASSERT(typeIndex < beam->types.count);
        const auto &entry = beam->types.entries[typeIndex];
        return !IS_SSMALL(entry.min) && IS_SSMALL(entry.max);
    }

    bool always_small(const ArgSource &arg) const {
        if (arg.isSmall()) {
            return true;
        }

        auto typeIndex =
                arg.isRegister() ? arg.as<ArgRegister>().typeIndex() : 0;
        ASSERT(typeIndex < beam->types.count);
        const auto &entry = beam->types.entries[typeIndex];
        return entry.type_union == BEAM_TYPE_INTEGER && entry.min <= entry.max;
    }

    bool always_immediate(const ArgSource &arg) const {
        if (arg.isImmed() || always_small(arg)) {
            return true;
        }

        int type_union = getTypeUnion(arg);
        return (type_union & BEAM_TYPE_MASK_ALWAYS_IMMEDIATE) == type_union;
    }

    bool always_same_types(const ArgSource &lhs, const ArgSource &rhs) const {
        int lhs_types = getTypeUnion(lhs);
        int rhs_types = getTypeUnion(rhs);

        /* We can only be certain that the types are the same when there's
         * one possible type. For example, if one is a number and the other
         * is an integer, they could differ if the former is a float. */
        if ((lhs_types & (lhs_types - 1)) == 0) {
            return lhs_types == rhs_types;
        }

        return false;
    }

    bool always_one_of(const ArgSource &arg, int types) const {
        if (arg.isImmed()) {
            if (arg.isSmall()) {
                return !!(types & BEAM_TYPE_INTEGER);
            } else if (arg.isAtom()) {
                return !!(types & BEAM_TYPE_ATOM);
            } else if (arg.isNil()) {
                return !!(types & BEAM_TYPE_NIL);
            }

            return false;
        } else {
            int type_union = getTypeUnion(arg);
            return type_union == (type_union & types);
        }
    }

    int masked_types(const ArgSource &arg, int mask) const {
        if (arg.isImmed()) {
            if (arg.isSmall()) {
                return mask & BEAM_TYPE_INTEGER;
            } else if (arg.isAtom()) {
                return mask & BEAM_TYPE_ATOM;
            } else if (arg.isNil()) {
                return mask & BEAM_TYPE_NIL;
            }

            return BEAM_TYPE_NONE;
        } else {
            return getTypeUnion(arg) & mask;
        }
    }

    bool exact_type(const ArgSource &arg, int type_id) const {
        return always_one_of(arg, type_id);
    }

    bool is_sum_small_if_args_are_small(const ArgSource &LHS,
                                        const ArgSource &RHS) {
        Sint min, max;
        auto [min1, max1] = getClampedRange(LHS);
        auto [min2, max2] = getClampedRange(RHS);
        min = min1 + min2;
        max = max1 + max2;
        return IS_SSMALL(min) && IS_SSMALL(max);
    }

    bool is_diff_small_if_args_are_small(const ArgSource &LHS,
                                         const ArgSource &RHS) {
        Sint min, max;
        auto [min1, max1] = getClampedRange(LHS);
        auto [min2, max2] = getClampedRange(RHS);
        min = min1 - max2;
        max = max1 - min2;
        return IS_SSMALL(min) && IS_SSMALL(max);
    }

    bool is_product_small_if_args_are_small(const ArgSource &LHS,
                                            const ArgSource &RHS) {
        auto [min1, max1] = getClampedRange(LHS);
        auto [min2, max2] = getClampedRange(RHS);
        auto mag1 = std::max(std::abs(min1), std::abs(max1));
        auto mag2 = std::max(std::abs(min2), std::abs(max2));

        /*
         * mag1 * mag2 <= MAX_SMALL
         * mag1 <= MAX_SMALL / mag2   (when mag2 != 0)
         */
        ERTS_CT_ASSERT(MAX_SMALL < -MIN_SMALL);
        return mag2 == 0 || mag1 <= MAX_SMALL / mag2;
    }

    bool is_bsl_small(const ArgSource &LHS, const ArgSource &RHS) {
        if (!(always_small(LHS) && always_small(RHS))) {
            return false;
        } else {
            auto [min1, max1] = getClampedRange(LHS);
            auto [min2, max2] = getClampedRange(RHS);

            if (min1 < 0 || max1 == 0 || min2 < 0) {
                return false;
            }

            return max2 < Support::clz(max1) - _TAG_IMMED1_SIZE;
        }
    }

    /* Helpers */
    void emit_gc_test(const ArgWord &Stack,
                      const ArgWord &Heap,
                      const ArgWord &Live);
    void emit_gc_test_preserve(const ArgWord &Need,
                               const ArgWord &Live,
                               const ArgSource &Preserve,
                               x86::Gp preserve_reg);

    x86::Mem emit_variable_apply(bool includeI);
    x86::Mem emit_fixed_apply(const ArgWord &arity, bool includeI);

    x86::Gp emit_call_fun(bool skip_box_test = false,
                          bool skip_fun_test = false,
                          bool skip_arity_test = false);

    x86::Gp emit_is_binary(const ArgLabel &Fail,
                           const ArgSource &Src,
                           Label next,
                           Label subbin);

    void emit_is_boxed(Label Fail, x86::Gp Src, Distance dist = dLong) {
        BeamAssembler::emit_is_boxed(Fail, Src, dist);
    }

    void emit_is_boxed(Label Fail,
                       const ArgVal &Arg,
                       x86::Gp Src,
                       Distance dist = dLong) {
        if (always_one_of(Arg, BEAM_TYPE_MASK_ALWAYS_BOXED)) {
            comment("skipped box test since argument is always boxed");
            return;
        }

        BeamAssembler::emit_is_boxed(Fail, Src, dist);
    }

    void emit_get_list(const x86::Gp boxed_ptr,
                       const ArgRegister &Hd,
                       const ArgRegister &Tl);

    void emit_div_rem(const ArgLabel &Fail,
                      const ArgSource &LHS,
                      const ArgSource &RHS,
                      const ErtsCodeMFA *error_mfa,
                      bool need_div = true,
                      bool need_rem = true);

    void emit_setup_guard_bif(const std::vector<ArgVal> &args,
                              const ArgWord &bif);

    void emit_error(int code);

    x86::Mem emit_bs_get_integer_prologue(Label next,
                                          Label fail,
                                          int flags,
                                          int size);

    int emit_bs_get_field_size(const ArgSource &Size,
                               int unit,
                               Label Fail,
                               const x86::Gp &out,
                               unsigned max_size = 0);

    void emit_bs_get_utf8(const ArgRegister &Ctx, const ArgLabel &Fail);
    void emit_bs_get_utf16(const ArgRegister &Ctx,
                           const ArgLabel &Fail,
                           const ArgWord &Flags);
    void update_bin_state(x86::Gp bin_base,
                          x86::Gp bin_offset,
                          Sint bit_offset,
                          Sint size,
                          x86::Gp size_reg);
    bool need_mask(const ArgVal Val, Sint size);
    void set_zero(Sint effectiveSize);
    bool bs_maybe_enter_runtime(bool entered);
    void bs_maybe_leave_runtime(bool entered);

    void emit_raise_exception();
    void emit_raise_exception(const ErtsCodeMFA *exp);
    void emit_raise_exception(Label I, const ErtsCodeMFA *exp);
    void emit_raise_exception(x86::Gp I, const ErtsCodeMFA *exp);

    void emit_validate(const ArgWord &arity);
    void emit_bs_skip_bits(const ArgLabel &Fail, const ArgRegister &Ctx);

    void emit_linear_search(x86::Gp val,
                            const ArgVal &Fail,
                            const Span<ArgVal> &args);

    void emit_float_instr(uint32_t instId,
                          const ArgFRegister &LHS,
                          const ArgFRegister &RHS,
                          const ArgFRegister &Dst);

    void emit_is_small(Label fail, const ArgSource &Arg, x86::Gp Reg);
    void emit_are_both_small(Label fail,
                             const ArgSource &LHS,
                             x86::Gp A,
                             const ArgSource &RHS,
                             x86::Gp B);

    void emit_validate_unicode(Label next, Label fail, x86::Gp value);

    void emit_bif_is_eq_ne_exact(const ArgSource &LHS,
                                 const ArgSource &RHS,
                                 const ArgRegister &Dst,
                                 Eterm fail_value,
                                 Eterm succ_value);

    void emit_proc_lc_unrequire(void);
    void emit_proc_lc_require(void);

    void emit_nyi(const char *msg);
    void emit_nyi(void);

    void emit_binsearch_nodes(size_t Left,
                              size_t Right,
                              const ArgVal &Fail,
                              const Span<ArgVal> &args);

    bool emit_optimized_three_way_select(const ArgVal &Fail,
                                         const Span<ArgVal> &args);

#ifdef DEBUG
    void emit_tuple_assertion(const ArgSource &Src, x86::Gp tuple_reg);
#endif

#include "beamasm_protos.h"

    const Label &resolve_beam_label(const ArgLabel &Lbl) const {
        return rawLabels.at(Lbl.get());
    }

    void make_move_patch(x86::Gp to,
                         std::vector<struct patch> &patches,
                         int64_t offset = 0) {
        const int MOV_IMM64_PAYLOAD_OFFSET = 2;
        Label lbl = a.newLabel();

        a.bind(lbl);
        a.long_().mov(to, imm(LLONG_MAX));

        patches.push_back({lbl, MOV_IMM64_PAYLOAD_OFFSET, offset});
    }

    void make_word_patch(std::vector<struct patch> &patches) {
        Label lbl = a.newLabel();
        UWord word = LLONG_MAX;

        a.bind(lbl);
        a.embed(reinterpret_cast<char *>(&word), sizeof(word));

        patches.push_back({lbl, 0, 0});
    }

    template<typename A, typename B>
    void mov_arg(A to, B from) {
        /* We can't move to or from Y registers when we're on the runtime
         * stack, so we'll conservatively disallow all mov_args in the hopes of
         * finding such bugs sooner. */
        emit_assert_erlang_stack();

        mov_arg(to, from, ARG1);
    }

    template<typename T>
    void cmp_arg(T oper, const ArgVal &val) {
        cmp_arg(oper, val, ARG1);
    }

    void cmp_arg(x86::Mem mem, const ArgVal &val, const x86::Gp &spill) {
        /* Note that the cast to Sint is necessary to handle negative numbers
         * such as NIL. */
        if (val.isImmed() && Support::isInt32((Sint)val.as<ArgImmed>().get())) {
            a.cmp(mem, imm(val.as<ArgImmed>().get()));
        } else {
            mov_arg(spill, val);
            a.cmp(mem, spill);
        }
    }

    void cmp_arg(x86::Gp gp, const ArgVal &val, const x86::Gp &spill) {
        if (val.isImmed() && Support::isInt32((Sint)val.as<ArgImmed>().get())) {
            a.cmp(gp, imm(val.as<ArgImmed>().get()));
        } else {
            mov_arg(spill, val);
            a.cmp(gp, spill);
        }
    }

    void cmp(x86::Gp gp, int64_t val, const x86::Gp &spill) {
        if (Support::isInt32(val)) {
            a.cmp(gp, imm(val));
        } else {
            mov_imm(spill, val);
            a.cmp(gp, spill);
        }
    }

    void sub(x86::Gp gp, int64_t val, const x86::Gp &spill) {
        if (Support::isInt32(val)) {
            a.sub(gp, imm(val));
        } else {
            mov_imm(spill, val);
            a.sub(gp, spill);
        }
    }

    /* Note: May clear flags. */
    void mov_arg(x86::Gp to, const ArgVal &from, const x86::Gp &spill) {
        if (from.isBytePtr()) {
            make_move_patch(to, strings, from.as<ArgBytePtr>().get());
        } else if (from.isExport()) {
            make_move_patch(to, imports[from.as<ArgExport>().get()].patches);
        } else if (from.isImmed()) {
            mov_imm(to, from.as<ArgImmed>().get());
        } else if (from.isLambda()) {
            make_move_patch(to, lambdas[from.as<ArgLambda>().get()].patches);
        } else if (from.isLiteral()) {
            make_move_patch(to, literals[from.as<ArgLiteral>().get()].patches);
        } else if (from.isRegister()) {
            a.mov(to, getArgRef(from.as<ArgRegister>()));
        } else if (from.isWord()) {
            mov_imm(to, from.as<ArgWord>().get());
        } else {
            ASSERT(!"mov_arg with incompatible type");
        }

#ifdef DEBUG
        /* Explicitly clear flags to catch bugs quicker, it may be very rare
         * for a certain instruction to load values that would otherwise cause
         * flags to be cleared. */
        a.test(to, to);
#endif
    }

    void mov_arg(x86::Mem to, const ArgVal &from, const x86::Gp &spill) {
        if (from.isImmed()) {
            auto val = from.as<ArgImmed>().get();

            if (Support::isInt32((Sint)val)) {
                a.mov(to, imm(val));
            } else {
                a.mov(spill, imm(val));
                a.mov(to, spill);
            }
        } else {
            mov_arg(spill, from);
            a.mov(to, spill);
        }
    }

    void mov_arg(const ArgVal &to, x86::Gp from, const x86::Gp &spill) {
        (void)spill;

        a.mov(getArgRef(to), from);
    }

    void mov_arg(const ArgVal &to, x86::Mem from, const x86::Gp &spill) {
        a.mov(spill, from);
        a.mov(getArgRef(to), spill);
    }

    void mov_arg(const ArgVal &to, BeamInstr from, const x86::Gp &spill) {
        if (Support::isInt32((Sint)from)) {
            a.mov(getArgRef(to), imm(from));
        } else {
            a.mov(spill, imm(from));
            mov_arg(to, spill);
        }
    }

    void mov_arg(const ArgVal &to, const ArgVal &from, const x86::Gp &spill) {
        if (from.isRegister()) {
            mov_arg(spill, from);
            mov_arg(to, spill);
        } else {
            mov_arg(getArgRef(to), from);
        }
    }
};

void beamasm_metadata_update(
        std::string module_name,
        ErtsCodePtr base_address,
        size_t code_size,
        const std::vector<BeamAssembler::AsmRange> &ranges);
void beamasm_metadata_early_init();
void beamasm_metadata_late_init();

/**
 * @file        rex/core/fiber_android.cpp
 * @brief       Android backend for rex::thread::Fiber
 *
 * Android's Bionic does not provide getcontext/makecontext/swapcontext.
 * This implementation uses setjmp/longjmp with explicit stack allocation,
 * a common pattern for fiber libraries on Android (see libuv, boost.fcontext).
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#if REX_PLATFORM_ANDROID

#include <rex/thread/fiber.h>

#include <cassert>
#include <csetjmp>
#include <csignal>
#include <cstdint>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

// We bootstrap a new fiber by switching the stack pointer via inline asm,
// calling Trampoline, and immediately longjmp-ing back to the creator.
// Subsequent switches are setjmp/longjmp pairs between fibers.

// Platform-specific context save/restore.
// Wrapping setjmp/longjmp to preserve signal mask is not necessary here
// because the fibers are cooperative and don't alter signal masks.

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  // For the thread fiber, we just capture the current context.
  if (setjmp(f->jmpbuf_) == 0) {
    // Initial capture — nothing to do.
  }
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

// Helper: bootstrap a fiber on its own stack. Called once during Create().
static thread_local Fiber* s_bootstrap_fiber = nullptr;
static thread_local jmp_buf s_bootstrap_return;

/*static*/ void Fiber::Trampoline() {
  // Bootstrap trampoline: save context and jump back to Create().
  Fiber* f = s_bootstrap_fiber;
  if (setjmp(f->jmpbuf_) == 0) {
    // Return to Create()
    longjmp(s_bootstrap_return, 1);
  }
  // Reached on the first real SwitchTo into this fiber.
  f = Fiber::tls_current_;
  f->entry_(f->arg_);
  // If the entry function returns, spin to avoid stack corruption.
  for (;;) {}
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);

  // Set up the bootstrap: save current context, switch SP to the new stack,
  // call the trampoline which saves its context into f->jmpbuf_, then returns
  // here via longjmp(s_bootstrap_return).
  s_bootstrap_fiber = f;

  if (setjmp(s_bootstrap_return) == 0) {
    // Jump to the new stack and call the trampoline.
    // Stack grows downward on ARM64; point SP to the top of the allocation.
    void* stack_top = f->stack_.data() + f->stack_.size();
    // Align to 16 bytes (AArch64 ABI requirement).
    stack_top = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(stack_top) & ~uintptr_t(15));

    // Use a plain function pointer for the trampoline call via inline asm.
    void (*trampoline_fn)() = &Fiber::Trampoline;

#if REX_ARCH_ARM64
    register void* sp_new __asm__("x0") = stack_top;
    register void (*fn)() __asm__("x1") = trampoline_fn;
    __asm__ volatile(
        "mov x28, sp\n"  // save current SP in a callee-saved reg
        "mov sp, x0\n"   // switch to new stack
        "blr x1\n"       // call trampoline (should longjmp back)
        "mov sp, x28\n"  // restore SP (shouldn't reach here, but safety)
        :
        : "r"(sp_new), "r"(fn)
        : "x28", "x30", "memory");
#elif REX_ARCH_AMD64
    __asm__ volatile(
        "mov %%rsp, %%r12\n"     // save current SP
        "mov %[stack], %%rsp\n"  // switch to new stack
        "call *%[trampoline]\n"  // call Trampoline
        "mov %%r12, %%rsp\n"     // restore SP
        :
        : [stack] "r"(stack_top), [trampoline] "r"(&Fiber::Trampoline)
        : "r12", "memory");
#else
#error "Unsupported architecture for Android fibers"
#endif
    // Should not reach here — BootstrapTrampoline longjmps to s_bootstrap_return.
    delete f;
    return nullptr;
  }

  // Returned from the bootstrap longjmp — f->jmpbuf_ is now set up.
  s_bootstrap_fiber = nullptr;
  return f;
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  if (setjmp(from->jmpbuf_) == 0) {
    longjmp(target->jmpbuf_, 1);
  }
  // Execution resumes here when someone longjmps back to `from`.
}

void Fiber::Destroy() {
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_ANDROID

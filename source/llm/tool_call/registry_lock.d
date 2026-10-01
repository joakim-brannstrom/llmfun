/// The llm.tool_call registry mutation lock: program-start
/// registration (the RegisterLlmFunctions mixins' shared static this) is
/// single threaded, but the MCP runtime registration path registers from other
/// threads, so addFunction serializes its registry mutation with this lock.
///
/// The lock is a spin lock on an atomic flag rather than a Mutex because the
/// lazy singleton alternatives all fail somewhere: a module constructor does
/// not run reliably for the dub test runner (its generated test root links
/// only test-carrying modules), and a plain __gshared Mutex global starts as
/// null per thread (D globals are thread-local by default). Mutations are
/// short (a duplicate scan + append) and contention is program start (single
/// threaded) plus rare MCP connects.
module llm.tool_call.registry_lock;

import core.atomic : MemoryOrder, atomicStore, cas;
import core.thread : Thread;

// TODO: use shared
/// 0 = unlocked, 1 = locked.
private __gshared shared(int) registrySpin_;

/// Acquire the registry mutation lock (blocks until acquired).
void registrySpinAcquire() @trusted nothrow @nogc {
    while (!cas(&registrySpin_, 0, 1))
        Thread.yield();
}

/// Release the registry mutation lock (only after registrySpinAcquire).
void registrySpinRelease() @trusted nothrow @nogc {
    atomicStore!(MemoryOrder.rel)(registrySpin_, 0);
}

/// Run dg holding the registry mutation lock. Non-reentrant:
/// nesting self-deadlocks by spin — addFunction legitimately logs a
/// duplicate-name warning while holding the lock, but never register from
/// inside another withRegistryLock body.
void withRegistryLock(scope void delegate() dg) @trusted {
    registrySpinAcquire();
    scope (exit)
        registrySpinRelease();
    dg();
}

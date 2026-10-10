// Minimal downstream consumer, paired with the CMakeLists.txt in this directory.
//
// NOTE: this file is deliberately ASCII-only, unlike the rest of the repository.
// The reason is on the CMake side too -- see the comment in CMakeLists.txt about
// /utf-8. A downstream project must be able to consume this library with **default
// compiler settings**, so this file refuses to depend on any encoding flag.
//
// The #include lines below are the actual point of the test: when the package
// export is broken, "cannot find ms_queue.h" is the first thing to blow up,
// while the library's own build stays perfectly green.
#include "block_queue.h"
#include "lockfree_queue.h"
#include "ms_queue.h"
#include "ring_buffer.h"

#include <cstdio>

int main() {
    RingBuffer<int> ring(4);
    ring.Push(1);
    int a = 0;
    ring.Pop(a);

    LockFreeQueue<int> spsc(4);
    spsc.Enqueue(41);
    int b = 0;
    spsc.Dequeue(b);

    MSQueue<int> mpmc;
    mpmc.Enqueue(100);
    int c = 0;
    mpmc.Dequeue(c);

    // The close semantics have to be callable from downstream too -- that is part
    // of the public contract this repo now promises.
    BlockQueue<int> blocking(4);
    blocking.Push(a + b); // 1 + 41
    int d = 0;
    blocking.Pop(d);
    blocking.Close();

    printf("consumer ok: %d (ring %d, spsc %d, mpmc %d)\n", d, a, b, c);
    return d == 42 ? 0 : 1;
}

threads/
========

Fun mini-projects that have to do with threading/concurrency in some way, shape,
or form.

queue.cc
--------
Implementation of a lock-free, multi-writer/single-reader queue. Tuned for
writes to happen in batches compared to reads. Enqueue/Dequeue are both
lock-free, with Enqueue requiring an atomic operation each time, and Dequeue
only requiring an atomic operation sometimes.

It has no way of waiting for a new value to magically appear; you can spin, or
build your own with locks and condition variables. If you spin, please consider
those of us with hyperthreading, and either use OS primitives, or `pause`

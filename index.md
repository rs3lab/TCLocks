---
title: Home
---

# TCLocks

Today's high-performance applications heavily rely on various
synchronization mechanisms, such as locks.  While locks ensure mutual
exclusion of shared data, their design impacts application scalability.
Locks, as used in practice, move the lock-guarded shared data to the
core holding it, which leads to shared data transfer among cores.
This design adds unavoidable critical path latency leading to
performance scalability issues.  Meanwhile, some locks avoid this
shared data movement by localizing the access to shared data on
one core, and shipping the critical section to that specific core.
However, such locks require modifying applications to explicitly
package the critical section, which makes it virtually infeasible
for complicated applications with large code-bases, such as the
Linux kernel.

We propose transparent delegation, in which a waiter
automatically encodes its critical section information on its stack
and notifies the combiner (lock holder).  The combiner executes the
shipped critical section on the waiter's behalf using a light-weight
context switch.  Using transparent delegation, we design a family of
locking protocols (TCLocks), which require zero modification to
applications' logic.  The evaluation shows that TCLocks provide
up to 5.2x performance improvement compared with recent locking
algorithms.


The source code is publicly available at the [Github repository](https://github.com/rs3lab/TCLocks).

TCLocks and its associated paper will be presented at the Proceedings of the 17th USENIX Symposium on Operating Systems Design and Implementation 2023 (OSDI '23).
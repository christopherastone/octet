octet
=====

A reimplementation of Bond et al.'s OCTET barriers (lightweight, biased locks) in C++11.

See [Bond et. al, _OCTET: Capturing and Controlling Cross-Thread Dependencies
Efficiently_](http://www.cse.ohio-state.edu/~mikebond/papers.html#octet) (OOPSLA 2013)
and [Sengupta et al., _EnforSCer: Hybrid
Static–Dynamic Analysis for End-to-End Sequential Consistency in Software_](http://www.cse.ohio-state.edu/~mikebond/papers.html#enforscer) (TR)
for detailed background, but to quote from the latter:

> OCTET [has] read and write barriers before every access to potentially shared
> objects. At run time, each barrier checks that the current thread has exclusive
> or read-shared access to the object. If so, the access may proceed; otherwise,
> the barrier must change the object’s state before the access can proceed.
>
> The key to OCTET’s good performance is that if a thread already has exclusive or
> read-shared access to an object, it may proceed without performing
> synchronization. In this way, OCTET barriers essentially function like locks
> except that (1) they support both exclusive and read-shared behavior; (2) there
> is no explicit release operation, but rather an acquiring thread communicates
> with thread(s) that have access to the lock in order to gain access; and (3)
> acquire operations do not need synchronization as long as the thread already has
> access to the lock.


This code was written based on the descriptions in the above papers, rather than
starting from the official OCTET source code, which is now available in the form of
[a patch for the Jikes RVM](http://sourceforge.net/p/jikesrvm/research-archive/43/).
Further, there are a few *intentional* differences from the OCTET paper:

   * The global/thread-local counters for Read-Shared locks were
     elided, and compensating synchronization was added.
   * All threads use an identical "Intermediate" flag value.
   * I added an explicit unlocking operation, because it speeds up the stress test
     in certain situations.

The code is written in C++11, and the stress test has been compiled and run on
Mac OS X and on Linux (assuming clang++ is installed).

__Acknowledgements__

This code was developed for experimental and exploratory purposes within the
[Observationally Cooperative Multithreading](http://ocm-model.org/) research
project, and so we gratefully acknowledge that this material is based upon work
supported by the National Science Foundation under Grant No. 1219243. Any
opinions, findings, and conclusions or recommendations expressed in this
material are those of the author(s) and do not necessarily reflect the views of
the National Science Foundation.

We are also grateful to Mike Bond for answering a few questions about OCTET
barriers.


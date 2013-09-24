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


Notes

   * This code was written based on the descriptions in the above papers, and
     thus may or may not be a faithful implementation.

   * There are a few intentional differences from the OCTET paper
      * The global/thread-local counters for Read-Shared locks have
        been elided. This requires compensating synchronization.
      * All threads use the same "Intermediate" flag value.
      * I have added an explicit unlocking operation for testing purposes.


   * This code was tested only on MAC OS X (10.8), and even there only with
     a relatively simple stress test.



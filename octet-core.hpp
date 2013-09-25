/*
 * octet-core.hpp
 *
 * Locks modeled on the "Octet" barriers of Bond et al.
 *    "OCTET: Capturing and Controlling Cross-Thread Dependencies Efficiently"
 *
 * Author: Christopher A. Stone <stone@cs.hmc.edu>
 *
 */

#include <atomic>

/////////////////////////

// The following defines control the compilation of the lock code.

// If DEBUG is set, the lock operations will produce copious debugging output.

#define DEBUG 0

// If set, we use only default memory_order_sequential everywhere
//    (I've tried to get the memory order constraints correct everywhere,
//     but if the locks are not guaranteeing mutual exclusion, try turning
//     this on and see if it helps.)
#define SEQUENTIAL 0

// Should we gather/display lock statistics
#define STATISTICS 0

// Should we allow read/write locking (hence read-shared locking?)
#define READSHARED 0

/////////////////////////

// The above defines control two auxiliary macros


// MEM_ORD ignores its argument(s) if SEQUENTIAL is set,
// and is an identity function otherwise. I've put the
// std::memory_order_XXX operands (and any associated commas)
// inside this macro, so that we can easily go back to the
// default of sequential consistency.

#if SEQUENTIAL
#define MEM_ORD(...)
#else
#define MEM_ORD(...) __VA_ARGS__
#endif

// TRACE is an atomic_printf function if DEBUG is set,
// and a no-op otherwise.

#if DEBUG
#define TRACE(...) atomic_printf(__VA_ARGS__);
#else
#define TRACE(...)
#endif

/////////////////////////


namespace octet {


    /////////////////////////
    // Types


    // OctetThreadInfo
    //
    // Each thread maintains exactly one of these objects (in the thread-local
    //    variable myThreadInfo).
    // It tracks three pieces of data:
    //    (1) How many times threads have requested locks from them
    //    (2) Whether the thread is currently blocked (and hence
    //             other threads can just "take" anything they need,
    //             i.e., implicitly assume that any request is granted)
    //    (3) How many requests have been agreed to.
    //
    // So, if (1) > (3), then there are pending requests from threads
    //     that we have not yet agreed to (unless we're blocked).
    // And if (1) == (3), then there are no pending requests.
    //
    // For efficiency, we keep (1) and (2) in the same 32-bit word.
    //   Item (1) occupies the high 31 bits (to get the count, must ">> 1")
    //   Item (2) occupies the low bit (to get the flag, must "& 1")
    // This limits us to 2 billion lock requests. If it's a problem, we can
    // switch to 63-bit counters...

    struct OctetThreadInfo {
        std::atomic<uint32_t> requests_;  // 31 bit count + 1 bit "blocked" flag.
        char padding[64 - sizeof(requests_)];
        std::atomic<uint32_t> responses_;

        OctetThreadInfo( bool startBlocked = false );

        void handleRequests( bool shouldBlock );
        void unblock();

    };

    extern __thread OctetThreadInfo* myThreadInfo;


    // octetLockState_t
    //
    // The underlying representation for per-object locks consists of an
    // pointer-sized value. We distinguish four possibilities:
    //   (1) the value 0 means the lock is in read-shared mode
    //   (2) the value 1 means the lock is in "intermediate mode"
    //            (in the process of being acquired by a new thread)
    // Otherwise, the value gives us a pointer to an OctetThreadInfo
    //   (3) if the least-significant-bit is 0, it's locked for writing
    //           (and the pointer points to the owner's OctetThreadInfo object)
    //   (4) if the least-significant-bit is 1, it's locked for reading
    //           (and we need to zero out that bit before following the pointer).
    using octetLockState_t = uintptr_t;

    // octetLock_t
    //
    // Per-object locks are just the above pointer-sized value,
    // wrapped in a C++ atomic.
    //
    using octetLock_t = std::atomic<octetLockState_t>;


    // The following macros are useful for octetLockState_t values.
    //  * WrEx(T): Thread T may read or write the object without changing the state.
    //  * RdEx(T): T may read but not write the object without changing the state.
    //  * RdSh   : Any thread may read but not write the object without changing
    //                the state.

#define RDSH         0L
#define INTERMEDIATE 1L
#define WREX(T)      (reinterpret_cast<octetLockState_t>(T))
#define RDEX(T)      (reinterpret_cast<octetLockState_t>(T) | 0x1)

#define GET_TID(X)   (reinterpret_cast<OctetThreadInfo*>((X) & ~1))
#define IS_WREX(X)   ((X) != 0L && ((X) & 0x1) == 0)
#define IS_RDEX(X)   ((X) != 1L && ((X) & 0x1) != 0)
#define IS_RDSH(X)   ((X) == RDSH)




    /////////////////////////////
    // Inline methods start here
    /////////////////////////////

    // Declare internal variables/functions used by the inline methods

#if STATISTICS
    extern __thread size_t writeBarriers;
    extern __thread size_t slowWrites;
    extern __thread size_t readBarriers;
    extern __thread size_t slowReads;
#endif

    bool readSlowPath( octetLock_t* objLock );
    bool writeSlowPath( octetLock_t* objLock );

    int atomic_printf(const char *format, ...);


    // writeBarrier
    //
    //  Locks the given lock in WrEx mode.
    //
    //  Returns whether we granted any requests (e.g., while waiting).
    //    (and hence whether locks *other* than the one being locked here
    //     were relinquished)
    //
    inline bool writeBarrier( octetLock_t* objLock )
    {
#if STATISTICS
        ++writeBarriers;
#endif

        octetLockState_t goalState = WREX(myThreadInfo);

        // Memory order: if we find the value we're looking for, it could only
        //    be this thread who wrote it, so there are no cross-thread memory
        //    issues. If we don't see the value we're looking for, the CAS in
        //    the slow path will make sure we will get up-to-date data.
        octetLockState_t curState =
            objLock->load( MEM_ORD( std::memory_order_relaxed ) );

        if ( curState != goalState)  {
            TRACE("Thread 0x%x on slow path to write-lock 0x%x\n",
                  myThreadInfo, objLock);

            return writeSlowPath( objLock );
        }

        TRACE("Thread 0x%x took fast path to write-lock 0x%x\n",
              myThreadInfo, objLock);

        // The fast path never grants any requests from other threads.
        return false;
    }


    // readBarrier
    //
    //  Locks the given lock in RdEx or RdSh mode, as appropriate.
    //
    //  Returns whether we granted any requests (e.g., while waiting).
    //    (and hence whether locks *other* than the one being locked here
    //     were relinquished)
    //
    inline bool readBarrier( octetLock_t* objLock )
    {

#if READSHARED

#if STATISTICS
        ++readBarriers;
#endif

        // Memory order: if we find our own thread in the lock, it could only
        //    be this thread who wrote it, so there are no cross-thread memory
        //    issues. If we don't see the value we're looking for, the CAS in
        //    the slow path will make sure we will get up-to-date data.
        //
        octetLockState_t curState = objLock->load();

        if ( GET_TID(curState) != myThreadInfo ) {

            if ( curState == RDSH ) {

                // Memory order:
                //    On the other hand, if we see RdSh, it could have been written
                //    by another thread. We don't have to take the full slow path,
                //    but we do want to make sure that we see any changes to the data
                //    that happened-before that thread switched the data to RdSh

                std::atomic_thread_fence( std::memory_order_acquire );

            } else {

                TRACE("Thread 0x%x on slow path to read-lock 0x%x\n",
                      myThreadInfo, objLock);

                return readSlowPath( objLock );

            }
        }

        TRACE("Thread 0x%x took fast path to read-lock 0x%x\n",
              myThreadInfo, objLock);

        // The fast path never grants any requests from other threads.
        return false;

#else // READSHARED

        // If we don't distinguish between read locking and write locking,
        // then read and write barriers are the same.
        return writeBarrier( objLock );

#endif // READSHARED

    }



} // namespace octet



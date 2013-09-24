/* octet-private.hpp */

// Contains utility template definitions
// that act on octet::Lock objects (and hence
// cannot be defined in octet-core.hpp).


#include <utility>

namespace octet {
    
   
    ////////////////////////////////////////////
    // Helper functions to grab n locks
    ////////////////////////////////////////////
    
    // References consulted:
    // (1) LLVM library code
    //   http://llvm.org/svn/llvm-project/libcxx/trunk/include/mutex
    // particularly std::try_lock and std::lock.
    //
    // (2) http://stackoverflow.com/questions/10044449/a-function-with-variable-number-of-arguments-with-known-types-the-c11-way
    //
    // (3) Lippmann pp. 701-706
    
    
    // WARNING: the return value is (mostly) backwards from pthreads try_lock.
    // Lock acquisitional always succeeds (eventually); the question is whether
    //    we were interrupted (lost *other* locks) in the mean time.
    
    template <typename... Args>
    inline bool trylockThem() {
        // No locking to do => no interruptions.
        return false;
    }

    inline bool trylockOne(Lock& l1, bool lockForWriting)
    {
        if (lockForWriting) {
            TRACE("Thread 0x%x about to write-lock 0x%x\n", myThreadInfo, &l1);
            return l1.writeLock();
        } else {
            TRACE("Thread 0x%x about to read-lock 0x%x\n", myThreadInfo, &l1);
            return l1.readLock();
        }
    }
    
    template <typename... Args>
    inline bool trylockThem(Lock& l1, const bool& lockForWriting, Args&&... args)
    {
        bool restart = trylockOne(l1, lockForWriting);
        
        restart |= trylockThem(std::forward<Args>(args)...);
        
        return restart;
    }

    const int OCTET_BACKOFF_RETRIES = 5;
    const int OCTET_BACKOFF_EXPLIMIT = 13;
    
    // Note: only guarantees that all the given locks are locked.
    //       Does not say whether we might have lost other locks
    //       in the process.
    template <typename ...Tail>
    void lock(Lock& l1, bool lockForWriting, Tail&&... tail)
    {
        bool restart;
        size_t retries = 0;
        const int BACKOFF_RETRIES = OCTET_BACKOFF_RETRIES;
        const int MAX_BACKOFF = BACKOFF_RETRIES + OCTET_BACKOFF_EXPLIMIT;
        int us = 1;
        
        // The restart flag tells us whether we relinquished any locks in the
        //   process of acquiring these three locks.
        // This is a very coarse criterion. Maybe we lost some lock from
        //   a previous iteration (that we don't care about any more). Or
        //   maybe we lost the first two locks in the process of acquiring
        //   the third.
        // Fortunately, if we lost a lock we don't care about (which is
        //   the more likely alternative in the absence of high contention),
        //   the next loop will take 3 straight fast-paths and succeed almost
        //   immediately.
        
        do {
            // If we lost locks while waiting for the first, we don't care.
            trylockOne(l1, lockForWriting);
            // And if we lost locks while getting the rest, we might.
            restart = trylockThem(std::forward<Tail>(tail)...);
            
            if ( restart ) {
                ++retries;

                if (retries > BACKOFF_RETRIES) {

                    if (retries < MAX_BACKOFF) us *= 2;
                    
                    myThreadInfo->handleRequests( true );
                    std::this_thread::sleep_for(std::chrono::microseconds(us));
                    myThreadInfo->unblock();
                }
            }
        } while (restart);
    }

}



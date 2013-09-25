/*
 * octet.cpp
 *
 * Locks modeled on the "Octet" barriers of Bond et al.
 *    "OCTET: Capturing and Controlling Cross-Thread Dependencies Efficiently"
 *
 * Author: Christopher A. Stone <stone@cs.hmc.edu>
 *
 */

#include <cassert>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <atomic>
#include <unordered_set>
#include <vector>
#include <utility>
#include <algorithm>
#include <thread>

#include "octet.hpp"


namespace octet {



    ////////////////////////////////////////////
    // Global State
    ////////////////////////////////////////////

    // When a thread wants to write to a read-shared object,
    // it has to get permission from all readers. Since we don't
    // specifically track readers for each object, we conservatively
    // get permission from *all* running threads. We thus keep a
    // mutex-protected set representing the current threads, or at
    // least the OctetThreadInfo objects for the current threads.

#if READSHARED
    std::mutex activeThreadsMutex;
    std::unordered_set<OctetThreadInfo*> activeThreads;
#endif


    void initPerthread()
    {
        // Double-check that this is only being called once per pthread
        assert(myThreadInfo == nullptr);
        myThreadInfo = new OctetThreadInfo;

#if READSHARED
        // Add this thread to the set of active threads
        std::lock_guard<std::mutex> lockTheSet(activeThreadsMutex);
        activeThreads.insert( myThreadInfo );
#endif
    }

    void shutdownPerthread()
    {

        // Mark this thread as blocked and handle any pending requests.
        myThreadInfo->handleRequests( true );

#if READSHARED
        // Remove me from the set of active threads
        std::lock_guard<std::mutex> lockTheSet(activeThreadsMutex);
        assert( activeThreads.find(myThreadInfo) != activeThreads.end() );
        activeThreads.erase( myThreadInfo );
#endif


        // We leak a little bit of memory (*myThreadInfo)
        //   because there may still be many objects owned
        //   by us.

#if STATISTICS
        atomic_printf("Thread 0x%x: %d/%d slow writes and %d/%d slow reads\n",
                      myThreadInfo, slowWrites, writeBarriers, slowReads, readBarriers);
#endif
    }

    ////////////////////////////////////////////
    // Per-Thread State
    ////////////////////////////////////////////

    // Clang 3.2 (on the mac) doesn't support the thread_local qualifier.
    //   (Apparently 3.3 will)

    // WARNING: if we ever move to thread pools, then we have to start worrying
    // about keeping track of per worker-thread info rather than per-pthread info!

    // It's important to keep the data on the heap, rather than in thread-local
    // memory, because we want it to persist beyond the termination of the thread.

    __thread OctetThreadInfo* myThreadInfo = nullptr;


#if STATISTICS
    __thread size_t writeBarriers = 0;
    __thread size_t slowWrites = 0;
    __thread size_t readBarriers = 0;
    __thread size_t slowReads = 0;
#endif

    ///////////////////////////////
    // For Debugging
    ///////////////////////////////


    static int atomic_vfprintf(FILE *stream, const char *format, va_list &ap)
    {
        return vfprintf(stream, format, ap);
    }

    int atomic_printf(const char *format, ...)
    {
        va_list ap;
        va_start(ap, format);
        return atomic_vfprintf(stderr, format, ap);
    }


    ////////////////////////////////////////////
    // The OctetThreadInfo class
    ////////////////////////////////////////////

    // Methods used by the *owner* of the OctetThreadInfo

    OctetThreadInfo::OctetThreadInfo( bool startBlocked )
    : requests_(startBlocked), responses_(0)
    {
        // Sanity checking
        assert( requests_.is_lock_free() );
        assert( responses_.is_lock_free() );
    };

    void OctetThreadInfo::handleRequests( bool shouldBlock )
    {
        // Recall:fetch_or returns the old (hopefully unblocked) value
        uint32_t req = requests_.fetch_or( shouldBlock
                                          MEM_ORD(, std::memory_order_acq_rel ) );

        // We shouldn't call handleRequests while the thread is blocked.
        //    (unblock() would be more appropriate)
        assert ( ! (req & 0x1) );

        uint32_t request_count = req >> 1;

        // Memory order:
        //    Any threads waiting for this response are in a memory_order_acquire
        //    loop. By using release here, we ensure that any changes we made to
        //    data before giving up the lock happen-before the waiting thread
        //    sees the response.
        responses_.store( request_count MEM_ORD(, std::memory_order_release ) );
    }

    void OctetThreadInfo::unblock()
    {
        requests_.fetch_and( ~1 MEM_ORD(, std::memory_order_acq_rel) );
    }




    ////////////////////////////////////////////
    // Implementation Code for (slow) read and write barriers
    ////////////////////////////////////////////

    int INTERMEDIATE_BACKOFF_RETRIES = 1;
    int INTERMEDIATE_BACKOFF_EXPLIMIT = 1;

    // lockIntermediate
    //
    //    Mark the object as being in the process of being acquired.
    //    Return the immediately preceeding (non-INTERMEDIATE)
    //    OctetThreadInfo*.
    //
    //    If the lock is already in an intermediate state when this
    //       code is called, we wait until it's not (and *then*
    //       mark it as intermediate).
    octetLockState_t lockIntermediate( octetLock_t* objLock )
    {

        TRACE("Thread 0x%x setting 0x%x to intermediate\n", myThreadInfo, objLock);

        // Memory order: since anything we read will be verified by compare_exchange,
        //               stale data would not be problematic.
        octetLockState_t prevLock =
           objLock->load( MEM_ORD( std::memory_order_relaxed ) );

        while (prevLock == INTERMEDIATE ||
               ! objLock->compare_exchange_weak( prevLock, INTERMEDIATE ) ) {

            // In practice, adding this yield produces a huge performance boost
            //    when the test has more threads than cpus
            //    (e.g., on my 4-core Macbook Pro,
            //        6 threads, 10000 iterations, 1000 accounts)
            // I tried adding an exponential backoff as in lock(), and at least
            // for high contention (10/10000/10 stresstest), it was 5-6x slower.
            std::this_thread::yield();


            // We can't grab the lock yet, so to avoid deadlock, we
            // respond to any pending requests.
            myThreadInfo->handleRequests( false );

            // If the compare_exchange failed, then prevLock has already
            //   changed (been updated). But the value might have changed
            //   again while we were handling requests, so it doesn't hurt
            //   to re-update it. Plus, if the lock was in an intermediate
            //   state, we didn't attempt the compare_exchange and must
            //   poll again to see if the intermediate lock has resolved
            //   to a new owner.
            //
            // Memory order: see above.
            prevLock = objLock->load( MEM_ORD( std::memory_order_relaxed ) );
        }

        TRACE("Thread 0x%x set 0x%x to intermediate\n", myThreadInfo, objLock);

        assert ( prevLock != INTERMEDIATE );

        return prevLock;
    }

    // ping
    //
    // Notifies another thread that we want something they have locked.
    //
    // Returns the response count that will allow us to proceed, and
    //     (via reference) whether the pinged thread was blocked
    //     at the time of the ping. [If so, we can just go ahead with
    //     the acquire, rather than waiting for a response.]
    //
    uint32_t ping( OctetThreadInfo* owner, bool& owner_was_blocked )
    {
        assert ( owner != nullptr );
        assert ( owner != myThreadInfo );

        // We could probably avoid increasing the request count if
        // the thread is blocked. But, the thread probably wants to know
        // whether any requests were implicitly granted while it
        // was blocked, so we don't make that optimization.

        // Increase by 2 because the LSB bit is being used as a flag.
        // Note: fetch_add returns the *previous* value of the variable,
        //       but += returns the *updated* value.
        uint32_t req = owner->requests_ += 2;

        // If this assert fails, we need to switch from 31-bit to 63-bit counters
        assert (req < 2147483644ul);  // 2^31 - 4

        owner_was_blocked          = req & 1;
        uint32_t new_request_count = req >> 1;

        TRACE(owner_was_blocked ? "Thread 0x%x pinged 0x%x (blocked)\n":
              "Thread 0x%x pinged 0x%x\n", myThreadInfo, owner);

        return new_request_count;
    }

    // awaitResponse
    //
    // Waits until the specified thread approves our request (by
    //   incrementing its response count sufficiently).
    //
    //
    void awaitResponse( OctetThreadInfo* owner, uint32_t desired_response_count )
    {
        assert ( owner != nullptr );

        // Memory order: if we do get a response, we want to make sure that we will
        //               be able to see all data written by the owner before they
        //               responded.
        uint32_t response_count = owner->responses_.load( MEM_ORD( std::memory_order_acquire ) );

        TRACE("Thread 0x%x waiting for response from 0x%x\n", myThreadInfo, owner);

        while( response_count < desired_response_count ) {

            // If not, yield, and try again.
            //    (This function is a little bit convoluted, because we don't want
            //    to yield if the response was immediate.)
            // TODO: would it be better to sleep (with increasing backoff?)
            std::this_thread::yield();

            // Need to handle requests while waiting, to avoid deadlock.
            myThreadInfo->handleRequests( false );

            // Mmeory order: see above.
            response_count = owner->responses_.load( MEM_ORD( std::memory_order_acquire ) );
        }
    }

    // notifyOne
    //
    //    slow-path round trip (unless the receiver is blocked) communication
    //    for when we're planning to steal a lock.
    //
    void notifyOne( OctetThreadInfo* owner )
    {
        assert( owner != nullptr );

        TRACE("Thread 0x%x will notify 0x%x\n", myThreadInfo, owner);

        // Ping the owner
        bool ownerWasBlocked = false;
        uint32_t desired_response_count = ping( owner, ownerWasBlocked );

        // Wait until the owner is blocked or has responded.
        //   (The owner will respond before blocking, so we
        //   only have to check for blocking at the very start.)

        if (! ownerWasBlocked ) {
            awaitResponse( owner, desired_response_count );
        }
    }

    // writeSlowPath
    //
    //   Locks the given lock for write-exclusive access
    //
    //   Returns a flag stating whether we've lost any previous locks
    //       (agreed to other threads' requests) in the process
    //
    bool writeSlowPath( octetLock_t* objLock )
    {

#if STATISTICS
        ++slowWrites;
#endif

        // We count the number of responses before and after the slow path,
        //    to detect whether we granted any requests (lost any locks) in
        //    the mean time.
        //
        // Memory order:
        //    Since we're in the only thread that ever *writes* to
        //    the response count, we don't have to worry about preserving
        //    inter-thread dependencies here when peeking at the count.

        uint32_t requestsBefore =
        myThreadInfo->responses_.load( MEM_ORD( std::memory_order_relaxed ) );

        // XXX: If the thread is unlocked, it would make more sense to
        // grab it directly, rather than setting it to INTERMEDIATE
        // and notifying a non-existent "owner"

        octetLockState_t prevLock = lockIntermediate( objLock );

#if READSHARED
        if ( IS_RDSH( prevLock ) ) {

            // XXX  We can lock the activeThreads set while we're notifying everyone, but
            // What happens if threads appear or disappear while we're waiting !!??

            // Ping *everyone* (sigh...)

            TRACE("Thread 0x%x wants to write to RdSh data 0x%x; notifying everyone\n", myThreadInfo, objLock)

            activeThreadsMutex.lock();

            std::vector<std::pair<OctetThreadInfo*,uint32_t>> peers;

            for (OctetThreadInfo* owner : activeThreads ) {

                if ( owner != myThreadInfo ) {
                    bool wasBlocked = false;
                    uint32_t count = ping( owner, wasBlocked );
                    if (! wasBlocked  ) {
                        peers.push_back( std::make_pair(owner,count) );
                    }
                }
            }

            activeThreadsMutex.unlock();

            // Wait for each in turn to respond.

            for (auto& peer : peers)
            {
                assert( peer.first != nullptr );

                awaitResponse( peer.first, peer.second );
            }

        } else {
#endif // READSHARED
            OctetThreadInfo* owner = GET_TID( prevLock );

            if ( owner != myThreadInfo) {
                // Another thread holds a RdEx or WrEx lock
                notifyOne( owner );
            } else {
                // Only other possibility (since we're on the slow path):
                //  upgrading our own read-lock to a write-lock.
                assert ( prevLock = RDEX(myThreadInfo) );
            }
#if READSHARED
        }
#endif
        // OK, mark it as ours!


        // Memory order: This is after we used CAS to set the same variable to INTERMEDIATE;
        //               whether other threads see that or this, they're still not allowed
        //               to observe the protected data.
        objLock->store( WREX(myThreadInfo) MEM_ORD(, std::memory_order_relaxed ) );

        TRACE("Thread 0x%x can now write to 0x%x\n", myThreadInfo, objLock)

        // Memory order: see above.
        uint32_t requestsAfter =
        myThreadInfo->responses_.load( MEM_ORD( std::memory_order_relaxed ) );

        // Did we grant any requests while waiting?
        bool requestsWereGranted = requestsBefore != requestsAfter;

        return requestsWereGranted;
    }

#if READSHARED

    // readSlowPath
    //
    //   Locks the given lock for read-exclusive or read-shared access
    //       (depending on whether the lock is already locked for
    //        read access or not)
    //
    //   Returns a flag stating whether we've lost any previous locks
    //       (agreed to other threads' requests) in the process
    //
    bool readSlowPath( octetLock_t* objLock )
    {

#if STATISTICS
        slowReads++;
#endif

        // We count the number of responses before and after the slow path,
        //    to detect whether we granted any requests (lost any locks) in
        //    the mean time.
        //
        // Memory order:
        //    Since we're in the only thread that ever *writes* to
        //    the response count, we don't have to worry about preserving
        //    inter-thread dependencies here when peeking at the count.

        uint32_t requestsBefore =
        myThreadInfo->responses_.load( MEM_ORD( std::memory_order_relaxed ) );

        octetLockState_t prevLock = lockIntermediate( objLock );

        assert( prevLock != INTERMEDIATE );

        if ( IS_RDSH( prevLock ) ) {

            // Normally we wouldn't take the slow path if the lock was already
            // read-shared, but it's possible that the state was changed by
            // another thread to RdSh while we were waiting for our turn to
            // set the lock to INTERMEDIATE. (E.g., we're in the slow path
            // because another thread had it RdEx, and a third thread snuck
            // in and set it to RdSh before we got our hands on the lock.)

            // We've already set it to INTERMEDIATE; put it back the way it was.

            objLock->store( RDSH );

        } else if ( IS_RDEX( prevLock ) ) {

            // Someone else had it locked for exclusive reading.
            // Generalize the lock to RdSh

            assert( GET_TID ( prevLock) != myThreadInfo );

            objLock->store( RDSH );

        } else {

            assert( IS_WREX( prevLock ) );
            // Find the owner and notify them.

            OctetThreadInfo* owner = GET_TID( prevLock );
            assert( owner != nullptr );

            notifyOne( owner );

            objLock->store( RDEX( myThreadInfo ) );
        }

        TRACE("Thread 0x%x can now read 0x%x\n", myThreadInfo, objLock)

        // See above for the justification of "relaxed"
        uint32_t requestsAfter =
        myThreadInfo->responses_.load( MEM_ORD( std::memory_order_relaxed ) );

        // Did we grant any requests while waiting?
        bool requestsWereGranted = requestsBefore != requestsAfter;

        return requestsWereGranted;
    }
#endif // READSHARED

    // yield
    //
    //    Potentially releases locks
    //
    void yield()
    {
        myThreadInfo->handleRequests( false );
    }

    ////////////////////////////////////////////
    // Actual Octet Lock objects
    ////////////////////////////////////////////

    // noThreadInfo
    //
    // Returns the OctetThreadInfo for a designated "dead" thread,
    // who is considered the owner of all newly created locks.
    //
    OctetThreadInfo* noThreadInfo()
    {
        // Create the illusion of a terminated (permanently blocked) thread.
        // Note: C++11 guarantees that only one thread will initialize this
        //       resource
        static OctetThreadInfo* nti = new OctetThreadInfo( true );
        return nti;
    }

    Lock::Lock() :
       lk_( WREX( noThreadInfo() ) )
    {
       // Nothing else to do
    }


    void Lock::forceUnlock()
    {
        // We can't just overwrite the lock with the "unlocked"
        // bit pattern, because another thread might have marked
        // it INTERMEDIATE. Or it might be in a RDSH state.
        // Or we might have unlocked it previously, and another
        // thread has already claimed it.

        octetLockState_t unlocked = WREX( noThreadInfo() );


        octetLockState_t objLock =
           lk_.load( MEM_ORD( std::memory_order_relaxed ) );

        // Assumes GET_TID returns non-pointer value for RDSH, INTERMEDIATE
        if ( GET_TID(objLock) == myThreadInfo ) {
            // Best effort attempt to unlock
            lk_.compare_exchange_strong( objLock, unlocked ) ;
        }
    }


} // namespace octet



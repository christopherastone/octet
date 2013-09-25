/*
 * octet.hpp
 *
 * Locks modeled on the "Octet" barriers of Bond et al.
 *    "OCTET: Capturing and Controlling Cross-Thread Dependencies Efficiently"
 *
 * This file contains the "public" interface to octet locks.
 *
 * Author: Christopher A. Stone <stone@cs.hmc.edu>
 *
 */

#ifndef OCTET_HPP_INCLUDED
#define OCTET_HPP_INCLUDED

#include "octet-core.hpp"

namespace octet {

    class Lock {

        octetLock_t lk_;

    public:
        Lock();

        bool readLock()  { return readBarrier ( &lk_ ); }
        bool writeLock() { return writeBarrier ( &lk_ ); }

        void forceUnlock();
    };

    // yield
    //
    //     Calling this makes you a good citizen,
    //     because it checks to see if anyone is waiting for
    //     one of our locks.
    void yield();

    // initPerthread
    //
    //     Should be called once at the beginning of each thread.
    //
    void initPerthread();


    // shutdownPerthread
    //
    //     Should be called once at the beginning of each thread.
    //
    void shutdownPerthread();

    // utility functions
    int atomic_printf(const char *format, ...);

    template <typename ...Tail>
    void lock(Tail&&... tail);

}

#include "octet-private.hpp"

#endif // OCTET_HPP_INCLUDED

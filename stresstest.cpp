// stresstest.cpp
//
// Uses octet locks as lightweight locks, with potentially heavy contention.
//
//  Creates an array of "accounts" all initially 0
//  Threads repeatedly choose
//     one to increment; one to decrement; one to just read
//
//  At the end, the sum of all accounts should be zero
//  (if the locks are enforcing mutual exclusion!)
//
//  Compile-time flags let us run the same test with octet vs. pthreads
//  (recursive) locks, remove all contention, etc.

///////////////////
// CONTROL FLAGS //
///////////////////

// USE_OCTET
//
// If 1, we'll use OCTET locking.
// If 0, we'll use Pthreads (recursive) locks

#define USE_OCTET 1

// DO_YIELD
//   If 1, we'll do a yield-like step at the end of each iteration.
//   If 0, we won't.
//
//   Note: Octet yield == Grant other threads' pending requests.
//         Pthreads yield == sched_yield()
#define DO_YIELD 0

// CONTENTION
//   If 1, we choose 2-3 random accounts per iteration, randomly
//   If 0, thread i gets accounts 30i+1
//              (no contention, and no false (data) sharing.
#define CONTENTION 1

// OCTET_UNLOCK
//    If 1, we set locks to unowned at the end of
//           each iteration [unless someone else has grabbed them]
//    If 0, we retain ownership until we get an explicit slow-path request
//            from another thread.
//
#define OCTET_UNLOCK 0

static_assert( !OCTET_UNLOCK || USE_OCTET,
              "OCTET_UNLOCK only makes sense when we are using Octet barriers");

////////////////////////
// CONTROL PARAMETERS //
////////////////////////

int NUM_THREADS = 10;            // How many threads are created

int NUM_ITERATIONS = 10000;      // How much work each thread does

int NUM_ACCOUNTS = 10;           // How many accounts the threads are choosing from
                                 //   (more accounts ==> less interthread
                                 //   contention)


#include <algorithm>
#include <cassert>
#include <ctime>
#include <unistd.h>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if USE_OCTET
#include "octet.hpp"
#else
#include <mutex>
#endif



// A lockable integer.

struct Account {
    volatile int balance_;
#if USE_OCTET
    octet::Lock lock_;
#else
    // We use recursive_mutex rather than mutex, because
    //  extra might equal from or to.
    std::recursive_mutex lock_;
#endif

    Account() : balance_(0) {}
};


Account* accounts;

// Futzes with the accounts array.
//   Repeatedly picks three elements
//      increments one, decrements another, reads a third
//        (although the read account might overlap with one of
//         the first two)
void futz(int threadNum)
{

#if USE_OCTET
    octet::initPerthread();
    // octet::atomic_printf("Starting thread %d: 0x%x\n", threadNum, myThreadInfo);
#endif


    std::default_random_engine engine(100*threadNum);
    std::uniform_int_distribution<int> dis(0,NUM_ACCOUNTS-1);

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
#if CONTENTION
        int from  = dis(engine);
        int to    = dis(engine);
        int extra = dis(engine);
#else
        int from = 30*threadNum;
        int to   = 30*threadNum + 1;
        int extra = 30*threadNum + 2;
        assert (extra < NUM_ACCOUNTS);
#endif

        // The read-modify-write code below doesn't work when (from==to).
        if (from == to) {--i; continue; }


        //////////////////
        // Lock the three accounts
        /////////////////

        // from and to locked for writing; extra locked for reading.

#if USE_OCTET
        octet::lock(accounts[from].lock_,  true,
                    accounts[to].lock_,    true,
                    accounts[extra].lock_, false);
#else
        std::lock(accounts[from].lock_,
                  accounts[to].lock_,
                  accounts[extra].lock_);
#endif


        /////////////////////////////
        // READ-MODIFY-WRITE sequence
        /////////////////////////////

        int from_balance = accounts[from].balance_;
        int to_balance = accounts[to].balance_;

        --from_balance;
        ++to_balance;

        accounts[to].balance_= to_balance;
        accounts[from].balance_ = from_balance;

#if USE_OCTET
#if OCTET_UNLOCK
        accounts[to].lock_.forceUnlock();
        accounts[from].lock_.forceUnlock();
        accounts[extra].lock_.forceUnlock();
#endif
#if DO_YIELD
        // Optional (be a good citizen)
        octet::yield();
#endif
#else
        accounts[from].lock_.unlock();
        accounts[to].lock_.unlock();
        accounts[extra].lock_.unlock();
#if DO_YIELD
        std::this_thread::yield();
#endif
#endif
    }

#if USE_OCTET
    // octet::atomic_printf("Ending thread %d\n", threadNum);
    octet::shutdownPerthread();
#endif

    return;
}
namespace octet {
    extern int INTERMEDIATE_BACKOFF_EXPLIMIT;
    extern int INTERMEDIATE_BACKOFF_RETRIES;
}

int main(int argc, char** argv)
{
    // Command-line argument processing

    std::vector<std::string> args(argv, argv+argc);

    if (argc >= 2) {
        NUM_THREADS = std::max(1, std::stoi(args[1]));
    }
    if (argc >= 3) {
        NUM_ITERATIONS = std::max(1, std::stoi(args[2]));
    }
    if (argc >= 4) {
        NUM_ACCOUNTS = std::max(1, std::stoi(args[3]));
    }

    // Where and when did this test run?

    const int MAXHOSTNAME_LENGTH = 256;
    char hostname[MAXHOSTNAME_LENGTH];
    gethostname(hostname, MAXHOSTNAME_LENGTH);

    auto now =
       std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    // Display the parameters of the test.

    std::cout << "Context: "
              << std::put_time(std::localtime(&now), "%c") << "  "
              << hostname << "   "
              << std::endl;

    std::cout << "Compiled settings: USE_OCTET=" << USE_OCTET << "  "
              << "DO_YIELD=" << DO_YIELD << "  "
              << "CONTENTION=" << CONTENTION << "   "
              << "OCTET_UNLOCK=" << OCTET_UNLOCK << "  "
              << std::endl;

#if USE_OCTET
    // Log the compile-time options for the octet library
    std::cout << "Library  settings: DEBUG=" << DEBUG << "  "
              << "SEQUENTIAL=" << SEQUENTIAL << "  "
              << "STATISTICS=" << STATISTICS << "  "
              << "READSHARED=" << READSHARED << "  "
              << std::endl;
#endif

    std::cout << "Run-time settings: NUM_THREADS= " << NUM_THREADS << "  "
              << "NUM_ITERATIONS=" << NUM_ITERATIONS << "  "
              << "NUM_ACCOUNTS=" << NUM_ACCOUNTS << "  "
              << std::endl;


    // Set up the test.

    accounts = new Account[NUM_ACCOUNTS];
    std::thread* thread = new std::thread[NUM_THREADS];

    // Run the test, with timing.

    auto start = std::chrono::system_clock::now();

    for (int i = 0; i < NUM_THREADS; ++i) {
        thread[i] = std::thread(futz, i);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
        thread[i].join();
    }

    auto end = std::chrono::system_clock::now();
    auto elapsed =
       std::chrono::duration_cast<std::chrono::milliseconds>(end-start).count();

    // Verify that nothing went wrong.
    int sum = 0;
    for (int i = 0; i < NUM_ACCOUNTS; ++i) {
        sum += accounts[i].balance_;
    }
    assert (sum == 0);


    // Display running-time
    std::cout << elapsed << "ms  " ;
    std::cout << std::endl << std::endl;

    // Clean up
    delete[] accounts;
    delete[] thread;

    return 0;
}


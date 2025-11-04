#include "List.h"
#include "Skiplist.h"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <algorithm>
#include "Epochs.h"
// simple helper to assign thread_id manually
constexpr int THREAD_COUNT = 4;
constexpr int OPS_PER_THREAD = 2000;

SkipList<int> list;

// Atomic counters to track failures
std::atomic<int> addFailures{ 0 };
std::atomic<int> getFailures{ 0 };
std::atomic<int> containsFailures{ 0 };
std::atomic<int> removeFailures{ 0 };
std::mutex resultsMutex;
// Simple helper to register threads
void register_thread(int id) {
    EpochManager::instance().registerThread(id);
}
void popWorker(int threadId, SkipList<int>& list, std::vector<int>& results, std::mutex& resultsMutex) {
    register_thread(threadId);
    while (true) {
        int* val = list.popMin();
        if (!val) break; // skiplist empty

        {
            std::lock_guard<std::mutex> lock(resultsMutex);
            results.push_back(*val);
        }
    }
}
void worker(int t) {
    register_thread(t);
    int opCount = 0;

    for (int i = t * OPS_PER_THREAD; i < (t + 1) * OPS_PER_THREAD; ++i) {
        opCount++;
        if (opCount % 1000 == 0) {
            std::cout << "[Thread " << t << "] processed " << opCount << " ops\n";
        }
        if (!list.add(i, i)) addFailures++;

        auto v = list.get(i);
        if (!v || *v != i) getFailures++;

        if (!list.contains(i)) containsFailures++;

        if (!list.remove(i)) removeFailures++;
    }
    EpochManager::instance().unregisterThread(t);
}
SkipList<int> pq;

// Worker function for threads
void worker2(int threadId, int start, int end) {
    for (int i = start; i < end; ++i) {
        pq.add(i, i * 10);
    }
}

int main() {
    const int THREADS = 4;
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back(worker2, t, t * 200, (t + 1) * 200);
    }

    for (auto& th : threads) th.join();
    int* val = nullptr;
    while ((val = pq.popMin()) != nullptr) {
        std::cout << *val << " ";
    }
    std::cout << std::endl;

    constexpr int THREAD_COUNT = 4;
    constexpr int OPS_PER_THREAD = 2000;

    SkipList<int> list;

    std::vector<std::thread> threads2;

    for (int t = 0; t < THREAD_COUNT; ++t) {
        threads2.emplace_back([&, t]() {
            register_thread(t);

            for (int i = t * OPS_PER_THREAD; i < (t + 1) * OPS_PER_THREAD; ++i) {
                list.add(i, i); // insert key=i, value=i
                auto v = list.get(i);
                if (v)
                    std::cout << *v << "\n";
                if (v == nullptr || *v != i) {
                    std::cout << "[THREAD " << t << "] GET FAILED at " << i << "\n";
                }

                if (!list.contains(i)) {
                    std::cout << "[THREAD " << t << "] CONTAINS FAILED at " << i << "\n";
                }

                if (!list.remove(i)) {
                    std::cout << "[THREAD " << t << "] REMOVE FAILED at " << i << "\n";
                }
            }

            EpochManager::instance().unregisterThread(t);
            });
    }

    for (auto& th : threads2) th.join();

    std::cout << "Finished. Final check...\n";

    // sanity test: list should be empty
    for (int i = 0; i < THREAD_COUNT * OPS_PER_THREAD; ++i) {
        if (list.contains(i)) {
            std::cout << "ERROR: value " << i << " still in list!\n";
        }
    }

    std::cout << "Test complete.\n";
    
    
    std::vector<std::thread> threads3;

    // Start workers
    for (int t = 0; t < THREAD_COUNT; ++t)
        threads3.emplace_back(worker, t);

    // Join all
    for (auto& th : threads3)
        th.join();

    std::cout << "Test complete.\n";
    std::cout << "Add failures: " << addFailures << "\n";
    std::cout << "Get failures: " << getFailures << "\n";
    std::cout << "Contains failures: " << containsFailures << "\n";
    std::cout << "Remove failures: " << removeFailures << "\n";

    // Final sanity check: list should be empty
    int remaining = 0;
    for (int i = 0; i < THREAD_COUNT * OPS_PER_THREAD; ++i) {
        if (list.contains(i)) remaining++;
    }
    std::cout << "Remaining items in list: " << remaining << "\n";
    
    const int TOTAL_NODES = 2000;
    
    SkipList<int> list3;
    for (int i = 0; i < TOTAL_NODES; ++i) {
        list3.add(i, i);
    }

    std::vector<int> results;
    std::mutex resultsMutex;
    std::vector<std::thread> threads4;
    for (int t = 0; t < THREAD_COUNT; ++t)
        threads4.emplace_back(popWorker, t, std::ref(list3), std::ref(results), std::ref(resultsMutex));

    for (auto& th : threads4) th.join();

    // Verify results
    std::sort(results.begin(), results.end());
    for (int i = 0; i < TOTAL_NODES; ++i) {
        if (results[i] != i) {
            std::cout << "Mismatch at " << i << ": got " << results[i] << "\n";
        }
    }
    std::cout << "PopMin test complete. Total nodes popped: " << results.size() << "\n";


    return 0;
}



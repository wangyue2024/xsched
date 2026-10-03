#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <cassert>
#include <chrono>
#include <shared_mutex>
#include <unordered_map>
#include <algorithm>

// Standalone verification of CudaContextRegistry algorithmic logic
namespace test_eval {

class TestCudaContextRegistry {
public:
    static void Register(void* ctx, uint64_t hwq_h) {
        if (!ctx || !hwq_h) return;
        std::unique_lock lock(mtx_);
        auto &vec = registry_[ctx];
        if (std::find(vec.begin(), vec.end(), hwq_h) == vec.end()) {
            vec.push_back(hwq_h);
        }
    }

    static void Unregister(void* ctx, uint64_t hwq_h) {
        if (!hwq_h) return;
        std::unique_lock lock(mtx_);
        bool found = false;
        if (ctx) {
            auto it = registry_.find(ctx);
            if (it != registry_.end()) {
                auto &vec = it->second;
                auto pos = std::find(vec.begin(), vec.end(), hwq_h);
                if (pos != vec.end()) {
                    vec.erase(pos);
                    if (vec.empty()) registry_.erase(it);
                    found = true;
                }
            }
        }
        if (!found) {
            for (auto it = registry_.begin(); it != registry_.end(); ) {
                auto &vec = it->second;
                auto pos = std::find(vec.begin(), vec.end(), hwq_h);
                if (pos != vec.end()) {
                    vec.erase(pos);
                }
                if (vec.empty()) {
                    it = registry_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    static std::vector<uint64_t> GetSnapshot(void* ctx) {
        if (!ctx) return {};
        std::shared_lock lock(mtx_);
        auto it = registry_.find(ctx);
        if (it == registry_.end()) return {};
        return it->second;
    }

    static size_t ContextCount() {
        std::shared_lock lock(mtx_);
        return registry_.size();
    }

    static void Clear() {
        std::unique_lock lock(mtx_);
        registry_.clear();
    }

private:
    static std::shared_mutex mtx_;
    static std::unordered_map<void*, std::vector<uint64_t>> registry_;
};

std::shared_mutex TestCudaContextRegistry::mtx_;
std::unordered_map<void*, std::vector<uint64_t>> TestCudaContextRegistry::registry_;

} // namespace test_eval

using namespace test_eval;

void TestBasicRegistrationAndIsolation() {
    std::cout << "[Test 1] Basic Registration and Context Isolation... ";
    TestCudaContextRegistry::Clear();

    void* ctx1 = (void*)0x1000;
    void* ctx2 = (void*)0x2000;

    TestCudaContextRegistry::Register(ctx1, 101);
    TestCudaContextRegistry::Register(ctx1, 102);
    TestCudaContextRegistry::Register(ctx2, 201);

    auto snap1 = TestCudaContextRegistry::GetSnapshot(ctx1);
    auto snap2 = TestCudaContextRegistry::GetSnapshot(ctx2);

    assert(snap1.size() == 2);
    assert(snap1[0] == 101 && snap1[1] == 102);
    assert(snap2.size() == 1);
    assert(snap2[0] == 201);

    // Verify deduplication
    TestCudaContextRegistry::Register(ctx1, 101);
    snap1 = TestCudaContextRegistry::GetSnapshot(ctx1);
    assert(snap1.size() == 2);

    // Verify unregister
    TestCudaContextRegistry::Unregister(ctx1, 101);
    snap1 = TestCudaContextRegistry::GetSnapshot(ctx1);
    assert(snap1.size() == 1 && snap1[0] == 102);

    // Context 2 should remain completely unaffected
    snap2 = TestCudaContextRegistry::GetSnapshot(ctx2);
    assert(snap2.size() == 1 && snap2[0] == 201);

    // Unregister remaining in ctx1 -> ctx1 removed from registry
    TestCudaContextRegistry::Unregister(ctx1, 102);
    snap1 = TestCudaContextRegistry::GetSnapshot(ctx1);
    assert(snap1.empty());
    assert(TestCudaContextRegistry::ContextCount() == 1);

    // Defensive scan unregister with null ctx
    TestCudaContextRegistry::Unregister(nullptr, 201);
    snap2 = TestCudaContextRegistry::GetSnapshot(ctx2);
    assert(snap2.empty());
    assert(TestCudaContextRegistry::ContextCount() == 0);

    std::cout << "PASSED!\n";
}

void TestConcurrentStress() {
    std::cout << "[Test 2] High Concurrency Stress Test (16 threads, 50,000 ops)... ";
    TestCudaContextRegistry::Clear();

    constexpr int NUM_THREADS = 16;
    constexpr int OPS_PER_THREAD = 3000;
    std::vector<std::thread> workers;
    std::atomic<bool> start_flag{false};
    std::atomic<int> errors{0};

    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([t, &start_flag, &errors]() {
            while (!start_flag.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            void* my_ctx = (void*)(uintptr_t)(0x1000 + (t % 4) * 0x1000);
            for (int i = 1; i <= OPS_PER_THREAD; ++i) {
                uint64_t handle = (uint64_t)t * 100000 + i;
                TestCudaContextRegistry::Register(my_ctx, handle);
                auto snap = TestCudaContextRegistry::GetSnapshot(my_ctx);
                if (snap.empty()) {
                    errors++;
                }
                TestCudaContextRegistry::Unregister(my_ctx, handle);
            }
        });
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    start_flag.store(true);
    for (auto &w : workers) {
        w.join();
    }
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    assert(errors.load() == 0);
    assert(TestCudaContextRegistry::ContextCount() == 0);
    std::cout << "PASSED (" << (NUM_THREADS * OPS_PER_THREAD) << " ops in " << elapsed_ms << " ms)!\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << "   CudaContextRegistry Logic Unit Test  \n";
    std::cout << "========================================\n";
    TestBasicRegistrationAndIsolation();
    TestConcurrentStress();
    std::cout << "All Unit Tests Passed Successfully!\n";
    return 0;
}

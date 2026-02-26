#include <logger.hpp>
#include <mrpc/client.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <bitset>
#include <atomic>
#include <vector>
#include <condition_variable>
using namespace std::chrono_literals;
using namespace mrpc;

std::atomic<uint64_t> total_requests = 0;
std::atomic<uint64_t> success_requests = 0;
std::atomic<uint64_t> total_latency = 0;
std::atomic<uint64_t> global_request_id = 0;
std::atomic<bool> should_stop = false;

void test_worker(std::shared_ptr<connection> conn) {
    while (!should_stop.load()) {
        auto ret = conn->call<int>("test_add",1,1 );
        total_requests++;
    }
}
int main(int argc, char* argv[]) {
    auto& client = mrpc::client::get();
    client.run();

    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " <concurrency> <duration_seconds>" << std::endl;
        return 1;
    }

    int concurrency = std::stoi(argv[1]);
    int duration_seconds = std::stoi(argv[2]);

    auto conn = client.connect("127.0.0.1", 3333);
    if (!conn) {
        std::cerr << "Failed to connect to server" << std::endl;
        return 1;
    }
    std::vector<std::thread> threads;
    auto start_time = std::chrono::steady_clock::now();

    for (int i = 0; i < concurrency; ++i) {
        threads.emplace_back(test_worker, conn);
    }
    std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
    should_stop = true;


    std::cout << "----------------------------------------" << std::endl;
    std::cout << "Test completed!" << std::endl;
    std::cout << "Total duration: " << duration_seconds << " seconds" << std::endl;
    std::cout << "Total requests: " << total_requests.load() << std::endl;
    uint64_t avg_latency = total_requests.load() ? total_latency.load() / total_requests.load() : 0;
    double qps = duration_seconds > 0 ? total_requests.load() / duration_seconds : 0;
    
    std::cout << "Final QPS: " << qps << std::endl;
    std::cout << "Average latency: " << avg_latency << " us" << std::endl;

    client.shutdown();
    return 0;
}
#include <logger.hpp>
#include <mrpc/client.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <bitset>
#include <atomic>
#include <vector>
#include <future>
using namespace std::chrono_literals;
using namespace mrpc;

std::atomic<uint64_t> total_requests = 0;
std::atomic<uint64_t> success_requests = 0;
std::atomic<uint64_t> total_latency = 0;

std::string make_big_string(size_t size_in_bytes) {
    std::string s;
    s.reserve(size_in_bytes);
    s.resize(size_in_bytes);
    std::fill_n(s.begin(), s.size(), 'a');
    return s;
}

auto big_str = make_big_string(1024);

void test_worker_async_callback(int conn_id, int requests_per_conn) {
    auto& client = mrpc::client::get();
    auto conn = client.connect("127.0.0.1", 3333);
    if (!conn) return;

    std::atomic<int> completed_requests = 0;
    std::mutex mtx;
    std::vector<std::chrono::steady_clock::time_point> start_times;
    start_times.reserve(requests_per_conn);

    for (int i = 0; i < requests_per_conn; ++i) {
        start_times.push_back(std::chrono::steady_clock::now());
        int request_id = i;  // 在循环内保存当前请求ID
        
        conn->async_call<MSG_FMT_JSON>(
            [request_id, &completed_requests, &start_times](uint32_t err_code, const std::string& err_msg, const nlohmann::json& ret) {
                try {
                    auto end = std::chrono::steady_clock::now();
                    total_requests++;
                    std::cout << "[Callback] Request " << request_id << " completed, err_code: " << err_code << std::endl;
                    if (err_code == mrpc::ok) {
                        success_requests++;
                        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start_times[request_id]).count();
                        total_latency += latency;
                    } else {
                        std::cout << "[Callback] Request " << request_id << " failed: " << err_msg << std::endl;
                    }
                    
                    completed_requests++;
                    std::cout << "[Callback] Completed: " << completed_requests.load() << "/" << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "[Callback] Exception in request " << request_id << ": " << e.what() << std::endl;
                }
            },
            "other_echo", 
            std::to_string(i)
        );
    }

    while (completed_requests.load() < requests_per_conn) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " <concurrency> <requests_per_conn>" << std::endl;
        return 1;
    }

    auto& client = mrpc::client::get();
    client.run();

    int concurrency = std::stoi(argv[1]);
    int requests_per_conn = std::stoi(argv[2]);

    std::vector<std::thread> threads;
    auto start_time = std::chrono::steady_clock::now();

    for (int i = 0; i < concurrency; ++i) {
        threads.emplace_back(test_worker_async_callback, i, requests_per_conn);
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    double total = total_requests.load();
    double success = success_requests.load();
    double avg_latency = total ? total_latency.load() / total : 0;
    double qps = total / (duration / 1000.0);

    std::cout << "=== Async Call Performance Test ===" << std::endl;
    std::cout << "Concurrency: " << concurrency << std::endl;
    std::cout << "Total requests: " << total << std::endl;
    std::cout << "Success requests: " << success << std::endl;
    std::cout << "Error rate: " << (total - success) * 100.0 / total << "%" << std::endl;
    std::cout << "QPS: " << qps << std::endl;
    std::cout << "Average latency: " << avg_latency << " us" << std::endl;
    std::cout << "Duration: " << duration << " ms" << std::endl;

    client.shutdown();
    return 0;
}
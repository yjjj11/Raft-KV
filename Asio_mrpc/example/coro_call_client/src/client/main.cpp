#include <logger.hpp>
#include <mrpc/client.hpp>
#include <mrpc/coroutine.hpp>

#include <iostream>
#include <thread>
#include <chrono>
using namespace std::chrono_literals;
using namespace mrpc;

task<uint32_t> test_coro1(int ) {
    auto starts = std::chrono::steady_clock::now();
    // auto total =  client::get().coro_call<uint32_t>("test_mul", 4, 4);
    //  auto total1 =  client::get().coro_call<uint32_t>("test_mul", 4, 4);

    //  co_await total;
    //  co_await total1;
    std::vector<task_awaitable<uint32_t>> tasks;
    for(int i = 0; i < 10; i++){
        auto total =  client::get().coro_call<uint32_t>("test_mul", 4, 4);
        
        tasks.push_back(total);
    }
    //用来测试的函数执行时间一定要长，否则返回值，也就是on_response返回时，
    // 协程还没有开始co_await,也就是未绑定req_id，导致回来的消息找不到对应回调
    for(auto& task : tasks){
        co_await task;

        //第一个co_await开始调用的时间肯定小于rpc调用的时间，但是第二个co_await开始的时候也就是第一个co_await结束的时候
        //这个时候如果再去注册就会失败，我们应该把注册逻辑移到suspend外面去
    }

    auto ends = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(ends - starts);
    LOG_DEBUG("\ncoroutine1 total time: {} ms\n", duration.count());
    co_return 16;
}
int main(int argc, char* argv[]){
    wlog::logger::get().init("logs/" PROJECT_NAME ".log");
    
    client::get().run();
    int conn_count = std::stoi(argv[1]);
    int request_count = std::stoi(argv[2]);
    test_coro1(request_count);

    client::get().wait_shutdown();
    wlog::logger::get().shutdown();
    return 0;
}
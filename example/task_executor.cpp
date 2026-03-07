#include "task_scheduler.hpp"
#include <atomic>
#include <thread>
#include <iostream>
#include "excutor.hpp"
using namespace mrpc;
std::atomic<bool> g_running{true};
RaftNode* g_node = nullptr;

void signal_handler(int signal) {
    g_running = false;
    if (g_node) {
        g_node->stop();
    }
    exit(0);
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    auto node = initialize_server(argc, argv);
    g_node = node.get();
    auto kv_service = std::make_shared<KvService>(node);
    TaskScheduler scheduler(kv_service, node); 

    system("clear");

    TaskExecutor executor1(kv_service, "executor_0");
    TaskExecutor executor2(kv_service, "executor_1");
    TaskExecutor executor3(kv_service, "executor_2");
    executor1.register_task_handler("data_process", [](const TaskPayload& payload) {
        auto json=nlohmann::json::parse(payload);
        std::cout << "✅ Executor_0 Processing data task: " << json["data"] << std::endl;
        return true;
    });
    executor2.register_task_handler("data_process", [](const TaskPayload& payload) {
        auto json=nlohmann::json::parse(payload);
        std::cout << "✅ Executor_1 Processing data task: " << json["data"] << std::endl;
        return true;
    });
    executor3.register_task_handler("data_process", [](const TaskPayload& payload) {
        auto json=nlohmann::json::parse(payload);
        std::cout << "✅ Executor_2 Processing data task: " << json["data"] << std::endl;
        return true;
    });

    executor1.start();
    executor2.start();
    executor3.start();
    std::cout<<"✅ 所有执行器已启动！"<<std::endl;
    while(g_running);
    executor1.stop();
    executor2.stop();
    executor3.stop();
    std::cout<<"✅ 所有执行器已停止！"<<std::endl;
}
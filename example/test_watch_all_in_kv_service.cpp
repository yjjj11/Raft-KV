#include "raftnode.hpp"
#include <signal.h>
#include <iostream>
#include "kv_store.hpp"
// 全局标志，用于通知主线程退出
using namespace mrpc;
std::atomic<bool> g_running{true};
RaftNode* g_node = nullptr;
// 信号处理函数
void signal_handler(int signal) {
    g_running = false;
        if (g_node) {
            g_node->stop();
        }
        exit(0);
}

int main(int argc, char* argv[]) {
     // 注册信号处理函数，用于捕获 SIGINT (Ctrl+C) 和 SIGTERM
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    auto node = initialize_server(argc, argv);
    g_node = node.get();
    KvService service(node);
    std::string key, value;
    int choice = 0;

    std::cout << "Raft-based KV Store Client Started." << std::endl;
    
    service.WATCH("put", [](const std::string& key, const std::string& value) -> bool {
        std::cout << "键值对已设置: " << key << " -> " << value << std::endl;
        return true;
    });

    std::cout<<"按下执行put操作"<<std::endl;
    std::cin.get();
    service.Put("key1", "value1");
    while(1);
   spdlog::debug("Raft node shutdown complete.");
}
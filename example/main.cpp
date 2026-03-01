#include "raftnode.hpp"
#include <signal.h>
// 全局标志，用于通知主线程退出
using namespace mrpc;
std::atomic<bool> g_running{true};
static RaftNode* g_raft_node = nullptr;
// 信号处理函数
void signal_handler(int signal) {
    if (g_raft_node) {
        g_raft_node->stop();
    }
    // std::cout << "\nReceived signal " << signal << ", shutting down gracefully..." << std::endl;
    g_running = false;
}

int main(int argc, char* argv[]) {
     // 注册信号处理函数，用于捕获 SIGINT (Ctrl+C) 和 SIGTERM
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    auto node = initialize_server(argc, argv);
    g_raft_node = node.get();
        
        // spdlog::debug("Raft node started successfully!");
        
        // 保持主线程运行，直到收到退出信号
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    spdlog::debug("Raft node shutdown complete.");


    return 0;
}
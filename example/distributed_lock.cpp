#include "raftnode.hpp"
#include <signal.h>
#include <iostream>
#include <map>
#include <chrono>

struct LockInfo {
    std::string owner;
    int64_t expire_at;
    bool is_locked = false;
};

class DistributedLockManager {
public:
    bool is_locked(const std::string& name) {
        auto it = locks_.find(name);
        if (it == locks_.end() || !it->second.is_locked) return false;
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return it->second.expire_at == 0 || it->second.expire_at > now;
    }

    std::string get_holder(const std::string& name) {
        return is_locked(name) ? locks_[name].owner : "None";
    }

    void try_lock(const std::string& name, const std::string& owner, int64_t now, int64_t ttl) {
        auto& lock = locks_[name];
        if (lock.is_locked && (lock.expire_at == 0 || lock.expire_at > now)) {
            spdlog::warn("[LockManager] '{}' LOCK FAILED: already held by '{}'", name, lock.owner);
            return;
        }
        lock = {owner, ttl == 0 ? 0 : now + ttl, true};
        spdlog::warn("[LockManager] '{}' LOCK SUCCESS by '{}' (TTL: {}ms)", name, owner, ttl);
    }

    void try_unlock(const std::string& name, const std::string& owner) {
        auto it = locks_.find(name);
        if (it != locks_.end() && it->second.is_locked && it->second.owner == owner) {
            it->second.is_locked = false;
            spdlog::warn("[LockManager] '{}' UNLOCK SUCCESS by '{}'", name, owner);
        }
    }
    std::map<std::string, LockInfo> locks_;
};

DistributedLockManager g_lock_manager;
std::atomic<bool> g_running{true};
RaftNode* g_node = nullptr;

void signal_handler(int) {
    g_running = false;
    if (g_node) g_node->stop();
    exit(0);
}

int keep_trying_lock(int64_t timeout_ms) {
    auto start_time = std::chrono::steady_clock::now();
    std::cout << "\n--------------------正在尝试获取锁--------------------\n";
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto entry=g_node->pack_logentry("LOCK", "distributed_resource_lock", std::to_string(g_node->node_id_), now, 10000);
    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
        if (elapsed > timeout_ms) return -1;
        
        // 如果当前没有被锁定，或者锁定者不是自己（且已过期），尝试竞争
        if (g_lock_manager.get_holder("distributed_resource_lock") != std::to_string(g_node->node_id_)) {
            int64_t req_id = g_node->submit(entry);
            if (req_id != -1) {
                if (g_lock_manager.get_holder("distributed_resource_lock") == std::to_string(g_node->node_id_)) {
                    std::cout << "\n--------------------成功获取锁, 开始执行5s任务--------------------\n";
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    
                    std::cout << "\n--------------------任务执行完成, 准备释放锁--------------------\n";
                    auto unlock_entry=g_node->pack_logentry("UNLOCK", "distributed_resource_lock", std::to_string(g_node->node_id_));
                    int64_t unl_req_id = g_node->submit(unlock_entry);
                    if (unl_req_id != -1) return 0; 
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 避免过于频繁的重试请求
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <client_id> <node_id> <ip> <port> ..." << std::endl;
        return 1;
    }
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    auto node = initialize_server(argc, argv);
    g_node = node.get();
    node->callback_reg.reg_callback("LOCK", &g_lock_manager, &DistributedLockManager::try_lock);
    node->callback_reg.reg_callback("UNLOCK", &g_lock_manager, &DistributedLockManager::try_unlock);

    std::this_thread::sleep_for(std::chrono::seconds(3));
    system("clear");

    int ret = keep_trying_lock(20000); // 20s 获取锁超时
    
    if (ret == 0) {
        std::cout << "\n>>> 全流程执行成功并已释放锁。\n";
    } else {
        std::cout << "\n>>> 获取锁超时（15s），程序退出。\n";
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
    g_node->stop();
    return 0;
}

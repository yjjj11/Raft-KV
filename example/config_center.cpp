#include "raftnode.hpp"
#include <signal.h>
#include <iostream>
#include <map>
#include <set>
#include <functional>

std::atomic<bool> g_running{true};
RaftNode* g_node = nullptr;

struct ConfigItem {
    std::string value;
    int64_t version;
    int64_t timestamp;
};

class ConfigCenter {
public:
    std::string GET(const std::string& key) {
        auto it = configs_.find(key);
        return it != configs_.end() ? it->second.value : "";
    }

    int64_t GET_VERSION(const std::string& key) {
        auto it = configs_.find(key);
        return it != configs_.end() ? it->second.version : -1;
    }

    void LIST() {
        if (configs_.empty()) {
            std::cout << "配置中心为空" << std::endl;
            return;
        }
        std::cout << "\n========== 配置列表 ==========" << std::endl;
        for (const auto& [key, item] : configs_) {
            std::cout << "Key: " << key << std::endl;
            std::cout << "  Value: " << item.value << std::endl;
            std::cout << "  Version: " << item.version << std::endl;
            std::cout << "  Updated: " << item.timestamp << std::endl;
            std::cout << "------------------------------" << std::endl;
        }
    }

    void WATCH_SET(const std::string& key, std::function<void(const std::string&, const std::string&)> callback) {
        set_watchers_[key].insert(callback);
    }

    void WATCH_DELETE(const std::string& key, std::function<void(const std::string&)> callback) {
        delete_watchers_[key].insert(callback);
    }

    void apply_set(const std::string& key, const std::string& value, int64_t timestamp) {
        auto& item = configs_[key];
        item.value = value;
        item.version++;
        item.timestamp = timestamp;

        spdlog::warn("[ConfigCenter] SET: {} = {} (version: {})", key, value, item.version);

        if (set_watchers_.count(key)) {
            for (auto& cb : set_watchers_[key]) {
                cb(key, value);
            }
        }
    }

    void apply_delete(const std::string& key) {
        auto it = configs_.find(key);
        if (it != configs_.end()) {
            spdlog::warn("[ConfigCenter] DELETE: {} (was version: {})", key, it->second.version);
            configs_.erase(it);
            
            // 触发删除操作的watch通知
            if (delete_watchers_.count(key)) {
                for (auto& cb : delete_watchers_[key]) {
                    cb(key);
                }
            }
        }
    }

private:
    std::map<std::string, ConfigItem> configs_;
    std::map<std::string, std::set<std::function<void(const std::string&, const std::string&)>>> set_watchers_;
    std::map<std::string, std::set<std::function<void(const std::string&)>>> delete_watchers_;
};

ConfigCenter g_config_center;

void signal_handler(int) {
    g_running = false;
    if (g_node) g_node->stop();
    exit(0);
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    auto node = initialize_server(argc, argv);
    g_node = node.get();

    node->set_apply_callback([&](int32_t log_index, const LogEntry& entry) -> bool {
        if (entry.command_type == "SET_CONFIG") {
            g_config_center.apply_set(entry.key, entry.value, entry.timestamp);
        } else if (entry.command_type == "DELETE_CONFIG") {
            g_config_center.apply_delete(entry.key);
        }
        g_node->lock_store_[entry.req_id].set_value(true);
        return true;
    });

    g_config_center.WATCH_SET("database", [](const std::string& key, const std::string& value) {
        std::cout << "数据库地址已更新: " << value << std::endl;
    });

    g_config_center.WATCH_DELETE("database", [](const std::string& key) {
        std::cout << "数据库地址已删除: " << key << std::endl;
    });
    
    std::cout << "-----------------------------准备加载操作页面------------------------------------" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    while (g_running) {
        system("clear");
        std::cout << "\n=====================================" << std::endl;
        std::cout << "          配置中心操作界面              " << std::endl;
        std::cout << "=====================================" << std::endl;
        std::cout << "  1. SET    - 设置配置项              " << std::endl;
        std::cout << "  2. GET    - 查询配置项              " << std::endl;
        std::cout << "  3. DELETE - 删除配置项              " << std::endl;
        std::cout << "  4. LIST   - 列出所有配置项          " << std::endl;
        std::cout << "  5. VERSION- 查询配置版本            " << std::endl;
        std::cout << "  0. EXIT   - 退出操作界面            " << std::endl;
        std::cout << "=====================================\n\n" << std::endl;

        std::cout << "请输入操作编号(0-5):";
        int op;
        if (!(std::cin >> op)) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << "输入无效，请输入数字！" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        std::string key, value;
        switch (op) {
            case 1: {
                std::cout << "请输入配置项Key: ";
                std::cin >> key;
                std::cout << "请输入配置项Value: ";
                std::cin.ignore();
                std::getline(std::cin, value);
                LogEntry entry{0, key, value, "SET_CONFIG"};
                int64_t req_id = g_node->submit(entry);
                if (req_id != -1) {
                    std::cout << "\n✓ 配置项设置成功！" << std::endl;
                } else {
                    std::cout << "\n✗ 配置项设置失败！（可能不是Leader）" << std::endl;
                }
                std::cin.ignore();
                std::cin.get();
                break;
            }
            case 2: {
                std::cout << "请输入要查询的Key: ";
                std::cin >> key;
                value = g_config_center.GET(key);
                if (!value.empty()) {
                    std::cout << "\n配置项值: " << value << std::endl;
                    std::cout << "版本号: " << g_config_center.GET_VERSION(key) << std::endl;
                } else {
                    std::cout << "\n配置项不存在！" << std::endl;
                }
                std::cout << "\n按回车键继续...";
                std::cin.ignore();
                std::cin.get();
                break;
            }
            case 3: {
                std::cout << "请输入要删除的Key: ";
                std::cin >> key;
                LogEntry entry{0, key, "Null", "DELETE_CONFIG"};
                int64_t req_id = g_node->submit(entry);
                if (req_id != -1) {
                    std::cout << "\n✓ 配置项删除成功！" << std::endl;
                } else {
                    std::cout << "\n✗ 配置项删除失败！（可能不是Leader）" << std::endl;
                }
                std::cout << "\n按回车键继续...";
                std::cin.ignore();
                std::cin.get();
                break;
            }
            case 4: {
                g_config_center.LIST();
                std::cout << "\n按回车键继续...";
                std::cin.ignore();
                std::cin.get();
                break;
            }
            case 5: {
                std::cout << "请输入要查询版本的Key: ";
                std::cin >> key;
                int64_t version = g_config_center.GET_VERSION(key);
                if (version >= 0) {
                    std::cout << "\n配置项版本: " << version << std::endl;
                } else {
                    std::cout << "\n配置项不存在！" << std::endl;
                }
                std::cout << "\n按回车键继续...";
                std::cin.ignore();
                std::cin.get();
                break;
            }
            case 0: {
                std::cout << "退出配置中心..." << std::endl;
                g_running = false;
                break;
            }
            default: {
                std::cout << "无效的操作编号！" << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                break;
            }
        }
    }

    g_node->stop();
    return 0;
}

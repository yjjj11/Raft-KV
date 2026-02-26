#ifndef ZOOKEEPERUTIL_HPP
#define ZOOKEEPERUTIL_HPP

#include <string>
#include <zookeeper/zookeeper.h>
#include <semaphore.h>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <vector>
#include <unordered_map>
#include <functional>

// 日志宏定义（仅声明，实现不在这里）
#ifndef LOG_INFO
#define LOG_INFO(fmt, ...) do { \
    char buffer[256]; \
    snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__); \
    std::cout << "[INFO] " << buffer << std::endl; \
} while(0)
#endif

#ifndef LOG_ERROR
#define LOG_ERROR(fmt, ...) do { \
    char buffer[256]; \
    snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__); \
    std::cerr << "[ERROR] " << buffer << std::endl; \
} while(0)
#endif

// 节点变化事件类型枚举
enum class ZkNodeEventType {
    NODE_ADDED,    // 节点新增
    NODE_REMOVED,  // 节点删除
    NODE_CHANGED,  // 节点数据修改
    SESSION_EXPIRED // 会话过期
};

// 全局watcher声明（仅声明，无实现）
void global_watcher(zhandle_t* zh, int type, int state, const char* path, void* watcherCtx);

// 异步操作的回调上下文（仅声明结构体，无实现）
struct ZkAsyncCtx {
    int rc;                     // 操作返回码（ZOK为成功）
    std::string data;           // 存储getData的结果数据
    std::vector<std::string> children; // 存储子节点列表
    sem_t sem;                  // 阻塞等待的信号量
    char path_buf[128];         // 存储create的节点路径
    int path_buf_len;           // path_buf长度
    char data_buf[64];          // 存储getData的数据缓冲区
    int data_buf_len;           // data_buf长度
    
    // 构造函数声明
    ZkAsyncCtx();
    // 析构函数声明
    ~ZkAsyncCtx();
};

class Zookeeperutil
{
public:
    // 节点变化回调函数类型
    using NodeChangeCallback = std::function<void(const std::string&, ZkNodeEventType, const std::string&)>;

    // 构造/析构函数声明
    Zookeeperutil();
    ~Zookeeperutil();

    // 成员函数声明（仅声明，无实现）
    void start();
    void create(std::string path, std::string data, int state);
    std::optional<std::pair<std::string,uint64_t>> getData(std::string path, bool watch = false);
    std::optional<std::vector<std::string>> getServiceList(const std::string& service_path, bool watch = true);

    void set_ip_port(const std::string& ip, uint64_t port);

    // 新增：设置节点变化回调函数
    void setNodeChangeCallback(NodeChangeCallback callback);

    // 内部方法声明
    void refreshServiceListCache(const std::string& path);

private:
    zhandle_t* m_handle;                  // zk客户端句柄
    std::string m_zk_ip = "127.0.0.1";    // ZK IP
    std::string m_zk_port = "2181";       // ZK 端口
public:
    NodeChangeCallback m_change_callback; // 节点变化回调
    std::mutex m_cache_mutex;             // 缓存操作互斥锁
    std::unordered_map<std::string, std::vector<std::string>> m_service_cache; // 服务列表缓存
};

// 异步回调函数仅声明（无实现）
void aexists_completion(int rc, const struct Stat* stat, const void* ctx);
void acreate_completion(int rc, const char* path, const void* ctx);
void aget_completion(int rc, const char* data, int data_len, const struct Stat* stat, const void* ctx);
void achildren_completion(int rc, const struct String_vector* strings, const void* ctx);


extern "C" {
    void zoo_set_log_stream(FILE* stream);
}

#endif  // ZOOKEEPERUTIL_HPP

#ifndef ZOOKEEPERUTIL_HPP
#define ZOOKEEPERUTIL_HPP

#include <string>
#include <zookeeper/zookeeper.h>
#include <semaphore.h>
#include <iostream>
#include <cstdlib>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <vector>
#include <unordered_map>
#include <functional>  // 新增：回调函数类型

// 日志宏定义（兼容原有逻辑）
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

// 全局watcher：处理会话事件和子节点变化事件
void global_watcher(zhandle_t* zh, int type, int state, const char* path, void* watcherCtx);

// 异步操作的回调上下文
struct ZkAsyncCtx {
    int rc;                     // 操作返回码（ZOK为成功）
    std::string data;           // 存储getData的结果数据
    std::vector<std::string> children; // 存储子节点列表
    sem_t sem;                  // 阻塞等待的信号量
    char path_buf[128];         // 存储create的节点路径
    int path_buf_len;           // path_buf长度
    char data_buf[64];          // 存储getData的数据缓冲区
    int data_buf_len;           // data_buf长度
    ZkAsyncCtx() : rc(-1), path_buf_len(128), data_buf_len(64) {
        sem_init(&sem, 0, 0);
    }
    ~ZkAsyncCtx() {
        sem_destroy(&sem);
    }
};

class Zookeeperutil
{
public:
    // 节点变化回调函数类型：参数1=监听路径，参数2=事件类型，参数3=变化的节点名
    using NodeChangeCallback = std::function<void(const std::string&, ZkNodeEventType, const std::string&)>;

    Zookeeperutil();
    ~Zookeeperutil();
    void start();
    void create(std::string path, std::string data, int state);
    std::optional<std::pair<std::string,uint64_t>> getData(std::string path, bool watch = false);
    // 增强：支持注册监听（watch=true），返回缓存的节点列表
    std::optional<std::vector<std::string>> getServiceList(const std::string& service_path, bool watch = true);

    void set_ip_port(const std::string& ip, uint64_t port) {
        m_zk_ip = ip;
        m_zk_port = std::to_string(port);
    }

    // 新增：设置节点变化回调函数（外部自定义处理逻辑）
    void setNodeChangeCallback(NodeChangeCallback callback) {
        m_change_callback = std::move(callback);
    }

public:
    // 内部方法：重新拉取指定路径的子节点并更新缓存
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

// 全局watcher实现：处理会话事件 + 子节点变化事件
void global_watcher(zhandle_t* zh, int type, int state, const char* path, void* watcherCtx) {
    Zookeeperutil* zk_util = static_cast<Zookeeperutil*>(watcherCtx);
    if (!zk_util) return;

    // 1. 会话事件处理
    if (type == ZOO_SESSION_EVENT) {
        if (state == ZOO_CONNECTED_STATE) {
            LOG_INFO("ZK session connected success!");
        } else if (state == ZOO_EXPIRED_SESSION_STATE) {
            LOG_ERROR("ZK session expired!");
            if (zk_util->m_change_callback) {
                zk_util->m_change_callback("", ZkNodeEventType::SESSION_EXPIRED, "");
            }
        }
        return;
    }

    // 2. 子节点变化事件（服务上下线核心事件）
    if (type == ZOO_CHILD_EVENT && path != nullptr) {
        LOG_INFO("【ZK节点变化】path = {}，服务已上线/下线", path);
        
        // 先刷新缓存，再重新注册watcher（ZK watcher一次性，必须重注册）
        zk_util->refreshServiceListCache(path);

        // 触发客户端回调，通知更新列表
        if (zk_util->m_change_callback) {
            zk_util->m_change_callback(std::string(path), ZkNodeEventType::NODE_REMOVED, "");
        }
    }
}

// 异步exists回调
void aexists_completion(int rc, const struct Stat* stat, const void* ctx) {
    ZkAsyncCtx* async_ctx = const_cast<ZkAsyncCtx*>(static_cast<const ZkAsyncCtx*>(ctx));
    async_ctx->rc = rc;
    sem_post(&async_ctx->sem);
}

// 异步create回调
void acreate_completion(int rc, const char* path, const void* ctx) {
    ZkAsyncCtx* async_ctx = const_cast<ZkAsyncCtx*>(static_cast<const ZkAsyncCtx*>(ctx));
    async_ctx->rc = rc;
    if (rc == ZOK && path) {
        snprintf(async_ctx->path_buf, async_ctx->path_buf_len, "%s", path);
    }
    sem_post(&async_ctx->sem);
}

// 异步get回调
void aget_completion(int rc, const char* data, int data_len, const struct Stat* stat, const void* ctx) {
    ZkAsyncCtx* async_ctx = const_cast<ZkAsyncCtx*>(static_cast<const ZkAsyncCtx*>(ctx));
    async_ctx->rc = rc;
    if (rc == ZOK && data && data_len > 0) {
        async_ctx->data = std::string(data, data_len);
    }
    sem_post(&async_ctx->sem);
}

// 异步获取子节点列表的回调函数
void achildren_completion(int rc, const struct String_vector* strings, const void* ctx) {
    ZkAsyncCtx* async_ctx = const_cast<ZkAsyncCtx*>(static_cast<const ZkAsyncCtx*>(ctx));
    async_ctx->rc = rc;
    
    if (rc == ZOK && strings != nullptr && strings->count > 0) {
        for (int i = 0; i < strings->count; ++i) {
            if (strings->data[i] != nullptr) {
                async_ctx->children.emplace_back(strings->data[i]);
            }
        }
    }
    sem_post(&async_ctx->sem);
}

// 构造函数
Zookeeperutil::Zookeeperutil() : m_handle(nullptr) {}

// 析构函数
Zookeeperutil::~Zookeeperutil() {
    if (m_handle != nullptr) {
        zookeeper_close(m_handle);
        m_handle = nullptr;
        LOG_INFO("ZK handle closed!");
    }
}

// 初始化ZK连接（修改：将watcherCtx指向当前对象）
void Zookeeperutil::start() {
    std::string connstr = m_zk_ip + ":" + m_zk_port;
    
    // 初始化ZK句柄：watcherCtx改为当前对象，用于事件回调
    m_handle = zookeeper_init(
        connstr.c_str(),
        global_watcher,
        30000,
        nullptr,
        this,  // 关键：将当前对象作为watcher上下文
        0
    );
    
    if (m_handle == nullptr) {
        LOG_ERROR("zookeeper_init error! connect to: {}", connstr);
        exit(EXIT_FAILURE);
    }

    // 等待会话连接成功（简化版：实际生产可加超时）
    LOG_INFO("zookeeper_init success! connect to: {}", connstr);
}

// 创建节点（保持原有逻辑）
void Zookeeperutil::create(std::string path, std::string data, int state) {
    if (m_handle == nullptr) {
        LOG_ERROR("ZK handle is null, call start first!");
        exit(EXIT_FAILURE);
    }

    size_t pos = 1;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string parent_path = path.substr(0, pos);
        ZkAsyncCtx ctx;
        zoo_aexists(m_handle, parent_path.c_str(), 0, aexists_completion, &ctx);
        sem_wait(&ctx.sem);
        if (ctx.rc == ZNONODE) {
            zoo_acreate(
                m_handle,
                parent_path.c_str(),
                "",
                0,
                &ZOO_OPEN_ACL_UNSAFE,
                ZOO_PERSISTENT,
                acreate_completion,
                &ctx
            );
            sem_wait(&ctx.sem);
            if (ctx.rc != ZOK) {
                LOG_ERROR("create parent node failure, rc: {}, path: {}", ctx.rc, parent_path);
                exit(EXIT_FAILURE);
            }
        }
    }

    ZkAsyncCtx ctx;
    zoo_aexists(m_handle, path.c_str(), 0, aexists_completion, &ctx);
    sem_wait(&ctx.sem);

    if (ctx.rc == ZNONODE) {
        zoo_acreate(
            m_handle,
            path.c_str(),
            data.c_str(),
            data.size(),
            &ZOO_OPEN_ACL_UNSAFE,
            state,
            acreate_completion,
            &ctx
        );
        sem_wait(&ctx.sem);

        if (ctx.rc == ZOK) {
            LOG_INFO("ZK function node create success, path: [{}]", path);
        } else {
            LOG_ERROR("znode create failure, rc: [{}], path: [{}]", ctx.rc, path);
            exit(EXIT_FAILURE);
        }
    }
}

std::optional<std::pair<std::string,uint64_t>> Zookeeperutil::getData(std::string path, bool watch) {
        if (m_handle == nullptr) {
            LOG_ERROR("ZK handle is null, call start first!");
            return std::nullopt;
        }
        ZkAsyncCtx ctx;
        
        // 异步获取数据：watch=1则注册节点数据变化监听
        zoo_aget(
            m_handle,
            path.c_str(),
            watch ? 1 : 0,
            aget_completion,
            &ctx
        );

        // ========== 核心修改：设置 50ms 超时 ==========
        // 1. 计算超时时间点（当前时间 + 50ms）
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000 * 1000;  // 50ms = 50 * 10^6 纳秒
        // 处理纳秒溢出（1秒 = 10^9 纳秒）
        if (ts.tv_nsec >= 1000 * 1000 * 1000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000 * 1000 * 1000;
        }

        // 2. 带超时等待信号量
        int sem_rc = sem_timedwait(&ctx.sem, &ts);
        if (sem_rc == -1) {
            if (errno == ETIMEDOUT) {
                LOG_ERROR("get znode timeout (50ms), path: {}", path);
            } else {
                LOG_ERROR("sem_timedwait error, errno: {}, path: {}", errno, path);
            }
            return std::nullopt;
        }

        // ========== 原有逻辑保留 ==========
        if (ctx.rc != ZOK) {
            LOG_ERROR("get znode error, rc: {}, path: {}", ctx.rc, path);
            return std::nullopt;
        } else {
            size_t pos = ctx.data.find(':');
            if (pos == std::string::npos) {
                LOG_ERROR("invalid data format, no ':' found: {}", ctx.data);
                return std::nullopt;
            }
            std::string ip = ctx.data.substr(0, pos);
            uint64_t port = std::stoull(ctx.data.substr(pos + 1));
            return std::make_pair(ip,port);
        }
    }

// 内部方法：刷新指定路径的子节点缓存并重新注册watcher
void Zookeeperutil::refreshServiceListCache(const std::string& path) {
    if (m_handle == nullptr) return;

    ZkAsyncCtx ctx;
    // 异步获取子节点 + 注册watcher（第二个参数=1）
    zoo_aget_children(
        m_handle,
        path.c_str(),
        1,  // 关键：注册一次性watcher，节点变化时触发ZOO_CHILD_EVENT
        achildren_completion,
        &ctx
    );

    sem_wait(&ctx.sem);

    if (ctx.rc == ZOK) {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        m_service_cache[path] = ctx.children;  // 更新缓存
        LOG_INFO("Refresh service cache for path: {}, node count: {}", path, ctx.children.size());
    } else {
        LOG_ERROR("Refresh service cache failed, rc: {}, path: {}", ctx.rc, path);
    }
}

// 增强版：拉取服务列表 + 自动监听 + 本地缓存
std::optional<std::vector<std::string>> Zookeeperutil::getServiceList(const std::string& service_path, bool watch) {
    if (m_handle == nullptr) {
        LOG_ERROR("ZK handle is null, call start first!");
        return std::nullopt;
    }

    // 1. 如果开启监听，先刷新缓存（首次拉取+注册watcher）
    if (watch) {
        refreshServiceListCache(service_path);
    }

    // 2. 从缓存读取数据（线程安全）
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    auto it = m_service_cache.find(service_path);
    if (it != m_service_cache.end()) {
        return it->second;
    }

    // 3. 未开启监听时，直接拉取（不更新缓存）
    ZkAsyncCtx ctx;
    zoo_aget_children(
        m_handle,
        service_path.c_str(),
        0,
        achildren_completion,
        &ctx
    );
    sem_wait(&ctx.sem);

    if (ctx.rc != ZOK) {
        LOG_ERROR("get service list error, rc: {}, path: {}", ctx.rc, service_path);
        return std::nullopt;
    }

    return ctx.children;
}

#endif  // ZOOKEEPERUTIL_HPP
#include "ZookeeperUtil.hpp"
#include <iostream>
#include <cstdlib>
#include <ctime>
#include <cerrno>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>
// -------------------- ZkAsyncCtx 实现 --------------------
ZkAsyncCtx::ZkAsyncCtx() : rc(-1), path_buf_len(128), data_buf_len(64) {
    sem_init(&sem, 0, 0);
}

ZkAsyncCtx::~ZkAsyncCtx() {
    sem_destroy(&sem);
}

// -------------------- 全局watcher实现 --------------------
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

// -------------------- 异步回调函数实现 --------------------
void aexists_completion(int rc, const struct Stat* stat, const void* ctx) {
    ZkAsyncCtx* async_ctx = const_cast<ZkAsyncCtx*>(static_cast<const ZkAsyncCtx*>(ctx));
    async_ctx->rc = rc;
    sem_post(&async_ctx->sem);
}

void acreate_completion(int rc, const char* path, const void* ctx) {
    ZkAsyncCtx* async_ctx = const_cast<ZkAsyncCtx*>(static_cast<const ZkAsyncCtx*>(ctx));
    async_ctx->rc = rc;
    if (rc == ZOK && path) {
        snprintf(async_ctx->path_buf, async_ctx->path_buf_len, "%s", path);
    }
    sem_post(&async_ctx->sem);
}

void aget_completion(int rc, const char* data, int data_len, const struct Stat* stat, const void* ctx) {
    ZkAsyncCtx* async_ctx = const_cast<ZkAsyncCtx*>(static_cast<const ZkAsyncCtx*>(ctx));
    async_ctx->rc = rc;
    if (rc == ZOK && data && data_len > 0) {
        async_ctx->data = std::string(data, data_len);
    }
    sem_post(&async_ctx->sem);
}

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

// -------------------- Zookeeperutil 类实现 --------------------
Zookeeperutil::Zookeeperutil() : m_handle(nullptr) {}

Zookeeperutil::~Zookeeperutil() {
    if (m_handle != nullptr) {
        zookeeper_close(m_handle);
        m_handle = nullptr;
        LOG_INFO("ZK handle closed!");
    }
}

void Zookeeperutil::set_ip_port(const std::string& ip, uint64_t port) {
    m_zk_ip = ip;
    m_zk_port = std::to_string(port);
}

void Zookeeperutil::setNodeChangeCallback(NodeChangeCallback callback) {
    m_change_callback = std::move(callback);
}

// 初始化ZK连接
void Zookeeperutil::start() {
    setenv("ZOO_LOG_LEVEL", "ERROR", 1);

    FILE* null_stream = fopen("/dev/null", "w");
    if (null_stream != nullptr) {
        zoo_set_log_stream(null_stream); // 把ZooKeeper日志定向到空设备
    }
    
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
        LOG_ERROR("zookeeper_init error! connect to: {}", connstr.c_str());
        exit(EXIT_FAILURE);
    }

    // 等待会话连接成功（简化版：实际生产可加超时）
    LOG_INFO("zookeeper_init success! connect to: {}", connstr.c_str());
}


void Zookeeperutil::create(std::string path, std::string data, int state) {
    if (m_handle == nullptr) {
        LOG_ERROR("ZK handle is null, call start first!");
        return; // 改为return而非exit，避免程序直接终止
    }

    size_t pos = 1;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string parent_path = path.substr(0, pos);
        ZkAsyncCtx ctx;
        zoo_aexists(m_handle, parent_path.c_str(), 0, aexists_completion, &ctx);
        sem_wait(&ctx.sem);
        
        // 关键修复：只在节点不存在时才创建，节点已存在则跳过
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
                LOG_ERROR("create parent node failure, rc: {}, path: {}", ctx.rc, parent_path.c_str());
                return; // 改为return而非exit
            }
        } else if (ctx.rc != ZOK) {
            LOG_ERROR("check parent node exists failure, rc: {}, path: {}", ctx.rc, parent_path.c_str());
            return; // 改为return而非exit
        }
        // 节点已存在（ctx.rc == ZOK）则直接跳过
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
            LOG_INFO("ZK function node create success, path: [{}]", path.c_str());
        } else {
            LOG_ERROR("znode create failure, rc: [{}], path: [{}]", ctx.rc, path.c_str());
            return; // 改为return而非exit
        }
    } else if (ctx.rc == ZOK) {
        LOG_INFO("ZK node already exists, skip create: [{}]", path.c_str());
    } else {
        LOG_ERROR("check znode exists failure, rc: [{}], path: [{}]", ctx.rc, path.c_str());
    }
}

// 获取节点数据（带50ms超时）
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

    // 核心修改：设置 50ms 超时
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 50 * 1000 * 1000;  // 50ms = 50 * 10^6 纳秒
    // 处理纳秒溢出（1秒 = 10^9 纳秒）
    if (ts.tv_nsec >= 1000 * 1000 * 1000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000 * 1000 * 1000;
    }

    // 带超时等待信号量
    int sem_rc = sem_timedwait(&ctx.sem, &ts);
    if (sem_rc == -1) {
        if (errno == ETIMEDOUT) {
            LOG_ERROR("get znode timeout (50ms), path: {}", path.c_str());
        } else {
            LOG_ERROR("sem_timedwait error, errno: {}, path: {}", errno, path.c_str());
        }
        return std::nullopt;
    }

    // 原有逻辑保留
    if (ctx.rc != ZOK) {
        LOG_ERROR("get znode error, rc: {}, path: {}", ctx.rc, path.c_str());
        return std::nullopt;
    } else {
        size_t pos = ctx.data.find(':');
        if (pos == std::string::npos) {
            LOG_ERROR("invalid data format, no ':' found: {}", ctx.data.c_str());
            return std::nullopt;
        }
        std::string ip = ctx.data.substr(0, pos);
        uint64_t port = std::stoull(ctx.data.substr(pos + 1));
        return std::make_pair(ip,port);
    }
}

// 刷新指定路径的子节点缓存并重新注册watcher
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
        LOG_INFO("Refresh service cache for path: {}, node count: {}", path.c_str(), ctx.children.size());
    } else {
        LOG_ERROR("Refresh service cache failed, rc: {}, path: {}", ctx.rc, path.c_str());
    }
}

// 拉取服务列表 + 自动监听 + 本地缓存
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
        LOG_ERROR("get service list error, rc: {}, path: {}", ctx.rc, service_path.c_str());
        return std::nullopt;
    }

    return ctx.children;
}
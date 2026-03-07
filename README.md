# Raft-RPC Distributed Consensus Framework

本项目是一个基于自定义高性能 RPC 库 (`Asio_mrpc`) 实现的 **通用 Raft 共识协议框架**。它旨在为分布式系统提供一套高可靠、高性能的状态一致性解决方案，使开发者能够轻松地在 Raft 协议之上构建各种分布式业务。

## 🎯 项目定位

不同于单一的分布式应用，本项目核心在于 **Raft 共识引擎的实现**。它提供了一套标准的状态机接口，支持多种分布式场景：
## 🚧 分布式应用实现路线图 (Roadmap)

- [x] **Raft 核心框架实现**
- [x] **[示例应用: 分布式 KV 存储](#kv-store-guide)**
- [x] **[业务扩展: 实现分布式锁服务（支持超时机制）](#distributed-lock-guide)**
- [x] **[业务扩展: 实现分布式配置中心（支持配置监听）](#config-center-guide)**
- [x] **[业务扩展: 实现分布式任务调度系统](#task-scheduler-guide)**
- [ ] **性能优化**: 接入磁盘持久化 (`save_state`/`load_state`)。
- [ ] **功能增强**: 实现日志压缩 (Snapshotting)。

## 🌟 核心特性

- **模块化 Raft 引擎**:
  - **解耦设计**: Raft 核心逻辑与底层通信层、上层业务状态机完全解耦。
  - **高性能通信**: 基于 `Asio_mrpc`（底层使用 Boost.Asio），支持高效的异步 RPC 调用。
  - **标准的 Raft 协议**: 完整实现领导者选举、日志复制、心跳维护及安全性约束。
- **易扩展的状态机接口**: 通过简单的回调机制即可接入不同的业务逻辑。
- **多场景支持**: 项目结构设计支持在 `example/` 下扩展多个独立的分布式业务案例。
- **响应式监听 (WATCH)**: 业务组件可以监听特定前缀的 Key 变化，实现事件驱动的分布式应用。

## 🧠 设计哲学 (Design Philosophy)

在构建本项目时，我们始终遵循分布式系统的核心原则：

1. **确定性 (Determinism)**: 通过 Leader 注入逻辑时间戳，确保分布式状态机在所有节点上产生完全一致的结果，解决了时钟漂移带来的过期逻辑冲突。
2. **共识与业务解耦**: 引入专用的 `apply_thread`。Raft 引擎只负责“达成共识”，而耗时的业务逻辑（如分布式锁的阻塞、任务执行）由独立线程异步处理，确保了 Leader 心跳的稳定性。
3. **极简接入**: 开发者只需实现一个简单的回调函数，即可将单机业务瞬间提升为分布式高可用系统。

## 📂 项目结构

```text
.
├── raftnode.cpp            # Raft 共识引擎核心逻辑实现
├── CMakeLists.txt          # 项目构建配置文件
├── run.sh                  # 一键启动测试脚本
├── include/                # 核心头文件与业务组件定义
│   ├── raftnode.hpp        # Raft 节点定义
│   ├── struct.hpp          # Raft RPC 消息结构体
│   ├── kv_store.hpp        # KV 存储服务组件
│   ├── config_center.hpp   # 配置中心组件
│   ├── task_scheduler.hpp  # 任务调度核心逻辑
│   ├── excutor.hpp         # 任务执行器组件
│   └── scheduler_middle.hpp # 调度器中间层实现
└── example/                # 基于框架构建的分布式业务案例
    ├── node.cpp            # Raft 节点启动基础示例
    ├── kv_store.cpp        # 案例 1: 分布式 KV 存储应用
    ├── distributed_lock.cpp # 案例 2: 分布式锁应用
    ├── config_center.cpp   # 案例 3: 分布式配置中心应用
    ├── task_scheduler_server.cpp # 案例 4: 任务调度服务端
    ├── task_executor.cpp   # 案例 4: 任务执行器应用
    └── task_sys_client.cpp # 案例 4: 任务调度客户端示例
```

## 🛠️ 环境依赖

- **编译器**: 支持 C++17 或更高版本的 GCC/Clang。
- **构建工具**: CMake (>= 3.13)。
- **核心依赖**:
  - `Asio`: 高性能异步网络 IO。
  - `spdlog`: 高性能日志记录库。
  - `nlohmann/json`: 数据序列化。
  - `Threads`: 系统线程支持。

##  Raft 引擎原理

本项目严格遵循 Raft 论文设计，确保集群在网络分区、节点故障等异常情况下的强一致性：
1. **选举机制**: 采用随机超时避免选票瓜分，确保快速选出新领导者。
2. **复制协议**: 日志在多数派节点写入成功后，才会提交至状态机应用。
3. **状态机抽象**: `RaftNode` 通过 `set_apply_callback` 与业务逻辑交互，实现"一次共识，多处应用"。

## 4. 验证 Raft 实现

使用 `run.sh` 脚本管理节点：
```bash
mkdir build && cd build
cmake ..
make -j
./run.sh
```

此脚本提供交互式管理界面，可用于：
- **查看节点状态**：显示当前所有节点的运行状态和PID
- **停止指定节点**：模拟节点故障
- **重启指定节点**：模拟节点恢复
- **退出脚本**：停止所有节点

![Raft节点管理工具界面](./picture/run.sh.png)

通过此工具，您可以：
1. 随意杀死和启动节点
2. 观察日志变化来验证 Raft 协议的容错能力
3. 测试网络分区、节点故障等异常情况下的一致性保证
4. 根据logs/node*.log来查看节点日志，验证Raft协议的正确性

## 🚀 快速开始 (以 KV 存储为例) {#kv-store-guide}

### 1. 启动 KV 存储集群

打开多个终端，分别启动不同节点：

```bash
# 终端 1: 启动节点 0
./bin/kv_store 0 127.0.0.1 8000 2 127.0.0.1:8001 127.0.0.1:8002

# 终端 2: 启动节点 1
./bin/kv_store 1 127.0.0.1 8001 2 127.0.0.1:8000 127.0.0.1:8002

# 终端 3: 启动节点 2
./bin/kv_store 2 127.0.0.1 8002 2 127.0.0.1:8000 127.0.0.1:8001
```

### 3. 交互体验

进入节点交互界面进行分布式 KV 操作：
![KV存储交互界面](./picture/kv_store.png)
----你可以在node0创建键值对，node1查询键值对，node2删除并查询键值对

## 🚀 分布式锁使用指南 {#distributed-lock-guide}

### 1. 启动分布式锁集群

打开多个终端，分别启动不同节点：

```bash
# 终端 1: 启动节点 0
./bin/distributed_lock 0 127.0.0.1 8000 2 127.0.0.1:8001

# 终端 2: 启动节点 1
./bin/distributed_lock 1 127.0.0.1 8001 2 127.0.0.1:8000
```

### 3. 锁操作流程

1. **获取锁**：节点会尝试获取分布式锁，成功后执行耗时任务
2. **锁竞争**：当一个节点持有锁时，其他节点会等待锁释放
3. **自动释放**：锁会在指定的超时时间后自动释放
4. **手动释放**：任务完成后会手动释放锁
![分布式锁交互界面](./picture/distributed_lock.png)
### 4. 查看日志

查看节点日志了解锁操作详情：
```bash
cat logs/raft_node_0.log
```

## 🚀 分布式配置中心使用指南 {#config-center-guide}

### 1. 启动配置中心集群

打开多个终端，分别启动不同节点：

```bash
# 终端 1: 启动节点 0
./bin/config_center 0 127.0.0.1 8000 2 127.0.0.1:8001

# 终端 2: 启动节点 1
./bin/config_center 1 127.0.0.1 8001 2 127.0.0.1:8000
```

### 2. 配置中心功能

配置中心提供以下核心功能：

- **SET**: 设置配置项（支持新增和修改）
- **GET**: 查询配置项的值
- **DELETE**: 删除配置项
- **LIST**: 列出所有配置项
- **VERSION**: 查询配置项的版本号

### 3. 配置监听机制

配置中心支持配置变更监听，使用 `RegisterCallback` 接口注册回调函数：

```cpp
// 使用 lambda 注册配置变更监听器
g_config_center.WATCH_SET("database", [](const std::string& key, const std::string& value) {
    std::cout << "数据库地址已更新: " << value << std::endl;
});

g_config_center.WATCH_DELETE("database", [](const std::string& key) {
    std::cout << "数据库地址已删除: " << key << std::endl;
});
```
![分布式配置中心交互界面](./picture/config_center.png)



## 🚀 分布式任务调度系统使用指南 

### 启动任务调度系统的调度管理节点
```bash
./bin/task_scheduler_server 0 127.0.0.1 8000 1 127.0.0.1:8001 127.0.0.1:8002
./bin/task_scheduler_server 1 127.0.0.1 8001 1 127.0.0.1:8000 127.0.0.1:8002
```
### 启动任务调度系统的任务执行节点
```bash
./bin/task_executor 2 127.0.0.1 8002 1 127.0.0.1:8000 127.0.0.1:8001
```

### 启动系统客户端
```bash
./bin/task_client 
```
## 在客户端提交一个任务后
![任务调度系统客户端界面](./picture/task_sys_client.png)
![任务调度系统服务端界面](./picture/task_scheduler_server.png)
![任务调度系统任务执行节点界面](./picture/task_executor.png)



## 🤝 贡献与反馈

如果你有新的分布式业务场景想法，欢迎通过 Issue 或 Pull Request 提交！
---
*本项目基于个人开发的 [mrpc](https://github.com/yjjj11/Asio_mrpc) 库构建。*

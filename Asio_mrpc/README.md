# mRPC — 现代 C++ Header-only 双向 RPC 框架

一个短小精悍但功能完整的 RPC 框架，融合泛型编程、协程、异步网络 I/O 与服务治理能力。核心代码清晰易读，适合学习与生产演进。

## 技术栈与关键技术
- C++17/20 泛型编程与模板元编程：函数注册/反射调用、`function_traits`、`std::tuple`/`std::apply`、`if constexpr`
- Asio 网络 I/O：Reactor 模型、`asio::io_context` 池、异步读写、定时器
- C++20 协程：`co_await`、自定义 `task`/`promise`，以同步风格编写异步 RPC
- 序列化协议：RAW、JSON、BJSON、UBJSON、MSGPACK、CBOR（基于 `nlohmann::json`）
- ZooKeeper：服务注册与发现、Watcher 监听、客户端本地缓存
- 日志：`spdlog`/`wlog` 集成

## 快速开始
```sh
mkdir build & cd build
cmake ..
make -j
```
生成的可运行文件将会在bin目录下

## 架构与模块
- Server：[server.hpp](include/mrpc/server.hpp) 服务端入口、`io_context` 池与线程池管理、统一函数注册封装、ZooKeeper 注册
- Client：[client.hpp](include/mrpc/client.hpp) 客户端入口、对称的 `io_context` 管理、预留发现与缓存结构
- Connection：[connection.hpp](include/mrpc/connection.hpp) Socket 封装、异步读写、发送队列、响应分发（Future/Callback/Coroutine）
- Router：[router.hpp](include/mrpc/router.hpp) 路由与反射调用、异常回调、协议编解码
- Coroutine：[coroutine.hpp](include/mrpc/coroutine.hpp) 自定义 `task`/`awaitable`，无缝协程调用
- ZooKeeper 工具：[ZookeeperUtil.hpp](include/mrpc/ZookeeperUtil.hpp) 节点创建、数据读取、子节点列表与 Watcher、缓存与回调

消息协议采用固定头（Header）+ 变长体（Body）：
- Header：`msg_type | msg_id | req_id | body_len`
- Body：按 `msg_type` 指定的序列化格式进行编解码

## 功能特性
- 双向 RPC：客户端与服务端地位对等，双方均可注册与被调用
- 多调用风格：同步 `call`、Future 异步 `async_call`、回调异步、协程 `coro_call`、单向通知 `notify`
- 多序列化：按需选择 RAW/JSON/BJSON/UBJSON/MSGPACK/CBOR
- 服务治理：服务端接入 ZooKeeper 进行注册；客户端可读取服务/函数路径做发现与路由
- 线程模型：`io_context` 池 + 工作线程（支持轮询分配），避免业务阻塞读写

## 健壮性与性能
- 发送队列串行化：确保同一连接的写操作顺序与线程安全
- 上行/下行边界检查：消息长度校验、粘包处理、异常关闭统一收敛
- 并发读优化：路由名映射采用共享锁，降低高并发场景中的锁竞争
- 可观测性：统一日志等级与错误码

## 可扩展性
- 注册中心抽象：当前接入 ZooKeeper，可平滑扩展到 etcd/Consul
- 序列化抽象：在 Router 层封装编解码，支持替换更高性能的协议

## 快速开始
- 构建：见根目录 [CMakeLists.txt](CMakeLists.txt)，默认链接 `asio`、`nlohmann/json`、`spdlog`、`zookeeper_mt`（可选）
- 示例：
  - 同步调用：[block_call_client](example/block_call_client)
  - 异步回调：[async_call_client](example/async_call_client)
  - 协程调用：[coro_call_client](example/coro_call_client)
  - 服务端注册与监听：[server_register](example/server_register)
  - 双向 RPC：[two_way_call](example/two_way_call)

## 代码示例
服务端注册与 ZK 接入：
```cpp
auto& svr = mrpc::server::get();
svr.set_ip_port("127.0.0.1", 3333);
svr.set_server_name("test_server");
svr.set_zk_ip_port("127.0.0.1", 2181);
svr.run();
svr.register_to_Zk();
svr.reg_func("test_add", [](mrpc::connection::cptr conn, int i, int j) { return i + j; });
svr.accept();
svr.wait_shutdown();
```

客户端同步调用：
```cpp
auto ret = conn->call<int>("test_add", 11, 12);
if (ret.error_code() == mrpc::ok) { std::cout << ret.value() << std::endl; }
```

客户端协程调用：
```cpp
task<int> t(mrpc::connection::cptr conn) {
  auto r = co_await conn->coro_call<int>("test_add", 1, 1);
  co_return r.value();
}
```

## 可以获得的收获
- 现代 C++ 泛型编程与模板技巧：函数特征、参数解构、编译期条件,重中之重
- 网络异步编程模型：Reactor、`io_context` 池化、队列化写、超时与错误处理
- 协程工程化应用：自定义 awaitable/promise，与网络 I/O 协同 重中之重
- 序列化协议设计与实现：多格式编解码、二进制/文本的取舍
- 服务治理基础：注册/发现/Watcher、缓存一致性、负载与容错策略

## 依赖库
 
    1. asio 1.18
    2. nlohmann/json
    3. spdlog
    4. zookeeper_mt（可选，用于服务注册与发现）

## 目录结构
- include/mrpc：核心头文件（Server/Client/Connection/Router/Coroutine/ZookeeperUtil）
- example：示例程序（同步、异步、协程、服务注册、双向调用）
- third：第三方依赖（asio、nlohmann/json、spdlog、wlog 等）
- bin：构建输出

## 致谢
灵感来源于 `rest_rpc`，在其基础上以现代 C++ 技术栈重构并扩展了工程能力与生态集成。



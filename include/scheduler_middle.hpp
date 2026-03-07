#pragma once
#include <string>
#include <chrono>
#include <thread>
#include <memory>
#include <optional>
#include <atomic>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

// 引入依赖的类型定义（保持与你的环境一致）
#include "kv_store.hpp"
#include <raftnode.hpp>
#include <random>

using json = nlohmann::json;

// 补充必要的类型定义（适配你的环境）
using TaskId = std::string;
using ExecuteTimeMs = int64_t;
using ExecutorId = std::string;

// ========== 1. 任务调度器类（仅负责调度和分发，无执行逻辑） ==========
class TaskScheduler {
public:
    // 构造函数
    TaskScheduler(std::shared_ptr<KvService> kv_service, std::shared_ptr<RaftNode> raft_node) 
        : kv_service_(kv_service),
          raft_node_(raft_node),
          stop_flag_(false),
          scheduler_thread_(nullptr) {
        
        // 注册KV操作监听
        kv_service_->WATCH("put", "task_content_and_status_update", [this](const std::string& key, const std::string& value) {
            if (key.substr(0, 5) == "task:") {
                return this->on_task_updated(key, value);
            } 
            return true;
        });//只注册提交任务后提交状态和更新列表的watch
        spdlog::info("任务调度器 - 回调注册完成");
    }

    ~TaskScheduler() {
        stop();
    }

    // 初始化调度器（仅初始化锁，不再加载任务到内存队列）
    void initialize_scheduler() {
        kv_service_->Put("scheduler:leader_lock", std::to_string(kv_service_->node_id_));
        spdlog::info("任务调度器 - 调度锁初始化获取完成");
    }

    // 判断当前节点是否为Leader
    bool is_leader() const {
        if (!raft_node_) {
            spdlog::warn("任务调度器 - RaftNode未初始化，默认返回非Leader状态");
            return false;
        }
        return raft_node_->is_leader(raft_node_->node_id_);
    }

    // 添加定时任务（仅存储+设置Pending状态，后续状态由执行器修改）
    bool schedule_task(const ScheduledTask& task) {
        try {
            // 检查任务是否已存在

            // 存储任务数据
            json task_json = task;
            int64_t put_result = kv_service_->Put("task:" + task.id, task_json.dump());
            if (put_result == -1) {
                spdlog::error("任务调度器 - 存储任务 {} 失败", task.id);
                return false;
            }

            // 调度器仅负责设置初始Pending状态，后续状态由执行器处理
            TaskStatusInfo status_info;
            status_info.status = TaskStatus::Pending;
            status_info.created_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            status_info.updated_at = status_info.created_at;

            json status_json = status_info;
            int64_t status_result = kv_service_->Put("task_status:" + task.id, status_json.dump());
            if (status_result == -1) {
                spdlog::error("任务调度器 - 存储任务 {} 初始状态失败", task.id);
                return false;
            }

            // 更新任务列表
            update_task_list(task.id, true);
            return true;
        } catch (const std::exception& e) {
            spdlog::error("任务调度器 - 添加任务 {} 异常: {}", task.id, e.what());
            return false;
        }
    }

    // 取消任务
    bool cancel_task(const TaskId& task_id) {
        try {
            // 删除任务数据
            int64_t del_result = kv_service_->Del("task:" + task_id);
            if (del_result == -1) {
                spdlog::error("任务调度器 - 删除任务 {} 失败", task_id);
                return false;
            }

            // 删除任务状态
            del_result = kv_service_->Del("task_status:" + task_id);
            if (del_result == -1) {
                spdlog::error("任务调度器 - 删除任务 {} 状态失败", task_id);
                return false;
            }

            // 从执行器任务列表中移除
            remove_task_from_executor_list(task_id);

            // 更新任务列表
            update_task_list(task_id, false);

            spdlog::info("任务调度器 - 任务 {} 已取消", task_id);
            return true;
        } catch (const std::exception& e) {
            spdlog::error("任务调度器 - 取消任务 {} 异常: {}", task_id, e.what());
            return false;
        }
    }

    // 获取任务状态
    std::optional<TaskStatusInfo> get_task_status(const TaskId& task_id) {
        try {
            std::string status_str = kv_service_->Get("task_status:" + task_id);
            if (status_str.empty()) {
                return std::nullopt;
            }
            return json::parse(status_str).get<TaskStatusInfo>();
        } catch (const std::exception& e) {
            spdlog::error("任务调度器 - 获取任务 {} 状态异常: {}", task_id, e.what());
            return std::nullopt;
        }
    }

    // 启动调度器（仅Leader节点调用）
    void start() {
        if (scheduler_thread_ == nullptr) {
            stop_flag_ = false;
            initialize_scheduler();
            // 启动调度线程（仅负责分发任务）
            scheduler_thread_ = std::make_unique<std::thread>(&TaskScheduler::scheduler_loop, this);
            spdlog::info("任务调度器 - 已启动（当前节点为Leader）");
        } else {
            spdlog::warn("任务调度器 - 已处于运行状态，无需重复启动");
        }
    }

    // 停止调度器
    void stop() {
        stop_flag_ = true;
        
        if (scheduler_thread_ && scheduler_thread_->joinable()) {
            scheduler_thread_->join();
        }
        
        scheduler_thread_.reset();
        spdlog::info("任务调度器 - 已停止");
    }

    // 创建任务（字符串数据）
    ScheduledTask make_task_from_string(const std::string& task_type, 
                            const std::string& task_data, 
                            int64_t delay_seconds = 2,
                            const std::string& task_id = "") {
        ScheduledTask task;
        
        // 生成任务ID
        if (task_id.empty()) {
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(1000, 9999);
            task.id = "task_" + std::to_string(timestamp) + "_" + std::to_string(dis(gen));
        } else {
            task.id = task_id;
        }
        
        // 设置执行时间
        task.execute_at = std::chrono::duration_cast<std::chrono::milliseconds>(
            (std::chrono::system_clock::now() + std::chrono::seconds(delay_seconds)).time_since_epoch()
        ).count();
        
        // 构造payload
        try {
            json payload_json;
            payload_json["type"] = task_type;
            if (!task_data.empty()) {
                if (task_data.front() == '{' && task_data.back() == '}') {
                    payload_json["data"] = json::parse(task_data);
                } else {
                    payload_json["data"] = task_data;
                }
            }
            task.payload = payload_json.dump();
        } catch (const std::exception& e) {
            spdlog::error("任务调度器 - 构造任务 {} payload 失败: {}", task.id, e.what());
            task.payload = "{\"type\":\"" + task_type + "\",\"data\":\"" + task_data + "\"}";
        }
        
        // 默认状态（仅内存标识，实际以KV中Pending为准）
        task.status = TaskStatus::Pending;
        
        spdlog::info("任务调度器 - 创建任务 {} (类型: {}, 延迟: {}s)", task.id, task_type, delay_seconds);
        return task;
    }

    // 创建任务（JSON数据）
    ScheduledTask make_task_from_json(const std::string& task_type, 
                            const json& task_data, 
                            int64_t delay_seconds = 2,
                            const std::string& task_id = "") {
        return make_task_from_string(task_type, task_data.dump(), delay_seconds, task_id);
    }

    // 获取待分发的任务数量（直接从KV读取）
    size_t get_pending_task_count() {
        try {
            std::string tasks_list_str = kv_service_->Get("scheduler:tasks_list");
            if (tasks_list_str.empty()) {
                return 0;
            }
            
            json tasks_list = json::parse(tasks_list_str);
            if (!tasks_list.is_array()) {
                return 0;
            }
            
            size_t count = 0;
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            for (const auto& task_id : tasks_list) {
                TaskId tid = task_id.get<std::string>();
                // 检查任务状态和执行时间
                auto status_opt = get_task_status(tid);
                std::string task_str = kv_service_->Get("task:" + tid);
                
                if (status_opt.has_value() && status_opt.value().status == TaskStatus::Pending && !task_str.empty()) {
                    ScheduledTask task = json::parse(task_str).get<ScheduledTask>();
                    if (task.execute_at <= now) {
                        count++;
                    }
                }
            }
            return count;
        } catch (const std::exception& e) {
            spdlog::error("任务调度器 - 获取待分发任务数量异常: {}", e.what());
            return 0;
        }
    }

private:
    // ========== KV事件回调 ==========
    bool on_task_updated(const std::string& key, const std::string& value) {
        try {
            spdlog::debug("任务调度器 - 任务内容更新: {}", key);
            // 仅记录日志，无状态操作

             return true;
        } catch (const std::exception& e) {
            spdlog::error("任务调度器 - 处理任务更新事件异常: {}", e.what());
            return false;
        }
    }


    // 从执行器任务列表中移除指定任务
    void remove_task_from_executor_list(const TaskId& task_id) {
        // 简单遍历所有执行器（实际场景可优化为存储任务-执行器映射）
        for (int i = 0; i < 1; ++i) { // 假设有3个执行器
            ExecutorId executor_id = "executor_" + std::to_string(i);
            std::string executor_key = "executor:" + executor_id;
            
            kv_service_->Get_lock(executor_key, 2000, 2000);
            try {
                std::string tasks_str = kv_service_->Get(executor_key);
                if (tasks_str.empty()) {
                    kv_service_->Release_lock(executor_key);
                    continue;
                }
                
                json tasks_array = json::parse(tasks_str);
                if (!tasks_array.is_array()) {
                    kv_service_->Release_lock(executor_key);
                    continue;
                }
                
                // 过滤掉目标任务ID
                json new_tasks_array = json::array();
                for (const auto& tid : tasks_array) {
                    if (tid.get<std::string>() != task_id) {
                        new_tasks_array.push_back(tid);
                    }
                }
                
                // 更新执行器任务列表
                kv_service_->Put(executor_key, new_tasks_array.dump());
            } catch (const std::exception& e) {
                spdlog::error("任务调度器 - 从执行器列表移除任务 {} 失败: {}", task_id, e.what());
            }
            kv_service_->Release_lock(executor_key);
        }
    }

    // 调度器主循环（核心：分发任务到执行器）
    void scheduler_loop() {
        spdlog::info("任务调度器 - 调度线程启动，开始分发任务");
        while (!stop_flag_) {
            try {
                distribute_pending_tasks();
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 每100ms检查一次
            } catch (const std::exception& e) {
                spdlog::error("任务调度器 - 调度循环异常: {}", e.what());
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        spdlog::info("任务调度器 - 调度线程停止");
    }

    // 分发待执行的Pending任务（核心逻辑：直接操作KV，不修改状态）
    void distribute_pending_tasks() {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // 获取所有任务列表
        std::string tasks_list_str = kv_service_->Get("scheduler:tasks_list");
        if (tasks_list_str.empty()) {
            return;
        }
        
        json tasks_list = json::parse(tasks_list_str);
        if (!tasks_list.is_array()) {
            spdlog::error("任务调度器 - 任务列表格式错误");
            return;
        }
        
        // 加锁保证分发过程原子性
        kv_service_->Get_lock("executor", 2000, 2000);
        
        // 遍历所有任务，筛选出到达执行时间的Pending任务
        for (const auto& task_id : tasks_list) {
            try {
                TaskId tid = task_id.get<std::string>();
                
                // 1. 检查任务状态（仅处理Pending状态，状态由执行器修改）
                auto status_opt = get_task_status(tid);
                if (!status_opt.has_value() || status_opt.value().status != TaskStatus::Pending) {
                    continue;
                }
                
                // 2. 获取任务信息，检查执行时间
                std::string task_str = kv_service_->Get("task:" + tid);
                if (task_str.empty()) {
                    continue;
                }
                
                ScheduledTask task = json::parse(task_str).get<ScheduledTask>();
                if (task.execute_at > now) {
                    continue; // 执行时间未到
                }
                
                // 3. 分发任务到执行器（不修改任务状态）
                distribute_task_to_executor(tid);
                
            } catch (const std::exception& e) {
                spdlog::error("任务调度器 - 处理任务 {} 分发异常: {}", task_id.get<std::string>(), e.what());
            }
        }
        
        kv_service_->Release_lock("executor"); // 修复原代码拼写错误
    }

    // 分发单个任务到执行器（仅写入executor:{id}数组，不修改任务状态）
    void distribute_task_to_executor(const TaskId& task_id) {
        try {
            // 简单的执行器分配策略：轮询分配
            static std::atomic<int> executor_index = 0;
            int executor_count = 3; // 假设有3个执行器
            ExecutorId executor_id = "executor_" + std::to_string(executor_index++ % executor_count);
            std::string executor_key = "executor:" + executor_id;
            
            // 1. 获取执行器当前的任务列表
            std::string tasks_str = kv_service_->Get(executor_key);
            json tasks_array = json::array();
            
            if (!tasks_str.empty()) {
                tasks_array = json::parse(tasks_str);
                if (!tasks_array.is_array()) {
                    tasks_array = json::array();
                }
            }
            
            // 2. 检查任务是否已存在，避免重复添加
            bool exists = false;
            for (const auto& tid : tasks_array) {
                if (tid.get<std::string>() == task_id) {
                    exists = true;
                    break;
                }
            }
            
            if (!exists) {
                // 3. 添加任务ID到执行器列表（仅分发，不修改状态）
                tasks_array.push_back(task_id);
                int64_t put_result = kv_service_->Put(executor_key, tasks_array.dump());
                update_task_status(task_id, TaskStatus::Executing);
                if (put_result == -1) {
                    spdlog::error("任务调度器 - 分发任务 {} 到执行器 {} 失败", task_id, executor_id);
                    return;
                }
                
                spdlog::info("任务调度器 - 任务 {} 已分发到执行器 {}，执行器任务数: {}", 
                             task_id, executor_id, tasks_array.size());
            }
            
        } catch (const std::exception& e) {
            spdlog::error("任务调度器 - 分发任务 {} 异常: {}", task_id, e.what());
            update_task_status(task_id, TaskStatus::Pending); 
        }
    }

    void update_task_status(const TaskId& task_id, TaskStatus new_status, const std::string& error_msg = "") {
        try {
            // 1. 获取当前状态
            std::string status_str = kv_service_->Get("task_status:" + task_id);
            TaskStatusInfo status_info;
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            if (!status_str.empty()) {
                status_info = json::parse(status_str).get<TaskStatusInfo>();
            } else {
                // 异常情况：状态不存在，初始化基础信息
                status_info.created_at = now;
                status_info.status = TaskStatus::Pending;
            }
            
            // 2. 更新状态信息
            status_info.status = new_status;
            status_info.updated_at = now;
            
            // 状态流转逻辑
            if (new_status == TaskStatus::Executing) {
                status_info.started_at = now; // 记录开始时间
            }
            // 3. 保存到KV
            json status_json = status_info;
            int64_t put_result = kv_service_->Put("task_status:" + task_id, status_json.dump());
            if (put_result == -1) {
                spdlog::error("任务调度器 - 分发任务 {} 状态失败", task_id);
            }
            
        } catch (const std::exception& e) {
            spdlog::error("任务调度器 - 更新任务 {} 状态异常: {}", task_id, e.what());
        }
    }

    // 更新任务列表
    void update_task_list(const TaskId& task_id, bool add) {
        kv_service_->Get_lock("tasklist", 2000, 2000);
        try {
            std::string tasks_list_str = kv_service_->Get("scheduler:tasks_list");
            json tasks_list = json::array();
            
            if (!tasks_list_str.empty()) {
                tasks_list = json::parse(tasks_list_str);
                if (!tasks_list.is_array()) {
                    tasks_list = json::array();
                }
            }
                
            if (add) {
                // 避免重复添加
                bool exists = false;
                for (const auto& id : tasks_list) {
                    if (id.get<std::string>() == task_id) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    tasks_list.push_back(task_id);
                }
            } else {
                // 移除指定任务ID
                json new_list = json::array();
                for (const auto& id : tasks_list) {
                    if (id.get<std::string>() != task_id) {
                        new_list.push_back(id);
                    }
                }
                tasks_list = std::move(new_list);
            }
            
            // 保存到KV
            kv_service_->Put("scheduler:tasks_list", tasks_list.dump());
        } catch (const std::exception& e) {
            spdlog::error("任务调度器 - 更新任务列表失败: {}", e.what());
        }
        kv_service_->Release_lock("tasklist");
    }

   // 成员变量
    std::shared_ptr<KvService> kv_service_;
    std::shared_ptr<RaftNode> raft_node_;
    std::atomic<bool> stop_flag_;
    std::unique_ptr<std::thread> scheduler_thread_;

};


// 测试代码示例
// 1. 创建KV和Raft节点
// auto raft_node = std::make_shared<RaftNode>();
// auto kv_service = std::make_shared<KvService>(raft_node);

// // 2. 创建并启动调度器（仅Leader节点）
// TaskScheduler scheduler(kv_service, raft_node);
// if (scheduler.is_leader()) {
//     scheduler.start();
    
//     // 添加测试任务
//     auto task = scheduler.make_task_from_string("test", "hello world", 1);
//     scheduler.schedule_task(task);
// }

// // 3. 执行器逻辑示例（需自行实现）
// // 执行器需：1.读取executor:{id}获取任务列表 2.修改任务状态 3.执行任务
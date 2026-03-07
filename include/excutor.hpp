#pragma once
#include <string>
#include <chrono>
#include <thread>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <unordered_set>
#include <map>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

// 引入依赖的类型定义
#include "kv_store.hpp"
using json = nlohmann::json;
 
class TaskExecutor {
public:
    // 构造函数
    TaskExecutor(std::shared_ptr<KvService> kv_service, const ExecutorId& executor_id)
        : kv_service_(kv_service),
          executor_id_(executor_id),
          stop_flag_(false),
          worker_thread_(nullptr),
          executor_key_("executor:" + executor_id) { // 执行器任务列表KV键
        spdlog::info("任务执行器 {} - 初始化完成", executor_id_);
    }

    ~TaskExecutor() {
        stop();
    }

    // 注册任务处理器（按任务类型）
    void register_task_handler(const std::string& task_type, std::function<bool(const TaskPayload&)> handler) {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        task_handlers_[task_type] = handler;
        spdlog::info("任务执行器 {} - 注册任务处理器: {}", executor_id_, task_type);
    }

    // 启动执行器（轮询KV获取分配给自己的任务）
    void start() {
        if (worker_thread_ == nullptr) {
            stop_flag_ = false;
            // 启动工作线程
            worker_thread_ = std::make_unique<std::thread>(&TaskExecutor::worker_loop, this);
            spdlog::info("任务执行器 {} - 已启动", executor_id_);
        } else {
            spdlog::warn("任务执行器 {} - 已处于运行状态，无需重复启动", executor_id_);
        }
    }

    // 停止执行器
    void stop() {
        stop_flag_ = true;
        
        if (worker_thread_ && worker_thread_->joinable()) {
            worker_thread_->join();
        }
        
        worker_thread_.reset();
        spdlog::info("任务执行器 {} - 已停止", executor_id_);
    }

    // 获取当前执行器ID
    ExecutorId get_executor_id() const {
        return executor_id_;
    }

private:
    // 工作线程主循环（轮询KV获取任务）
    void worker_loop() {
        spdlog::info("任务执行器 {} - 工作线程启动，开始轮询任务", executor_id_);
        while (!stop_flag_) {
            try {
                // 轮询获取分配给自己的任务
                poll_assigned_tasks();
                std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 每50ms轮询一次
            } catch (const std::exception& e) {
                spdlog::error("任务执行器 {} - 工作循环异常: {}", executor_id_, e.what());
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        spdlog::info("任务执行器 {} - 工作线程停止", executor_id_);
    }

    // 轮询获取分配给自己的任务（核心逻辑）
    void poll_assigned_tasks() {
        // 加锁保证任务处理的原子性
        if (!kv_service_->Get_lock(executor_key_, 2000, 5000)) { // 锁超时1s，等待5s
            spdlog::warn("任务执行器 {} - 获取执行器锁失败，跳过本次轮询", executor_id_);
            return;
        }

        try {
            // 1. 获取分配给当前执行器的任务列表
            std::string tasks_str = kv_service_->Get(executor_key_);
            if (tasks_str.empty()) {
                kv_service_->Release_lock(executor_key_);
                return;
            }
            
            json tasks_array = json::parse(tasks_str);
            if (!tasks_array.is_array()) {
                spdlog::error("任务执行器 {} - 任务列表格式错误: {}", executor_id_, tasks_str);
                kv_service_->Release_lock(executor_key_);
                return;
            }
            
            // 2. 遍历任务列表，逐个处理
            json remaining_tasks = json::array(); // 未完成的任务（需要保留）
            for (const auto& task_id : tasks_array) {
                TaskId tid = task_id.get<std::string>();
                
                // 跳过已处理/正在处理的任务
                {
                    std::lock_guard<std::mutex> lock(processing_mutex_);
                    if (processed_tasks_.count(tid) || processing_tasks_.count(tid)) {
                        remaining_tasks.push_back(tid);
                        continue;
                    }
                }
                
                // 标记为正在处理
                {
                    std::lock_guard<std::mutex> lock(processing_mutex_);
                    processing_tasks_.insert(tid);
                }
                
                // 3. 执行任务
                bool task_completed = execute_assigned_task(tid);
                
                // 4. 任务未完成（执行失败/需要重试），保留到列表中
                if (!task_completed) {
                    remaining_tasks.push_back(tid);
                    
                    // 移除正在处理标记
                    std::lock_guard<std::mutex> lock(processing_mutex_);
                    processing_tasks_.erase(tid);
                } else {
                    // 任务完成，标记为已处理
                    std::lock_guard<std::mutex> lock(processing_mutex_);
                    processed_tasks_.insert(tid);
                    processing_tasks_.erase(tid);
                }
            }
            
            // 5. 更新执行器任务列表（仅保留未完成任务）
            if (remaining_tasks.size() != tasks_array.size()) {
                kv_service_->Put(executor_key_, remaining_tasks.dump());
                spdlog::info("任务执行器 {} - 更新任务列表，剩余任务数: {}", 
                             executor_id_, remaining_tasks.size());
            }
            
        } catch (const std::exception& e) {
            spdlog::error("任务执行器 {} - 轮询任务异常: {}", executor_id_, e.what());
        }
        
        // 释放锁
        kv_service_->Release_lock(executor_key_);
    }

    // 执行单个分配的任务（返回true表示任务完成，false表示需要重试）
    bool execute_assigned_task(const TaskId& task_id) {
        try {
            spdlog::info("任务执行器 {} - 开始执行任务 {}", executor_id_, task_id);
            
            // 1. 获取任务数据
            std::string task_str = kv_service_->Get("task:" + task_id);
            if (task_str.empty()) {
                spdlog::error("任务执行器 {} - 任务 {} 不存在", executor_id_, task_id);
                update_task_status(task_id, TaskStatus::Failed, "任务数据不存在");
                return true; // 任务无法执行，标记为完成
            }
            
            ScheduledTask task = json::parse(task_str).get<ScheduledTask>();
                        
            // 3. 执行任务逻辑
            bool success = execute_task_payload(task);
            
            // 4. 更新任务最终状态
            if (success) {
                update_task_status(task_id, TaskStatus::Completed);
                spdlog::info("任务执行器 {} - 任务 {} 执行成功", executor_id_, task_id);
                return true; // 任务完成
            } else {
                update_task_status(task_id, TaskStatus::Failed, "任务执行失败");
                spdlog::error("任务执行器 {} - 任务 {} 执行失败", executor_id_, task_id);
                // 可根据需求调整：返回false表示需要重试，true表示直接标记为失败
                return true; 
            }
            
        } catch (const std::exception& e) {
            spdlog::error("任务执行器 {} - 执行任务 {} 异常: {}", executor_id_, task_id, e.what());
            update_task_status(task_id, TaskStatus::Failed, e.what());
            return true; // 异常任务标记为完成
        }
    }

    // 执行任务负载（调用注册的处理器）
    bool execute_task_payload(const ScheduledTask& task) {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        
        try {
            json payload_json = json::parse(task.payload);
            std::string task_type = payload_json.value("type", "default");
            
            auto it = task_handlers_.find(task_type);
            if (it != task_handlers_.end()) {
                // 调用注册的处理器
                return it->second(task.payload);
            } else {
                spdlog::warn("任务执行器 {} - 未找到任务类型 {} 的处理器", executor_id_, task_type);
                return false;
            }
        } catch (const std::exception& e) {
            spdlog::error("任务执行器 {} - 任务 {} payload 解析失败: {}", 
                         executor_id_, task.id, e.what());
            return false;
        }
    }

    // 更新任务状态到KV（执行器专属逻辑）
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
            } else if (new_status == TaskStatus::Completed || new_status == TaskStatus::Failed) {
                status_info.completed_at = now; // 记录完成时间
                if (!error_msg.empty()) {
                    status_info.error_message = error_msg; // 记录错误信息
                }
            }
            
            // 3. 保存到KV
            json status_json = status_info;
            int64_t put_result = kv_service_->Put("task_status:" + task_id, status_json.dump());
            if (put_result == -1) {
                spdlog::error("任务执行器 {} - 更新任务 {} 状态失败", executor_id_, task_id);
            }
            
        } catch (const std::exception& e) {
            spdlog::error("任务执行器 {} - 更新任务 {} 状态异常: {}", executor_id_, task_id, e.what());
        }
    }

    // 成员变量
    std::shared_ptr<KvService> kv_service_;
    ExecutorId executor_id_;          // 执行器唯一ID
    std::string executor_key_;        // 执行器任务列表KV键（executor:{id}）
    std::atomic<bool> stop_flag_;     // 停止标志
    std::unique_ptr<std::thread> worker_thread_; // 工作线程
    
    // 任务处理器映射
    std::map<std::string, std::function<bool(const TaskPayload&)>> task_handlers_;
    mutable std::mutex handlers_mutex_;
    
    // 任务处理状态跟踪（避免重复执行）
    std::unordered_set<TaskId> processed_tasks_;    // 已完成的任务
    std::unordered_set<TaskId> processing_tasks_;   // 正在处理的任务
    mutable std::mutex processing_mutex_;           // 保护任务状态集合
};
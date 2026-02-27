#pragma once
#include "raftnode.hpp"
#include <iostream>
#include <random>

RaftNode::RaftNode(int node_id, const std::string& ip, int port, 
                   const std::vector<std::pair<std::string, int>>& peers,
                   int election_elapsed_time)
    : node_id_(node_id), ip_(ip), port_(port), peers_(peers),
      server_(mrpc::server::get()), client_(mrpc::client::get()) {

    // 初始化next_index和match_index数组
    next_index_.resize(peers_.size());
    match_index_.resize(peers_.size());
    total_nodes_count_ = peers_.size()+1;
    election_elapsed_time_ = election_elapsed_time;
        
    // 加载持久化状态
    load_state();
    
    // 设置RPC处理器
    setup_rpc_handlers();
    

    // 连接到其他节点
    start_server();

    // spdlog::info("Connected to peers---------------------------------:");

    
    last_heartbeat_time_ = std::chrono::steady_clock::now();
}



//具体选举逻辑
void RaftNode::run_election_timeout() {
    spdlog::info("------------------------------------Into run_election_timeout------------------------------------");
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 10000); // 1000-3000ms 随机选举超时

    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        std::unique_lock<std::mutex> lock(mutex_);

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_heartbeat_time_).count();

        State current_state = state_.load();
        
        // 检查是否超时
        if (elapsed > election_elapsed_time_) { //已经election_elapsed_time_毫秒没有收到leader的心跳了，本节点开始尝试选举
            if (current_state == FOLLOWER || current_state == CANDIDATE) {//进入选举阶段
                election_elapsed_time_ = dis(gen);//重新设置选举超时时间
                // 转换为候选人状态，开始新的选举
                become_candidate();
                
                lock.unlock();

                VoteRequest vote_req;
                {
                     std::lock_guard<std::mutex> lock(mutex_); // 获取锁以安全读取日志信息
                     vote_req.term = current_term_;
                     vote_req.candidateId = node_id_;
                     vote_req.lastLogIndex = get_last_log_index();
                     vote_req.lastLogTerm = get_last_log_term();
                }
                spdlog::info("------------------------------------Vote begain--------------------------------------");
                size_t vote_count = 0;
                // 发送投票请求
                for (size_t i=0;i<peer_connections_.size();i++) {
                    if(state_.load() != CANDIDATE){
                        spdlog::info("-----------------------------------Leader 诞生，停止选举------------------------------------");
                        std::this_thread::sleep_for(100ms);
                        exit(0);
                    }
                    if (peer_connections_[i]) {
                        VoteReply reply;
                        if (send_vote_request(vote_req, i, reply)) {
                            spdlog::debug("Success! Result: reply.term={}, voteGranted={}", reply.term, reply.voteGranted);
                            if(reply.term > current_term_){
                                become_follower(reply.term);
                                break;
                            }else if (reply.voteGranted) {
                                vote_count++;
                            }
                        } 
                    }
                }
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_election_time_).count();
                if(elapsed > election_elapsed_time_){
                    become_candidate();
                    //本轮选举超时，应该重新发起选举，这里的逻辑如何实现还有待思考
                    spdlog::debug("------------------------------------Election_begin_end_timeout: {}ms------------------------------------", elapsed);
                    continue;
                }
                
                if(state_.load() == FOLLOWER) continue;
                //实现投票请求rpc逻辑
                spdlog::info("------------------------------------Vote count: {}------------------------------------", vote_count);
                // 如果没有选举时长没有超时并且检查获得多数投票
                if (vote_count > (total_nodes_count_ / 2)) become_leader();
                else become_follower(current_term_);
            }
        }
        if(state_.load() == LEADER){
            spdlog::debug("------------------------------------Stop election------------------------------------");
            // break;
            running_ = false;
        }
        lock.unlock(); //提前解锁，防止持有锁睡眠100ms
        // 睡眠一小段时间，避免忙轮询
        std::this_thread::sleep_for(100ms);
    }
}

void RaftNode::become_candidate() {
    //    每次发起选举，Candidate 都会开启一个新的任期
    current_term_++;
    state_ = CANDIDATE;
    voted_for_ = node_id_;
        // 5. 记录日志
    spdlog::info("Node {} became candidate at term {}", node_id_, current_term_);
    last_election_time_ = std::chrono::steady_clock::now();
    save_state();
}
RaftNode::~RaftNode() {
    stop();
}

void RaftNode::become_follower(int32_t new_term) {
    std::lock_guard<std::mutex> lock(mutex_); // 保护共享状态的修改

    // 更新任期为发现的更高任期
    if (new_term > current_term_) {
        current_term_.store(new_term);
    }
    // 如果传入的 new_term 与 current_term 相等，也可能需要转换（例如，收到了合法的 AppendEntries）
    // 但最常见的情况是 new_term > current_term_
    if (new_term >= current_term_) {
        current_term_.store(new_term);
    }

    // 转换为跟随者状态
    state_.store(FOLLOWER);

    // 重置投票信息
    voted_for_.store(-1);
    last_heartbeat_time_ = std::chrono::steady_clock::now();//假装立马收到了leader心跳
    // 记录日志
    spdlog::debug("Node {} became follower at term {}", node_id_, current_term_.load());

    // 持久化状态 (任期和投票信息)
    save_state();
}

void RaftNode::stop() {
    if (!running_) return;
    
    spdlog::debug("Stopping Raft node {}...", node_id_);
    
    running_ = false;
    
    // 关闭服务器
    server_.shutdown();
    
    // 等待线程结束
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    if (election_thread_.joinable()) {
        election_thread_.join();
    }
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    client_.shutdown();
    spdlog::debug("Raft node {} stopped.", node_id_);
    exit(0);
}

void RaftNode::setup_rpc_handlers() {
    // 注册RPC处理函数
    server_.reg_func("vote_request", 
                     [this](const VoteRequest& req) -> VoteReply { 
                         return this->handle_vote_request(req); 
                     });
    server_.reg_func("append_request", 
                     [this](const AppendRequest& req) -> AppendReply { 
                         return this->handle_append_request(req); 
                     });
}

void RaftNode::start_server() {
    if (running_) return;
    
    running_ = true;
    
    // 启动服务器
    server_.set_ip_port(ip_, port_);
    server_.set_server_name("raft_node_" + std::to_string(node_id_));
    server_.run();
    
    // 启动各个线程
    server_thread_ = std::thread([this]() {
        server_.accept();
        // spdlog::info("server accept---------------------------------:");
    });
    
    spdlog::info("------------------------------------Raft node {} started on {}:{}------------------------------------", node_id_, ip_, port_);
}

void RaftNode::start_client() {
    client_.run();
    
    for (const auto& peer : peers_) {
        auto conn = client_.connect(peer.first, peer.second);
        if (conn) {
            peer_connections_.push_back(conn);
            spdlog::debug("Connected to peer: {}:{}", peer.first, peer.second);
        } else {
            spdlog::error("Failed to connect to peer: {}:{}", peer.first, peer.second);
            peer_connections_.push_back(nullptr);  // 保持索引对应
        }
    }

    // 启动选举超时监控线程
    election_thread_ = std::thread(&RaftNode::run_election_timeout, this);
}

bool RaftNode::send_vote_request(const VoteRequest& request, size_t peer_idx, VoteReply& reply) {
    if (peer_idx >= peer_connections_.size() || !peer_connections_[peer_idx]) {
        return false;
    }
    auto result = peer_connections_[peer_idx]->call<VoteReply>("vote_request", request);
    if (result.error_code() == mrpc::ok) {
        reply = result.value();
        return true;
    }else {
        spdlog::error("Failed: {} \nWhen vote_request on {}:{}", result.error_msg(), peers_[peer_idx].first, peers_[peer_idx].second);
    }
    return false;
}

bool RaftNode::send_append_entries(const AppendRequest& request, size_t peer_idx, AppendReply& reply) {
    if (peer_idx >= peer_connections_.size() || !peer_connections_[peer_idx]) {
        return false;
    }
    
    auto result = peer_connections_[peer_idx]->call<AppendReply>("append_request", request);
    if (result.error_code() == mrpc::ok) {
        reply = result.value();
        return true;
    }
    return false;
}

void RaftNode::save_state() {
    // 模拟持久化存储
    // 在实际实现中，这里会写入磁盘
    // 例如，可以将 current_term_ 和 voted_for_ 写入一个状态文件
}

void RaftNode::load_state() {
    // 模拟从持久化存储加载
    // 在实际实现中，这里会从磁盘读取
    // 例如，从状态文件中恢复 current_term_ 和 voted_for_
    // 如果没有状态文件，则初始化为默认值
}

int32_t RaftNode::get_last_log_index() const {
    // 日志索引通常从 0 开始
    // 如果日志为空，则最后索引为 -1 (或根据规范定义的初始值)
    if (log_.empty()) {
        return -1; // Raft 论文中初始值常为 -1，PrevLogIndex 为 -1 表示没有前置日志
    }
    return log_.back().index;
}

int32_t RaftNode::get_last_log_term() const {
    // 如果日志为空，则最后任期为 0
    if (log_.empty()) {
        return 0; 
    }
    // 返回最后一个日志条目的任期
    return log_.back().term;
}


VoteReply RaftNode::handle_vote_request(const VoteRequest& request) {
    spdlog::debug("Node {} received vote request: term={}, candidateId={}, lastLogIndex={}, lastLogTerm={}",
                  node_id_, request.term, request.candidateId, request.lastLogIndex, request.lastLogTerm);

    VoteReply reply{.term = current_term_.load(), .voteGranted = false}; // 默认拒绝投票

    // Rule 1: 如果请求的任期小于当前节点的任期，拒绝投票
    if (request.term < current_term_) {
        spdlog::debug("Node {} rejecting vote request from node {}: request term {} < current term {}",
                      node_id_, request.candidateId, request.term, current_term_.load());
        return reply; // reply.term 已设为 current_term，voteGranted 为 false
    }

    // Rule 2: 如果请求的任期大于当前节点的任期，转换为 Follower
    if (request.term > current_term_) {
        spdlog::debug("Node {} discovered higher term {} in vote request, becoming follower.", 
                     node_id_, request.term);
        become_follower(request.term); // 这会更新 current_term_, state_, voted_for_, 并重置 last_heartbeat_time_ 和持久化
        // 此时 current_term_ 已更新，reply.term 会在后面被更新
        reply.term = current_term_.load(); // 确保回复携带最新的任期
    }

    // Rule 3: 检查是否已经投票给了其他候选人 (在同一任期内)
    // 注意：只有在 request.term == current_term_ 时，voted_for_ 才有意义
    if (request.term == current_term_ && voted_for_ != -1 && voted_for_ != request.candidateId) {
        spdlog::debug("Node {} rejecting vote request from node {}: already voted for node {} in term {}.",
                      node_id_, request.candidateId, voted_for_.load(), current_term_.load());
        return reply; // 已投票给其他人，拒绝
    }

    // Rule 4: 检查候选人的日志是否至少和自己一样新 (Log Up-to-Date Test)
    if (!is_log_up_to_date(request.lastLogTerm, request.lastLogIndex)) {
        spdlog::debug("Node {} rejecting vote request from node {}: its log is not up-to-date.",
                      node_id_, request.candidateId);
        return reply; // 日志不够新，拒绝
    }

    // 如果所有条件都满足，则授予投票
    voted_for_ = request.candidateId;
    reply.voteGranted = true;

    // 重置心跳时间，防止刚投票后就因超时而发起选举
    // last_heartbeat_time_ = std::chrono::steady_clock::now();

    // 持久化状态 (任期和投票信息)
    save_state();

    spdlog::debug("Node {} granted vote to node {} for term {}.", node_id_, request.candidateId, request.term);
    return reply;
}

bool RaftNode::is_log_up_to_date(int32_t last_log_term, int32_t last_log_index) const {
    int32_t my_last_log_term = get_last_log_term();
    int32_t my_last_log_index = get_last_log_index();

    if (last_log_term != my_last_log_term) {
        return last_log_term > my_last_log_term;
    }
    return last_log_index >= my_last_log_index;
}

void RaftNode::become_leader() {
    state_.store(LEADER);
    spdlog::debug("Node {} became leader at term {}.", node_id_, current_term_.load());
//     std::lock_guard<std::mutex> lock(mutex_); // 获取锁保护共享状态

//     if (state_.load() == LEADER) {
//         // 可能存在竞态条件，多个地方尝试成为Leader，或重复调用
//         spdlog::warn("Node {} attempted to become leader but was already leader.", node_id_);
//         return;
//     }

//     spdlog::info("Node {} became leader at term {}.", node_id_, current_term_.load());

//     // 1. 转换状态
//     state_.store(LEADER);

//     // 2. 初始化 Leader 特有状态
//     int32_t last_log_idx = get_last_log_index();
//     for (size_t i = 0; i < next_index_.size(); ++i) {
//         // 对于集群中的每个节点，初始化其 next_index 为新领导者的最后日志条目索引 + 1
//         next_index_[i] = last_log_idx + 1;
//     }
//     for (size_t i = 0; i < match_index_.size(); ++i) {
//         // 对于集群中的每个节点，初始化其 match_index 为 0 (或 -1，取决于索引起始)
//         // 初始时，Leader 假设 Follower 没有任何日志
//         match_index_[i] = -1; // 假设索引从 -1 开始（空日志）
//     }

//     // 3. 持久化状态 (虽然成为 Leader 本身不改变 current_term 或 voted_for，但状态改变)
//     // 这一步可以省略，因为 state_ 是 atomic，但为了完整性可以记录。
//     save_state();

//     // 4. (可选) 立即发送一个空的 AppendEntries 作为心跳，快速通知其他节点
//     // 这个可以在 send_heartbeats 线程中由定时器触发，也可以在这里手动触发一次
//     // 为了简单，我们依赖心跳线程启动后发送第一个心跳

//     // 5. 启动心跳线程 (如果尚未启动)
//     // 注意：心跳线程需要持有锁来读取状态和发送请求，需要小心处理锁竞争
//     if (heartbeat_thread_.joinable()) {
//         // 如果心跳线程意外还在运行，先等待它结束
//         spdlog::warn("Heartbeat thread was still running when becoming leader. Joining...");
//         heartbeat_thread_.join();
//     }
//     heartbeat_thread_ = std::thread(&RaftNode::send_heartbeats, this);
}
#!/bin/bash
# 停止所有 Raft 节点
pkill -f "./bin/main"
# 检查是否所有节点都已停止
if [ $(pgrep -f "./bin/main" | wc -l) -eq 0 ]; then
    echo "All Raft nodes have stopped"
else
    echo "Some Raft nodes are still running"
fi
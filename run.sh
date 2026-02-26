#!/bin/bash

# 定义日志目录（和你的程序日志目录保持一致）
LOG_DIR="./logs"
# 创建日志目录（避免文件无法写入）
mkdir -p ${LOG_DIR}

# 启动 Node 0：所有输出（stdout/stderr）重定向到 logs/node0.log，彻底屏蔽控制台输出
./bin/main 0 127.0.0.1 8000 127.0.0.1:8001 127.0.0.1:8002 > ${LOG_DIR}/node0.log 2>&1 &
echo "Node 0 started (log: ${LOG_DIR}/node0.log)"

# 启动 Node 1：同理重定向到 node1.log
./bin/main 1 127.0.0.1 8001 127.0.0.1:8000 127.0.0.1:8002 > ${LOG_DIR}/node1.log 2>&1 &
echo "Node 1 started (log: ${LOG_DIR}/node1.log)"

# 启动 Node 2：同理重定向到 node2.log
./bin/main 2 127.0.0.1 8002 127.0.0.1:8001 127.0.0.1:8000 > ${LOG_DIR}/node2.log 2>&1 &
echo "Node 2 started (log: ${LOG_DIR}/node2.log)"

# 可选：如果想完全静默（连启动提示都不要），把上面的 echo 删掉即可
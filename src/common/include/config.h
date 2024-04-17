#pragma once

const bool Debug = true;

const int debugMul = 1; // 时间单位：time.Millisecond
const int HeartBeatTimeout = 25 * debugMul; // 心跳时间一般要比选举超时小一个数量级
const int ApplyInterval = 10 * debugMul; // 日志应用超时

const int minRandomizedElectionTime = 300 * debugMul; // 最小选举超时为300ms
const int maxRandomizedElectionTime = 500 * debugMul; // 最大选举超时为500ms
// 实际的选举超时在这个范围内随机选取

const int CONSENSUS_TIMEOUT = 500 * debugMul; // 命令执行的最大超时为500ms，允许在这个时间段内完成命令

// 协程库相关设置
const int FIBER_THREAD_NUM = 1;
const bool FIBER_USE_CALLER_THREAD = false;



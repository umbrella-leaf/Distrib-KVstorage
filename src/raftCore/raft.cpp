#include "raft.h"
#include "config.h"
#include "util.h"
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <memory>

void Raft::AppendEntries1(const raftRpcProctoc::AppendEntriesArgs* args, raftRpcProctoc::AppendEntriesReply* reply) {
    std::lock_guard<std::mutex> lock(m_mtx);
    reply->set_appstate(AppNormal);
    // 检查Term
    // 如果leader任期未赶上当前节点，就告诉leader他过期了，应该卸任
    if (args->term() < m_currentTerm) {
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        // 让领导人及时更新自己
        reply->set_updatenextindex(-100);
        DPrintf("[func-AppendEntries-rf{%d}] 拒绝了 因为Leader{%d}的term{%v}< rf{%d}.term{%d}\n", m_me, args->leaderid(),
                args->term(), m_me, m_currentTerm);
        return;
    }
    DEFER {persist();};
    // 当前节点任期小于leader任期
    // 需要赶上leader任期并且由于更新任期，投票对象也要重置
    if (args->term() > m_currentTerm) {
        m_status = Follower;
        m_currentTerm = args->term();
        m_votedFor = -1;
    }
    myAssert(args->term() == m_currentTerm, format("assert {args.Term == rf.currentTerm} fail"));
    // 发生网络分区，接收到同一个term的leader的消息，也应该转变为followwer
    m_status = Follower;
    // 重置选举超时
    m_lastResetElectionTime = now();

    // 任期一致以后，比较最新日志
    // leader日志给超前了，告诉leader给少一点，就从自己最新日志下一条开始给
    if (args->prevlogindex() > getLastLogIndex()) {
        reply->set_success(false);;
        reply->set_term(m_currentTerm);
        reply->set_updatenextindex(getLastLogTerm() + 1);
        return;
    } else if (args->prevlogindex() < m_lastSnapshotIncludeIndex) {
        // leader给日志给少了，甚至还没有跟上本地快照
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        reply->set_updatenextindex(m_lastSnapshotIncludeIndex + 1);
        return;
    }
    // 确保leader没有搞错当前节点的最后一条日志index以及term
    if (matchLog(args->prevlogindex(), args->prevlogterm())) {
        // 检查每一条日志
        for (int i = 0; i < args->entries_size(); ++i) {
            auto log = args->entries(i);
            if (log.logindex() > getLastLogIndex()) {
                // 超过就直接添加
                m_logs.emplace_back(log);
            } else {
                // 没超过就比较是否匹配。不匹配再更新
                // 如果同index term的日志但是command不相等，那么有问题
                if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() == log.logterm() &&
                    m_logs[getSlicesIndexFromLogIndex(log.logindex())].command() != log.command()) {
                    myAssert(false, format("[func-AppendEntries-rf{%d}] 两节点logIndex{%d}和term{%d}相同，但是其command{%d:%d}   "
                                 " {%d:%d}却不同！！\n",
                                 m_me, log.logindex(), log.logterm(), m_me,
                                 m_logs[getSlicesIndexFromLogIndex(log.logindex())].command(), args->leaderid(),
                                 log.command()));
                }
                // 同index日志，term不相等，那么就更新该日志term
                if (m_logs[getLogTermFromLogIndex(log.logindex())].logterm() != log.logterm()) {
                    m_logs[getLogTermFromLogIndex(log.logindex())].set_logterm(log.logterm());
                }
            }
        }

        // 考虑到可能有过期日志，因此传送日志不一定全部接受了
        myAssert(
            getLastLogIndex() >= args->prevlogindex() + args->entries_size(),
            format("[func-AppendEntries1-rf{%d}]rf.getLastLogIndex(){%d} != args.PrevLogIndex{%d}+len(args.Entries){%d}",
                    m_me, getLastLogIndex(), args->prevlogindex(), args->entries_size()));
        // leader已提交日志比当前节点已提交日志更新，就跟
        if (args->leadercommit() > m_commitIndex) {
            // 不能无脑跟，因为当前节点不一定有这么多日志
            m_commitIndex = std::min(args->leadercommit(), getLastLogIndex());
        }
        myAssert(getLastLogIndex() >= m_commitIndex,
                format("[func-AppendEntries1-rf{%d}]  rf.getLastLogIndex{%d} < rf.commitIndex{%d}", m_me,
                        getLastLogIndex(), m_commitIndex));
        reply->set_success(true);
        reply->set_term(m_currentTerm);
        return;    
    } else {
        reply->set_updatenextindex(args->prevlogindex());
        // 如果搞错，那么真实的最新日志与leader认为的最新日志index相等，但是term不相等，那么这个term下的日志可能都搞错了
        // 应当从上一个term最后一条日志的下一条开始更新，也就是倒序找到第一条term不与leader认为的最新日志term相等的日志，然后从该日志index + 1开始更新
        for (int index = args->prevlogindex(); index >= m_lastSnapshotIncludeIndex; --index) {
            if (getLogTermFromLogIndex(index) != getLogTermFromLogIndex(args->prevlogindex())) {
                reply->set_updatenextindex(index + 1);
                break;
            }
        }
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        return;
    }
}

void Raft::applierTicker() {
    while (true) {
        m_mtx.lock();
        if (m_status == Leader) {
            DPrintf("[Raft::applierTicker() - raft{%d}]  m_lastApplied{%d}   m_commitIndex{%d}", m_me, m_lastApplied,
              m_commitIndex);
        }
        auto applyMsgs = getApplyLogs();
        m_mtx.unlock();

        if (!applyMsgs.empty()) {
            DPrintf("[func- Raft::applierTicker()-raft{%d}] 向kvserver報告的applyMsgs長度爲：{%d}", m_me, applyMsgs.size());
        }
        for (auto& message: applyMsgs) {
            applyChan->Push(message);
        }
        sleepNMilliseconds(ApplyInterval);
    }
}

bool Raft::CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, const std::string& sanpshot) {
    return true;
}

void Raft::doElection() {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_status != Leader) {
        DPrintf("[       ticker-func-rf(%d)              ]  选举定时器到期且不是leader，开始选举 \n", m_me);
        // 选举时改变身份为竞选者，同时自身任期+1，并为自己投票
        m_status = Candidate;
        m_currentTerm += 1;
        // 为自己投票主要是防止给同辈竞选者投票
        m_votedFor = m_me;
        // 持久化
        persist();
        auto votedNum = std::make_shared<int>(1);
        // 重新设置选举时间
        m_lastResetElectionTime = now();
        for (int i = 0; i < m_peers.size(); ++i) {
            if (i == m_me) continue;
            int lastLogIndex = -1, lastLogTerm = -1;
            getLastLogIndexAndTerm(&lastLogIndex, &lastLogTerm);
            // 使用智能指针的目的是自动释放内存，防止内存泄漏
            auto requestVoteArgs = std::make_shared<raftRpcProctoc::RequestVoteArgs>();
            requestVoteArgs->set_term(m_currentTerm);
            requestVoteArgs->set_candidateid(m_me);
            requestVoteArgs->set_lastlogindex(lastLogIndex);
            requestVoteArgs->set_lastlogterm(lastLogTerm);
            auto requestVoteReply = std::make_shared<raftRpcProctoc::RequestVoteReply>();
            
            // 使用匿名函数执行避免其拿到锁
            std::thread t(&Raft::sendRequestVote, this, i, requestVoteArgs, requestVoteReply, votedNum);
            t.detach();
        }
    }
}

void Raft::doHeartBeat() {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_status == Leader) {
        DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了且拿到mutex，开始发送AE\n", m_me);
        auto appendNums = std::make_shared<int>(1);
        // 对所有其他节点发送心跳+日志复制请求，但未改变节点自身状态，因此无需持久化
        for (int i = 0; i < m_peers.size(); ++i) {
            if (i == m_me) continue;
            DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了 index:{%d}\n", m_me, i);
            myAssert(m_nextIndex[i] >= 1, format("rf.nextIndex[%d] = {%d}", i, m_nextIndex[i]));
            // 判断发送快照还是日志，对于每个peer节点，都要从其nextIndex开始发送
            // 但是如果其nextIndex <= 当前节点的快照的最后一条日志index，那么直接发送快照即可
            if (m_nextIndex[i] <= m_lastSnapshotIncludeIndex) {
                std::thread t(&Raft::leaderSendSnapShot, this, i);
                t.detach();
                continue;
            }
            // 否则就发送日志
            int preLogIndex = -1, preLogTerm = -1;
            getPrevLogInfo(i, &preLogIndex, &preLogTerm);
            auto appendEntriesArgs = std::make_shared<raftRpcProctoc::AppendEntriesArgs>();
            appendEntriesArgs->set_term(m_currentTerm);
            appendEntriesArgs->set_leaderid(m_me);
            appendEntriesArgs->set_prevlogindex(preLogIndex);
            appendEntriesArgs->set_prevlogterm(preLogTerm);
            appendEntriesArgs->clear_entries();
            appendEntriesArgs->set_leadercommit(m_commitIndex);
            // 发送快照的情况是preLogIndex < 当前快照最后一条日志的index
            // 在不发送快照的情况下，二者如果不等，那么一定是>，需要截取m_logs的一部分发
            if (preLogIndex != m_lastSnapshotIncludeIndex) {
                for (int j = getSlicesIndexFromLogIndex(preLogIndex) + 1; j < m_logs.size(); ++j) {
                    raftRpcProctoc::LogEntry* sendEntryPtr = appendEntriesArgs->add_entries();
                    *sendEntryPtr = m_logs[j];
                }
            // 否则二者相等，需要发送全部的日志
            } else {
                for (const auto& item: m_logs) {
                    raftRpcProctoc::LogEntry* sendEntryPtr = appendEntriesArgs->add_entries();
                    *sendEntryPtr = item;
                }
            }
            int lastLogIndex = getLastLogIndex();
            // 保证对每个peer节点，都从其prevIndex发送到最后
            myAssert(appendEntriesArgs->prevlogindex() + appendEntriesArgs->entries_size() == lastLogIndex,
                    format("appendEntriesArgs.PrevLogIndex{%d}+len(appendEntriesArgs.Entries){%d} != lastLogIndex{%d}",
                            appendEntriesArgs->prevlogindex(), appendEntriesArgs->entries_size(), lastLogIndex));
            auto appendEntriesReply = std::make_shared<raftRpcProctoc::AppendEntriesReply>();
            appendEntriesReply->set_appstate(Disconnected);

            std::thread t(&Raft::sendAppendEntries, this, i, appendEntriesArgs, appendEntriesReply, appendNums);
            t.detach();
        }
        // 更新心跳时间
        m_lastResetHeartBeatTime = now();
    }
}

void Raft::electionTimeOutTicker() {
    while (true) {
        // 
        while (m_status == Leader) {
            usleep(HeartBeatTimeout);
        }
        std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};
        std::chrono::system_clock::time_point wakeTime{};
        {
            m_mtx.lock();
            wakeTime = now();
            suitableSleepTime = getRandomizedElectionTimeout() + m_lastResetElectionTime - wakeTime;
            m_mtx.unlock();
        }
        if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1) {
            // 使用steady物理时钟来计算实际睡眠时间
            auto start = std::chrono::steady_clock::now();
            usleep(std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count());
            auto end = std::chrono::steady_clock::now();

            std::chrono::duration<double, std::milli> duration = end - start;

            // 使用ANSI控制序列将输出颜色修改为紫色
            std::cout << "\033[1;35m electionTimeOutTicker();函数设置睡眠时间为: "
                        << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() << " 毫秒\033[0m"
                        << std::endl;
            std::cout << "\033[1;35m electionTimeOutTicker();函数实际睡眠时间为: " << duration.count() << " 毫秒\033[0m"
                        << std::endl;
        }

        if (std::chrono::duration<double, std::milli>(m_lastResetElectionTime - wakeTime).count() > 0) {
            // 睡眠过程中选举定时器被重置，也就是收到了合法leader的心跳，那么就不发起选举，继续睡眠
            continue;
        }
        doElection();
    }
}

// 获取当前节点全部待执行日志
std::vector<ApplyMsg> Raft::getApplyLogs() {
    std::vector<ApplyMsg> applyMsgs;
    // 执行日志是[最新执行日志index，最新提交日志index]
    myAssert(m_commitIndex <= getLastLogIndex(), format("[func-getApplyLogs-rf{%d}] commitIndex{%d} >getLastLogIndex{%d}",
                        m_me, m_commitIndex, getLastLogIndex()));
    while (m_lastApplied < m_commitIndex) {
        ++m_lastApplied;
        myAssert(m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex() == m_lastApplied,
                format("rf.logs[rf.getSlicesIndexFromLogIndex(rf.lastApplied)].LogIndex{%d} != rf.lastApplied{%d} ",
                        m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex(), m_lastApplied));
        ApplyMsg applyMsg;
        applyMsg.CommandValid = true;
        applyMsg.SnapShotValid = false;
        applyMsg.Command = m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].command();
        applyMsg.CommandIndex = m_lastApplied;
        applyMsgs.emplace_back(applyMsg);
    }
    return applyMsgs;
}

// 获取新命令应该分配的index，类似自增ID
int Raft::getNewCommandIndex() {
    auto lastLogIndex = getLastLogIndex();
    return lastLogIndex + 1;
}

// leader调用，传入peer节点编号，获取该节点的preLogIndex和preLogTerm
void Raft::getPrevLogInfo(int server, int* preIndex, int* preTerm) {
    // 如果该节点nextIndex=当前快照最后一条日志index + 1
    // 那么preLog就应该是当前快照最后一条日志，但这条日志并不在m_logs里
    if (m_nextIndex[server] == m_lastSnapshotIncludeIndex + 1) {
        *preIndex = m_lastSnapshotIncludeIndex;
        *preTerm = m_lastSnapshotIncludeTerm;
        return;
    }
    auto nextIndex = m_nextIndex[server];
    *preIndex = nextIndex - 1;
    *preTerm = m_logs[getSlicesIndexFromLogIndex(*preIndex)].logterm();
}

void Raft::GetState(int* term, bool* isLeader) {
    m_mtx.lock();
    DEFER {
        m_mtx.unlock();
    };
    *term = m_currentTerm;
    *isLeader = (m_status == Leader);
}

void Raft::InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest* args,
                           raftRpcProctoc::InstallSnapShotResponse* reply) {
    m_mtx.lock();
    DEFER {m_mtx.unlock();};
    if (args->term() < m_currentTerm) {
        reply->set_term(m_currentTerm);
        return;
    }
    if (args->term() > m_currentTerm) {
        m_currentTerm = args->term();
        m_votedFor = -1;
        m_status = Follower;
        persist();
    }
    m_status = Follower;
    m_lastResetElectionTime = now();

    if (args->lastsnapshotincludeindex() <= m_lastSnapshotIncludeIndex) {
        return;
    }
    auto lastLogIndex = getLastLogIndex();
    // 现有日志比快照长，需要截断现有日志，否则直接清空现有日志即可
    if (lastLogIndex > args->lastsnapshotincludeindex()) {
        m_logs.erase(m_logs.begin(), m_logs.begin() + getSlicesIndexFromLogIndex(args->lastsnapshotincludeindex()) + 1);
    } else {
        m_logs.clear();
    }
    // 最新提交index和最新执行index都直接跟上快照最新index
    m_commitIndex = std::max(m_commitIndex, args->lastsnapshotincludeindex());
    m_lastApplied = std::max(m_lastApplied, args->lastsnapshotincludeindex());
    m_lastSnapshotIncludeIndex = args->lastsnapshotincludeindex();
    m_lastSnapshotIncludeTerm = args->lastsnapshotincludeterm();

    reply->set_term(m_currentTerm);
    ApplyMsg msg;
    msg.SnapShotValid = true;
    msg.Snapshot = args->data();
    msg.SnapshotTerm = args->lastsnapshotincludeterm();
    msg.SnapshotIndex = args->lastsnapshotincludeindex();

    applyChan->Push(msg);
    // 由于快照更新了，那么快照也需要保存
    m_persister->Save(persistData(), args->data());
}

void Raft::pushMsgToKvServer(ApplyMsg msg) {applyChan->Push(msg);}

void Raft::leaderHearBeatTicker() {
    while (true) {
        // 不是leader就循环睡眠
        while (m_status != Leader) {
            usleep(1000 * HeartBeatTimeout);
        }
        static std::atomic<int32_t> atomicCount = 0;

        std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};
        std::chrono::system_clock::time_point wakeTime{};
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            wakeTime = now();
            suitableSleepTime = std::chrono::milliseconds(HeartBeatTimeout) + m_lastResetHeartBeatTime - wakeTime;
        }
        if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1) {
            std::cout << atomicCount << "\033[1;35m leaderHearBeatTicker();函数设置睡眠时间为: "
                    << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() << " 毫秒\033[0m"
                    << std::endl;
            // 计算实际睡眠时间
            auto start = std::chrono::steady_clock::now();
            usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleepTime).count());
            auto end = std::chrono::steady_clock::now();
            std::chrono::duration<double, std::milli> duration = end - start;

            // 使用ANSI控制序列将输出颜色修改为紫色
            std::cout << atomicCount << "\033[1;35m leaderHearBeatTicker();函数实际睡眠时间为: " << duration.count()
                    << " 毫秒\033[0m" << std::endl;  
            ++atomicCount;  
        }

        if (std::chrono::duration<double, std::milli>(m_lastResetHeartBeatTime - wakeTime).count() > 0) {
            // 没有超时就再次睡眠
            continue;
        }
        doHeartBeat();
    }
}

void Raft::leaderSendSnapShot(int server) {
    m_mtx.lock();
    // 与心跳/日志复制调用不一样，这里在线程中才填充请求参数，因此无需智能指针
    raftRpcProctoc::InstallSnapshotRequest args;
    args.set_leaderid(m_me);
    args.set_term(m_currentTerm);
    args.set_lastsnapshotincludeindex(m_lastSnapshotIncludeIndex);
    args.set_lastsnapshotincludeterm(m_lastSnapshotIncludeTerm);
    args.set_data(m_persister->ReadSnapshot());

    raftRpcProctoc::InstallSnapShotResponse reply;
    m_mtx.unlock();

    bool ok = m_peers[server]->InstallSnapShot(&args,  &reply);
    m_mtx.lock();
    DEFER {m_mtx.unlock();};
    if (!ok) {
        return;
    }
    if (m_status != Leader || m_currentTerm != args.term()) {
        return;
    }
    if (reply.term() > m_currentTerm) {
        // 三变
        m_currentTerm = reply.term();
        m_votedFor = -1;
        m_status = Follower;
        persist();
        m_lastResetElectionTime = now();
        return;
    }
    // 目标节点matchIndex跟到发送快照的最后一条
    m_matchIndex[server] = args.lastsnapshotincludeindex();
    m_nextIndex[server] = m_matchIndex[server] + 1;
}

void Raft::leaderUpdateCommitIndex() {
    m_commitIndex = m_lastSnapshotIncludeIndex;
    for (int index = getLastLogIndex(); index >= m_lastSnapshotIncludeIndex + 1; --index) {
        int sum = 0;
        for (int i = 0; i < m_peers.size(); ++i) {
            if (i == m_me) {
                sum += 1;
                continue;
            }
            if (m_matchIndex[i] >= index) {
                sum += 1;
            }
        }
        // 只有当前term有新提交的，才更新commitIndex
        if (sum >= m_peers.size() / 2 + 1 && getLogTermFromLogIndex(index) == m_currentTerm) {
            m_commitIndex = index;
            break;
        }
    }
}

bool Raft::matchLog(int logIndex, int logTerm) {
    myAssert(logIndex >= m_lastSnapshotIncludeIndex && logIndex <= getLastLogIndex(),
            format("不满足：logIndex{%d}>=rf.lastSnapshotIncludeIndex{%d}&&logIndex{%d}<=rf.getLastLogIndex{%d}",
                logIndex, m_lastSnapshotIncludeIndex, logIndex, getLastLogIndex()));
    return logTerm == getLogTermFromLogIndex(logIndex);
}

void Raft::persist() {
    auto data = persistData();
    m_persister->SaveRaftState(data);
}

void Raft::RequestVote(const raftRpcProctoc::RequestVoteArgs* args, raftRpcProctoc::RequestVoteReply* reply) {
    std::lock_guard<std::mutex> lock(m_mtx);

    DEFER {
        persist();
    };
    if (args->term() < m_currentTerm) {
        reply->set_term(m_currentTerm);
        reply->set_votestate(Expire);
        reply->set_votegranted(false);
        return;
    }
    if (args->term() > m_currentTerm) {
        m_status = Follower;
        m_currentTerm = args->term();
        m_votedFor = -1;
    }
    myAssert(args->term() == m_currentTerm,
            format("[func--rf{%d}] 前面校验过args.Term==rf.currentTerm，这里却不等", m_me));
    //	现在节点任期都是相同的(任期小的也已经更新到新的args的term了)，还需要检查log的term和index是不是匹配的了

    int lastLogTerm = getLastLogTerm();
    // 只有没投票，并且candidate的最新日志比当前节点的最新日志更新，才会授票
    if (!UpToDate(args->lastlogindex(), args->lastlogterm())) {
        reply->set_term(m_currentTerm);
        reply->set_votestate(Voted);
        reply->set_votegranted(false);
        return;
    }
    // 当前节点在本任期已经投过票，并且不是投给当前candidate
    if (m_votedFor != -1 && m_votedFor != args->candidateid()) {
        reply->set_term(m_currentTerm);
        reply->set_votestate(Voted);
        reply->set_votegranted(false);
        return;
    } else {
        m_votedFor = args->candidateid();
        m_lastResetElectionTime = now();
        reply->set_term(m_currentTerm);
        reply->set_votestate(Normal);
        reply->set_votegranted(true);
        return;
    }
}

bool Raft::UpToDate(int index, int term) {
    int lastIndex = -1;
    int lastTerm = -1;
    getLastLogIndexAndTerm(&lastIndex, &lastTerm);
    return term > lastTerm || (term == lastTerm && index >= lastIndex);
}

void Raft::getLastLogIndexAndTerm(int* lastLogIndex, int* lastLogTerm) {
    if (m_logs.empty()) {
        *lastLogIndex = m_lastSnapshotIncludeIndex;
        *lastLogTerm = m_lastSnapshotIncludeTerm;
        return;
    } else {
        *lastLogIndex = m_logs[m_logs.size() - 1].logindex();
        *lastLogTerm = m_logs[m_logs.size() - 1].logterm();
        return;
    }
}

int Raft::getLastLogIndex() {
    int lastLogIndex = -1;
    int _ = -1;getLastLogIndexAndTerm(&lastLogIndex, &_);
    return lastLogIndex;
}

int Raft::getLastLogTerm() {
    int _ = -1;
    int lastLogTerm = -1;
    getLastLogIndexAndTerm(&_, &lastLogTerm);
    return lastLogTerm;
}

// 从日志索引获取日志term
int Raft::getLogTermFromLogIndex(int logIndex) {
    // 必须保证日志是存在于m_logs中，而不是快照
    myAssert(logIndex >= m_lastSnapshotIncludeIndex,
            format("[func-getSlicesIndexFromLogIndex-rf{%d}]  index{%d} < rf.lastSnapshotIncludeIndex{%d}", m_me,
                logIndex, m_lastSnapshotIncludeIndex));
    int lastLogIndex = getLastLogIndex();
    // 必须保证该日志index未超过当前节点最新日志
    myAssert(logIndex <= lastLogIndex, format("[func-getSlicesIndexFromLogIndex-rf{%d}]  logIndex{%d} > lastLogIndex{%d}",
                        m_me, logIndex, lastLogIndex));
    if (logIndex == m_lastSnapshotIncludeIndex) {
        return m_lastSnapshotIncludeTerm;
    } else {
        return m_logs[getSlicesIndexFromLogIndex(logIndex)].logterm();
    }
}

int Raft::GetRaftStateSize() {
    return m_persister->RaftStateSize();
}

int Raft::getSlicesIndexFromLogIndex(int logIndex) {
    myAssert(logIndex > m_lastSnapshotIncludeIndex,
            format("[func-getSlicesIndexFromLogIndex-rf{%d}]  index{%d} <= rf.lastSnapshotIncludeIndex{%d}", m_me,
                    logIndex, m_lastSnapshotIncludeIndex));
    int lastLogIndex = getLastLogIndex();
    myAssert(logIndex <= lastLogIndex, format("[func-getSlicesIndexFromLogIndex-rf{%d}]  logIndex{%d} > lastLogIndex{%d}",
                        m_me, logIndex, lastLogIndex));
    int sliceIndex = logIndex - m_lastSnapshotIncludeIndex - 1;
    return sliceIndex;
}

bool Raft::sendRequestVote(int server, std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
                           std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, std::shared_ptr<int> votedNum) {
    auto start = now();
    DPrintf("[func-sendRequestVote rf{%d}] 向server{%d} 發送 RequestVote 開始", m_me, m_currentTerm, getLastLogIndex());
    bool ok = m_peers[server]->RequestVote(args.get(), reply.get());
    DPrintf("[func-sendRequestVote rf{%d}] 向server{%d} 發送 RequestVote 完畢，耗時:{%d} ms", m_me, m_currentTerm,
            getLastLogIndex(), now() - start);
    
    if (!ok) {
        return ok;
    }

    std::lock_guard<std::mutex> lock(m_mtx);
    if (reply->term() > m_currentTerm) {
        // 收到任期更大的节点的响应，应该放弃竞选
        m_status = Follower;
        m_currentTerm = reply->term();
        m_votedFor = -1;
        persist();
        return true;
    } else if (reply->term() < m_currentTerm) {
        return true;
    }
    myAssert(reply->term() == m_currentTerm, format("assert {reply.Term==rf.currentTerm} fail"));

    if (!reply->votegranted()) {
        return true;
    }
    *votedNum += 1;
    // 得票数超过集群节点数的一半
    if (*votedNum >= m_peers.size() / 2 + 1) {
        // 变成leader
        *votedNum = 0;
        if (m_status == Leader) {
            myAssert(false,
                    format("[func-sendRequestVote-rf{%d}]  term:{%d} 同一个term当两次领导，error", m_me, m_currentTerm));           
        }
        m_status = Leader;
        // 变成leader后第一件事是重置全部peer节点的nextIndex和matchIndex
        DPrintf("[func-sendRequestVote rf{%d}] elect success  ,current term:{%d} ,lastLogIndex:{%d}\n", m_me, m_currentTerm,
                getLastLogIndex());
        int lastLogIndex = getLastLogIndex();
        for (int i = 0; i < m_nextIndex.size(); ++i) {
            // 每次换leader时，leader节点认为全部peer节点的nextIndex是自己最新日志的下一条
            // 但matchIndex从0开始
            m_nextIndex[i] = lastLogIndex + 1;
            m_matchIndex[i] = 0;
        }
        std::thread t(&Raft::doHeartBeat, this);
        t.detach();

        persist();
    }
    return true;
}

bool Raft::sendAppendEntries(int server, std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
                             std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply,
                             std::shared_ptr<int> appendNums) {
    DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc開始 ， args->entries_size():{%d}", m_me,
            server, args->entries_size());  
    bool ok = m_peers[server]->AppendEntires(args.get(), reply.get());

    if (!ok) {
        DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc失敗", m_me, server);
        return ok;
    }
    DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc成功", m_me, server);
    if (reply->appstate() == Disconnected) {
        return ok;
    }
    std::lock_guard<std::mutex> lock(m_mtx);

    if (reply->term() > m_currentTerm) {
        m_status = Follower;
        m_currentTerm = args->term();
        m_votedFor = -1;
        return ok;
    } else if (reply->term() < m_currentTerm) {
        DPrintf("[func -sendAppendEntries  rf{%d}]  节点：{%d}的term{%d}<rf{%d}的term{%d}\n", m_me, server, reply->term(),
                m_me, m_currentTerm);
        return ok;
    }
    // 发起心跳/日志复制的必须是leader!
    if (m_status != Leader) {
        return ok;
    }
    // 此时term相等
    myAssert(reply->term() == m_currentTerm,
        format("reply.Term{%d} != rf.currentTerm{%d}   ", reply->term(), m_currentTerm));
    if (!reply->success()) {
        // -100是无效的
        if (reply->updatenextindex() != -100) {
            DPrintf("[func -sendAppendEntries  rf{%d}]  返回的日志term相等，但是不匹配，回缩nextIndex[%d]：{%d}\n", m_me,
                    server, reply->updatenextindex());
            m_nextIndex[server] = reply->updatenextindex();
        }
    } else {
        *appendNums += 1;
        DPrintf("---------------------------tmp------------------------- 節點{%d}返回true,當前*appendNums{%d}", server,
                *appendNums);
        // 成功的话，则更新这个peer节点的matchIndex和nextIndex
        m_matchIndex[server] = std::max(m_matchIndex[server], args->prevlogindex() + args->entries_size());
        m_nextIndex[server] = m_matchIndex[server] + 1;
        int lastLogIndex = getLastLogIndex();

        myAssert(m_nextIndex[server] <= lastLogIndex + 1,
                format("error msg:rf.nextIndex[%d] > lastLogIndex+1, len(rf.logs) = %d   lastLogIndex{%d} = %d", server,
                        m_logs.size(), server, lastLogIndex));
        if (*appendNums >= m_peers.size() / 2 + 1) {
            // 超过一半的节点同步成功，可以提交了
            *appendNums = 0;
            if (args->entries_size() > 0) {
                DPrintf("args->entries(args->entries_size()-1).logterm(){%d}   m_currentTerm{%d}",
                        args->entries(args->entries_size() - 1).logterm(), m_currentTerm);
            }
            // 但是只有在当前term有日志可提交的时候，才提交日志，目的是为了防止重复提交过期日志
            if (args->entries_size() > 0 && args->entries(args->entries_size() - 1).logterm() == m_currentTerm) {
                DPrintf(
                "---------------------------tmp------------------------- 當前term有log成功提交，更新leader的m_commitIndex "
                    "from{%d} to{%d}",
                    m_commitIndex, args->prevlogindex() + args->entries_size());
                m_commitIndex = std::max(m_commitIndex, args->prevlogindex() + args->entries_size());
            }
            myAssert(m_commitIndex <= lastLogIndex,
                format("[func-sendAppendEntries,rf{%d}] lastLogIndex:%d  rf.commitIndex:%d\n", m_me, lastLogIndex,
                        m_commitIndex));
        }
    }
    return ok;
}

void Raft::AppendEntries(google::protobuf::RpcController* controller,
                         const ::raftRpcProctoc::AppendEntriesArgs* request,
                         ::raftRpcProctoc::AppendEntriesReply* response, ::google::protobuf::Closure* done) {
    AppendEntries1(request, response);
    done->Run();
}

void Raft::InstallSnapShot(google::protobuf::RpcController* controller,
                           const ::raftRpcProctoc::InstallSnapshotRequest* request,
                           ::raftRpcProctoc::InstallSnapShotResponse* response, ::google::protobuf::Closure* done) {
  InstallSnapshot(request, response);

  done->Run();
}

void Raft::RequestVote(google::protobuf::RpcController* controller, const ::raftRpcProctoc::RequestVoteArgs* request,
                       ::raftRpcProctoc::RequestVoteReply* response, ::google::protobuf::Closure* done) {
  RequestVote(request, response);
  done->Run();
}

// KvServer接收数据库相关命令后，调用该方法将日志写到raft节点中，leader only
void Raft::Start(Op command, int* newLogIndex, int* newLogterm, bool* isLeader) {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_status != Leader) {
        DPrintf("[func-Start-rf{%d}]  is not leader");
        *newLogIndex = -1;
        *newLogterm = -1;
        *isLeader = false;
        return;
    }

    raftRpcProctoc::LogEntry newLogEntry;
    newLogEntry.set_command(command.asString());
    newLogEntry.set_logterm(m_currentTerm);
    newLogEntry.set_logindex(getNewCommandIndex());
    m_logs.emplace_back(newLogEntry);

    int lastLogIndex = getLastLogIndex();

    DPrintf("[func-Start-rf{%d}]  lastLogIndex:%d,command:%s\n", m_me, lastLogIndex, &command);
    // 日志发生改变，需要持久化
    persist();
    *newLogIndex = newLogEntry.logindex();
    *newLogterm = newLogEntry.logterm();
    *isLeader = true;
}

// 初始化raft节点，包括peers节点集合，持久化节点，日志执行通道等
void Raft::init(std::vector<std::shared_ptr<RaftRpcUtil>>& peers, int me, std::shared_ptr<Persister> persister,
                std::shared_ptr<LockQueue<ApplyMsg>> applyCh) {
    m_peers = peers;
    m_persister = persister;
    m_me = me;
    m_mtx.lock();

    applyChan = applyCh;

    m_currentTerm = 0;
    m_status = Follower;
    m_commitIndex = 0;
    m_lastApplied = 0;
    m_logs.clear();
    for (int i = 0; i < m_peers.size(); ++i) {
        m_matchIndex.emplace_back(0);
        m_nextIndex.emplace_back(0);
    }
    m_votedFor = -1;

    m_lastSnapshotIncludeIndex = 0;
    m_lastSnapshotIncludeTerm = 0;
    m_lastResetElectionTime = now();
    m_lastResetHeartBeatTime = now();

    readPersist(m_persister->ReadRaftState());
    if (m_lastSnapshotIncludeIndex > 0) {
        m_lastApplied = m_lastSnapshotIncludeIndex;
    }
    DPrintf("[Init&ReInit] Sever %d, term %d, lastSnapshotIncludeIndex {%d} , lastSnapshotIncludeTerm {%d}", m_me,
            m_currentTerm, m_lastSnapshotIncludeIndex, m_lastSnapshotIncludeTerm);
    m_mtx.unlock();

    m_ioManager = std::make_unique<monsoon::IOManager>(FIBER_THREAD_NUM, FIBER_USE_CALLER_THREAD);

    // 使用协程来启动心跳定时器与选举定时器
    m_ioManager->scheduler([this]() -> void {this->leaderHearBeatTicker();});
    m_ioManager->scheduler([this]() -> void {this->electionTimeOutTicker();});
    // 对于日志执行定时器扔采用线程启动，其执行时间不是固定的，因为这个过程必须是顺序的，不能像上面两个过程一样允许异步
    std::thread t3(&Raft::applierTicker, this);
    t3.detach();
}

std::string Raft::persistData() {
    BoostPersistRaftNode boostPersistRaftNode;
    boostPersistRaftNode.m_currentTerm = m_currentTerm;
    boostPersistRaftNode.m_votedFor = m_votedFor;
    boostPersistRaftNode.m_lastSnapshotIncludeIndex = m_lastSnapshotIncludeIndex;
    boostPersistRaftNode.m_lastSnapshotIncludeTerm = m_lastSnapshotIncludeTerm;
    for (auto& item: m_logs) {
        boostPersistRaftNode.m_logs.emplace_back(item.SerializeAsString());
    }

    std::stringstream ss;
    boost::archive::text_oarchive oa(ss);
    oa << boostPersistRaftNode;
    return ss.str();
}

void Raft::readPersist(const std::string& data) {
    if (data.empty()) {
        return;
    }
    std::stringstream iss(data);
    boost::archive::text_iarchive ia(iss);
    BoostPersistRaftNode boostPersistRaftNode;
    ia >> boostPersistRaftNode;

    m_currentTerm = boostPersistRaftNode.m_currentTerm;
    m_votedFor = boostPersistRaftNode.m_votedFor;
    m_lastSnapshotIncludeIndex = boostPersistRaftNode.m_lastSnapshotIncludeIndex;
    m_lastSnapshotIncludeTerm = boostPersistRaftNode.m_lastSnapshotIncludeTerm;
    m_logs.clear();
    for (auto& item: boostPersistRaftNode.m_logs) {
        raftRpcProctoc::LogEntry logEntry;
        logEntry.ParseFromString(item);
        m_logs.emplace_back(logEntry);
    }
}

void Raft::Snapshot(int index, const std::string& snapshot) {
    std::lock_guard<std::mutex> lock(m_mtx);

    if (m_lastSnapshotIncludeIndex >= index || index > m_commitIndex) {
        DPrintf(
        "[func-Snapshot-rf{%d}] rejects replacing log with snapshotIndex %d as current snapshotIndex %d is larger or "
        "smaller ",
        m_me, index, m_lastSnapshotIncludeIndex);
        return;       
    }
    auto lastLogIndex = getLastLogIndex();

    // 制造此快照后剩余的日志
    int newLastSnapshotIncludeIndex = index;
    int newLastSnapshotIncludeTerm = m_logs[getSlicesIndexFromLogIndex(index)].logterm();
    std::vector<raftRpcProctoc::LogEntry> trunckedLogs;

    for (int i = index + 1; i <= lastLogIndex; ++i) {
        trunckedLogs.emplace_back(m_logs[getSlicesIndexFromLogIndex(i)]);
    }
    m_lastSnapshotIncludeIndex = newLastSnapshotIncludeIndex;
    m_lastSnapshotIncludeTerm = newLastSnapshotIncludeTerm;
    m_logs = trunckedLogs;
    m_commitIndex = std::max(m_commitIndex, index);
    m_lastApplied = std::max(m_commitIndex, index);

    m_persister->Save(persistData(), snapshot);

    DPrintf("[SnapShot]Server %d snapshot snapshot index {%d}, term {%d}, loglen {%d}", m_me, index,
          m_lastSnapshotIncludeTerm, m_logs.size());
    myAssert(m_logs.size() + m_lastSnapshotIncludeIndex == lastLogIndex,
           format("len(rf.logs){%d} + rf.lastSnapshotIncludeIndex{%d} != lastLogjInde{%d}", m_logs.size(),
                m_lastSnapshotIncludeIndex, lastLogIndex));   
}

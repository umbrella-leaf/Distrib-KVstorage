#pragma once

////////// 网络状态表示

// 网络异常
#include "ApplyMsg.h"
#include "Persister.h"
#include "iomanager.hpp"
#include "raftRPC.pb.h"
#include "raftRpcUtil.h"
#include "util.h"
#include "monsoon.h"
#include <boost/serialization/access.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
constexpr int Disconnected = 0;
// 网络状态
constexpr int AppNormal = 1;

////////// 投票状态
constexpr int Killed = 0;
constexpr int Voted = 1; // 本任期已经投过票
constexpr int Expire = 2; // 投票过期
constexpr int Normal = 3;

class Raft: public raftRpcProctoc::raftRpc {
private:
    std::mutex m_mtx;
    // 与集群其他节点通信对象的集合
    std::vector<std::shared_ptr<RaftRpcUtil>> m_peers;
    // 持久化对象
    std::shared_ptr<Persister> m_persister;
    // 节点序号
    int m_me;
    // 节点当前任期
    int m_currentTerm;
    // 当前投票的目标
    int m_votedFor;
    // 日志条目数组，每一条日志记录了执行的命令，以及命令发出时领导的任期
    std::vector<raftRpcProctoc::LogEntry> m_logs;

    // 如下两个状态，所有节点都在维护，注意已提交的日志不一定已经执行了，这两个过程是异步的

    // 已提交的最新日志index
    int m_commitIndex;
    // 已经汇报给上层状态机的最新日志的index
    int m_lastApplied;

    // 如下两个状态是由leader来维护

    // 维护所有follower节点的下一条要同步的日志index
    std::vector<int> m_nextIndex;
    // 维护所有followers节点的目前已经同步的最新日志的index
    std::vector<int> m_matchIndex;

    enum Status {
        Follower,
        Candidate,
        Leader
    };
    // 节点身份
    Status m_status;
    // KvServer与Raft节点通信通道，KvServer从这里取日志来执行
    std::shared_ptr<LockQueue<ApplyMsg>> applyChan;

    // 选举超时
    std::chrono::_V2::system_clock::time_point m_lastResetElectionTime;
    // 心跳超时，只对leader有效
    std::chrono::_V2::system_clock::time_point m_lastResetHeartBeatTime;

    // 储存了快照中最后一个日志的index和term
    int m_lastSnapshotIncludeIndex;
    int m_lastSnapshotIncludeTerm;

    // 协程管理器
    std::unique_ptr<monsoon::IOManager> m_ioManager = nullptr;

public:
    void AppendEntries1(const raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *reply);
    // 日志执行定时回调
    void applierTicker();
    bool CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, const std::string& snapshot);
    // 执行选举
    void doElection();
    // 发起心跳，leader only
    void doHeartBeat();
    // 每隔一段时间检查睡眠时间内有没有重置定时器，没有则说明超时了
    // 如果有则设置合适睡眠时间：睡眠到重置时间+超时时间
    void electionTimeOutTicker();
    std::vector<ApplyMsg> getApplyLogs();
    int getNewCommandIndex();
    void getPrevLogInfo(int server, int *preIndex, int *preTerm);
    void GetState(int *term, bool *isLeader);
    void InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest *args,
                        raftRpcProctoc::InstallSnapShotResponse *reply);
    void leaderHearBeatTicker();
    void leaderSendSnapShot(int server);
    void leaderUpdateCommitIndex();
    bool matchLog(int logIndex, int logTerm);
    void persist();
    void RequestVote(const raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *reply);
    bool UpToDate(int index, int term);
    int getLastLogIndex();
    int getLastLogTerm();
    void getLastLogIndexAndTerm(int *lastLogIndex, int *lastLogTerm);
    int getLogTermFromLogIndex(int logIndex);
    int GetRaftStateSize();
    int getSlicesIndexFromLogIndex(int logIndex);

    bool sendRequestVote(int server, std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
                        std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, std::shared_ptr<int> votedNum);
    bool sendAppendEntries(int server, std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
                            std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply, std::shared_ptr<int> appendNums);

    
    // 推送执行消息到KvServer服务层
    void pushMsgToKvServer(ApplyMsg msg);
    void readPersist(const std::string& data);
    std::string persistData();

    void Start(Op command, int *newLogIndex, int *newLogTerm, bool *isLeader);

    // index代表是快照apply应用的index,而snapshot代表的是上层service传来的快照字节流，包括了Index之前的数据
    // 这个函数的目的是把安装到快照里的日志抛弃，并安装快照数据，同时更新快照下标，属于peers自身主动更新，与leader发送快照不冲突
    // 即服务层主动发起请求raft保存snapshot里面的数据，index是用来表示snapshot快照执行到了哪条命令
    void Snapshot(int index, const std::string& snapshot);

public:
    // 重写基类方法,因为rpc远程调用真正调用的是这个方法
    // 序列化，反序列化等操作rpc框架都已经做完了，因此这里只需要获取值然后真正调用本地方法即可。
    void AppendEntries(google::protobuf::RpcController *controller, const ::raftRpcProctoc::AppendEntriesArgs *request,
                        ::raftRpcProctoc::AppendEntriesReply *response, ::google::protobuf::Closure *done) override;
    void InstallSnapShot(google::protobuf::RpcController *controller,
                        const ::raftRpcProctoc::InstallSnapshotRequest *request,
                        ::raftRpcProctoc::InstallSnapShotResponse *response, ::google::protobuf::Closure *done) override;
    void RequestVote(google::protobuf::RpcController *controller, const ::raftRpcProctoc::RequestVoteArgs *request,
                    ::raftRpcProctoc::RequestVoteReply *response, ::google::protobuf::Closure *done) override;

public:
    void init(std::vector<std::shared_ptr<RaftRpcUtil>>& peers, int me, std::shared_ptr<Persister> persister,
                std::shared_ptr<LockQueue<ApplyMsg>> applyCh);
private:
    class BoostPersistRaftNode {
    public:
        friend class boost::serialization::access;

        template<class Archive>
        void serialize(Archive& ar, const unsigned int version) {
            ar &m_currentTerm;
            ar &m_votedFor;
            ar &m_lastSnapshotIncludeIndex;
            ar &m_lastSnapshotIncludeTerm;
            ar &m_logs;
        }
        int m_currentTerm;
        int m_votedFor;
        int m_lastSnapshotIncludeIndex;
        int m_lastSnapshotIncludeTerm;
        std::vector<std::string> m_logs;
    };
};


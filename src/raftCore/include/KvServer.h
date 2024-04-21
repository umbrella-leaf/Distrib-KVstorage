#pragma once

#include "ApplyMsg.h"
#include "skipList.h"
#include "KvServerRPC.pb.h"
#include "raft.h"
#include "util.h"
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/access.hpp>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

class KvServer: public raftKVRpcProctoc::KvServerRpc {
private:
    std::mutex m_mtx;
    int m_me;
    std::shared_ptr<Raft> m_raftNode;
    std::shared_ptr<LockQueue<ApplyMsg>> applyChan;
    int m_maxRaftState;

    std::string m_serializedKVData;
    SkipList<std::string, std::string> m_skipList;
    // 日志index -> 日志执行完毕的通知通道
    std::unordered_map<int, LockQueue<Op> *> waitApplyCh;
    // 客户端Id -> 该客户端最近一次请求的ID，目的是维护线性一致性
    std::unordered_map<std::string, int> m_lastRequestId;

    int m_lastSnapshotRaftLogIndex;

public:
    // 禁止外部调用默认构造函数创建对象
    KvServer() = delete;

    KvServer(int me, int maxraftstate, const std::string &nodeInfoFileName, short port);

    void StartKVServer();

    void DprintfKVDB();

    void ExecuteAppendOpOnKVDB(Op& op);

    void ExecuteGetOpOnKVDB(Op& op, std::string *value, bool *exist);

    void ExecutePutOpOnKVDB(Op& op);

    void Get(const raftKVRpcProctoc::GetArgs *args, raftKVRpcProctoc::GetReply *reply);

    void GetCommandFromRaft(ApplyMsg& message);

    bool ifRequestDuplicate(const std::string &ClientId, int RequestId);

    void PutAppend(const raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply);

    // 读取从raft节点传来的日志提交
    void ReadRaftApplyCommandLoop();

    void ReadSnapShotToInstall(const std::string &snapshot);

    bool SendMessageToWaitChan(const Op &op, int raftIndex);

    // 检查是否需要制作快照
    void IfNeedToSendSnapShotCommand(int raftIndex, int proportion);

    void GetSnapShotFromRaft(ApplyMsg& message);

    std::string MakeSnapShot();

public:
    void PutAppend(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::PutAppendArgs *request,
                 ::raftKVRpcProctoc::PutAppendReply *response, ::google::protobuf::Closure *done) override;
    void Get(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::GetArgs *request,
            ::raftKVRpcProctoc::GetReply *response, ::google::protobuf::Closure *done) override;

private:
    friend class boost::serialization::access;
    // 序列化支持
    template<class Archive>
    void serialize(Archive &ar, const unsigned int version) {
        ar &m_serializedKVData;
        ar &m_lastRequestId;
    }

    std::string GetSnapShotData() {
        m_serializedKVData = m_skipList.dump_file();
        std::stringstream ss;
        boost::archive::text_oarchive oa(ss);
        oa << *this;
        m_serializedKVData.clear();
        return ss.str();
    }

    void ParseFromString(const std::string &str) {
        std::stringstream ss(str);
        boost::archive::text_iarchive ia(ss);
        ia >> *this;
        m_skipList.load_file(m_serializedKVData);
        m_serializedKVData.clear();
    }
};
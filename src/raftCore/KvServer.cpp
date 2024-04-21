#include "KvServer.h"
#include "ApplyMsg.h"
#include "Persister.h"
#include "config.h"
#include "raftRpcUtil.h"
#include "util.h"
#include <climits>
#include <memory>
#include <mutex>
#include <rpcprovider.h>
#include <mprpcconfig.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

void KvServer::DprintfKVDB() {
    if (!Debug) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mtx);
    DEFER {
        m_skipList.display_list();
    };
}

void KvServer::ExecuteAppendOpOnKVDB(Op& op) {
    m_mtx.lock();
    m_skipList.insert_set_element(op.key, op.value);
    m_lastRequestId[op.clientId] = op.requestId;
    m_mtx.unlock();

    DprintfKVDB();
}

void KvServer::ExecuteGetOpOnKVDB(Op& op, std::string *value, bool *exist) {
    m_mtx.lock();
    *value = "";
    *exist = false;
    if (m_skipList.search_element(op.key, *value)) {
        *exist = true;
    }
    m_lastRequestId[op.clientId] = op.requestId;
    m_mtx.unlock();

    DprintfKVDB();
}

void KvServer::ExecutePutOpOnKVDB(Op& op) {
    m_mtx.lock();
    m_skipList.insert_set_element(op.key, op.value);
    m_lastRequestId[op.clientId] = op.requestId;
    m_mtx.unlock();
    
    DprintfKVDB();
}

void KvServer::Get(const raftKVRpcProctoc::GetArgs *args, raftKVRpcProctoc::GetReply *reply) {
    Op op;
    op.operation = "Get";
    op.key = args->key();
    op.value = "";
    op.clientId = args->clientid();
    op.requestId = args->requestid();

    int raftIndex = -1;
    int _ = -1;
    bool isLeader = false;
    // 将日志送到raft节点，等待之后执行
    m_raftNode->Start(op, &raftIndex, &_, &isLeader);
    if (!isLeader) {
        reply->set_err(ErrWrongLeader);
        return;
    }

    m_mtx.lock();
    if (waitApplyCh.find(raftIndex) != waitApplyCh.end()) {
        waitApplyCh.emplace(raftIndex, new LockQueue<Op>());
    }
    auto chForRaftIndex = waitApplyCh[raftIndex];
    m_mtx.unlock();

    // 等待命令执行完毕
    Op raftCommitOp;
    // 执行超时
    if (!chForRaftIndex->timeoutPop(CONSENSUS_TIMEOUT, &raftCommitOp)) {
        int _ = -1;
        bool isLeader = false;
        m_raftNode->GetState(&_, &isLeader);
        // 如果读命令是重复的，允许再次执行，因为读命令是幂等的
        if (ifRequestDuplicate(op.clientId, op.requestId)) {
            std::string value;
            bool exist = false;
            ExecuteGetOpOnKVDB(op, &value, &exist);
            if (exist) {
                reply->set_err(OK);
                reply->set_value(value);
            } else {
                reply->set_err(ErrNoKey);
                reply->set_value("");
            }
        } else {
            // 读命令不是重复的，那么就回复拒绝让客户端换一个节点再试
            reply->set_err(ErrWrongLeader);
        }
    // 执行未超时
    } else {
        // 校验目的是保证收到执行完毕的命令式原命令
        if (raftCommitOp.clientId == op.clientId && raftCommitOp.requestId == op.requestId) {
            std::string value;
            bool exist = false;
            ExecuteGetOpOnKVDB(op, &value, &exist);
            if (exist) {
                reply->set_err(OK);
                reply->set_value(value);
            } else {
                reply->set_err(ErrNoKey);
                reply->set_value("");
            }
        } else {
            reply->set_err(ErrWrongLeader);
        }
    }
    m_mtx.lock();
    auto tmp = waitApplyCh[raftIndex];
    waitApplyCh.erase(raftIndex);
    delete tmp;
    m_mtx.unlock();
}

void KvServer::GetCommandFromRaft(ApplyMsg &message) {
    Op op;
    op.parseFromString(message.Command);

    DPrintf(
        "[KvServer::GetCommandFromRaft-kvserver{%d}] , Got Command --> Index:{%d} , ClientId {%s}, RequestId {%d}, "
        "Opreation {%s}, Key :{%s}, Value :{%s}",
        m_me, message.CommandIndex, &op.clientId, op.requestId, &op.operation, &op.key, &op.value);
    // 获取日志的index比当前快照最后一条日志还要旧，不予执行
    if (message.CommandIndex <= m_lastSnapshotRaftLogIndex) {
        return;
    }

    if (!ifRequestDuplicate(op.clientId, op.requestId)) {
        if (op.operation == "Put") {
            ExecutePutOpOnKVDB(op);
        }
        if (op.operation == "Append") {
            ExecuteAppendOpOnKVDB(op);
        }
    }

    if (m_maxRaftState != -1) {
        IfNeedToSendSnapShotCommand(message.CommandIndex, 9);
    }

    SendMessageToWaitChan(op, message.CommandIndex);
}

bool KvServer::ifRequestDuplicate(const std::string &clientId, int requestId) {
    std::lock_guard<std::mutex> lock(m_mtx);
    // 此用户还未发送过命令，当然不可能有重复请求
    if (m_lastRequestId.find(clientId) == m_lastRequestId.end()) {
        return false;
    }
    return requestId <= m_lastRequestId[clientId];
}

void KvServer::PutAppend(const raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply) {
    Op op;
    op.operation = args->op();
    op.key = args->key();
    op.value = args->value();
    op.clientId = args->clientid();
    op.requestId = args->requestid();
    int raftIndex = -1;
    int _ = -1;
    bool isLeader = false;

    m_raftNode->Start(op, &raftIndex, &_, &isLeader);
    if (!isLeader) {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]From Client %s (Request %d) To Server %d, key %s, raftIndex %d , but "
            "not leader",
            m_me, &args->clientid(), args->requestid(), m_me, &op.key, raftIndex);

        reply->set_err(ErrWrongLeader);
        return;
    }
    DPrintf(
        "[func -KvServer::PutAppend -kvserver{%d}]From Client %s (Request %d) To Server %d, key %s, raftIndex %d , is "
        "leader ",
        m_me, &args->clientid(), args->requestid(), m_me, &op.key, raftIndex);

    m_mtx.lock();
    if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) {
        waitApplyCh.emplace(raftIndex, new LockQueue<Op>());
    }
    auto chForRaftIndex = waitApplyCh[raftIndex];
    m_mtx.unlock();

    // 等待命令执行完毕
    Op raftCommitOp;
    if (!chForRaftIndex->timeoutPop(CONSENSUS_TIMEOUT, &raftCommitOp)) {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]TIMEOUT PUTAPPEND !!!! Server %d , get Command <-- Index:%d , "
            "ClientId %s, RequestId %s, Opreation %s Key :%s, Value :%s",
            m_me, m_me, raftIndex, &op.clientId, op.requestId, &op.operation, &op.key, &op.value);
        // 由于put/append命令不幂等，因此重复命令不可以再执行，而是返回OK
        if (ifRequestDuplicate(op.clientId, op.requestId)) {
            reply->set_err(OK);
        } else {
            reply->set_err(ErrWrongLeader);
        }
    } else {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]WaitChanGetRaftApplyMessage<--Server %d , get Command <-- Index:%d , "
            "ClientId %s, RequestId %d, Opreation %s, Key :%s, Value :%s",
            m_me, m_me, raftIndex, &op.clientId, op.requestId, &op.operation, &op.key, &op.value);
        if (raftCommitOp.clientId == op.clientId && raftCommitOp.requestId == op.requestId) {
            reply->set_err(OK);
        } else {
            reply->set_err(ErrWrongLeader);
        }
    }
    m_mtx.lock();
    auto tmp = waitApplyCh[raftIndex];
    waitApplyCh.erase(raftIndex);
    delete tmp;
    m_mtx.unlock();
}

void KvServer::ReadRaftApplyCommandLoop() {
    while (true) {
        auto message = applyChan->pop();
        DPrintf(
            "---------------tmp-------------[func-KvServer::ReadRaftApplyCommandLoop()-kvserver{%d}] 收到了下raft的消息",
            m_me);
        if (message.CommandValid) {
            GetCommandFromRaft(message);
        }
        if (message.SnapShotValid) {
            GetSnapShotFromRaft(message);
        }
    }
}

void KvServer::ReadSnapShotToInstall(const std::string &snapshot) {
    if (snapshot.empty()) {
        return;
    }
    ParseFromString(snapshot);
}

bool KvServer::SendMessageToWaitChan(const Op &op, int raftIndex) {
    std::lock_guard<std::mutex> lock(m_mtx);
    DPrintf(
        "[RaftApplyMessageSendToWaitChan--> raftserver{%d}] , Send Command --> Index:{%d} , ClientId {%d}, RequestId "
        "{%d}, Opreation {%v}, Key :{%v}, Value :{%v}",
        m_me, raftIndex, &op.clientId, op.requestId, &op.operation, &op.key, &op.value);
    
    if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) {
        return false;
    }
    waitApplyCh[raftIndex]->Push(op);
    DPrintf(
        "[RaftApplyMessageSendToWaitChan--> raftserver{%d}] , Send Command --> Index:{%d} , ClientId {%d}, RequestId "
        "{%d}, Opreation {%v}, Key :{%v}, Value :{%v}",
        m_me, raftIndex, &op.clientId, op.requestId, &op.operation, &op.key, &op.value);
    return true;    
}

void KvServer::IfNeedToSendSnapShotCommand(int raftIndex, int proportion) {
    // 当前raft节点日志数量超过最大数量的1/10，就制作快照，防止m_logs变得太大
    if (m_raftNode->GetRaftStateSize() > m_maxRaftState / 10.0) {
        auto snapshot = MakeSnapShot();
        m_raftNode->Snapshot(raftIndex, snapshot);
    }
}

void KvServer::GetSnapShotFromRaft(ApplyMsg &message) {
    std::lock_guard<std::mutex> lock(m_mtx);

    if (m_raftNode->CondInstallSnapshot(message.SnapshotTerm, message.SnapshotIndex, message.Snapshot)) {
        ReadSnapShotToInstall(message.Snapshot);
        m_lastSnapshotRaftLogIndex = message.SnapshotIndex;
    }
}

std::string KvServer::MakeSnapShot() {
    std::lock_guard<std::mutex> lock(m_mtx);
    std::string snapshotData = GetSnapShotData();
    return snapshotData;
}

void KvServer::PutAppend(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::PutAppendArgs *request,
                        ::raftKVRpcProctoc::PutAppendReply *response, ::google::protobuf::Closure *done) {
    KvServer::PutAppend(request, response);
    done->Run();
}

void KvServer::Get(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::GetArgs *request,
                    ::raftKVRpcProctoc::GetReply *response, ::google::protobuf::Closure *done) {
    KvServer::Get(request, response);
    done->Run();
}

KvServer::KvServer(int me, int maxraftstate, const std::string &nodeInfoFileName, short port): m_skipList(6) {
    auto persister = std::make_shared<Persister>(me);

    m_me = me;
    m_maxRaftState = maxraftstate;

    applyChan = std::make_shared<LockQueue<ApplyMsg>>();
    m_raftNode = std::make_shared<Raft>();
    // 启动线程来启动服务器
    std::thread t([this, port]()-> void {
        RpcProvider provider;
        // 初始化rpcprovider服务器，并注册所有服务
        provider.NotifyService(this);
        provider.NotifyService(m_raftNode.get());
        provider.Run(m_me, port);
    });
    t.detach();
    // 睡眠足够时间来等待集群全部的节点都启动
    std::cout << "raftServer node:" << m_me << " start to sleep to wait all ohter raftnode start!!!!" << std::endl;
    sleep(6);
    std::cout << "raftServer node:" << m_me << " wake up!!!! start to connect other raftnode" << std::endl;
    // 获取所有集群内peer节点
    MprpcConfig config;
    config.LoadConfigFile(nodeInfoFileName.c_str());
    // ip->端口映射数组
    std::vector<std::pair<std::string, short>> ipPortVt;
    for (int i = 0; i < INT_MAX - 1; ++i) {
        std::string node = "node" + std::to_string(i);
        std::string nodeIp = config.Load(node + "ip");
        std::string nodePortStr = config.Load(node + "port");
        if (nodeIp.empty()) {
            break;
        }
        ipPortVt.emplace_back(nodeIp, atoi(nodePortStr.c_str()));
    }
    std::vector<std::shared_ptr<RaftRpcUtil>> servers;
    // 进行连接
    for (int i = 0; i < ipPortVt.size(); ++i) {
        if (i == m_me) {
            servers.emplace_back(nullptr);
            continue;
        }
        std::string &otherNodeIp = ipPortVt[i].first;
        short otherNodePort = ipPortVt[i].second;
        auto *rpc = new RaftRpcUtil(otherNodeIp, otherNodePort);
        servers.emplace_back(std::shared_ptr<RaftRpcUtil>(rpc));

        std::cout << "node" << m_me << " 连接node" << i << "success!" << std::endl;
    }
    // 这里的睡眠是等待所有通信节点相互连接成功，再启动raft节点
    sleep(ipPortVt.size() - me);
    m_raftNode->init(servers, m_me, persister, applyChan);

    m_lastSnapshotRaftLogIndex = 0;
    auto &&snapshot = persister->ReadSnapshot();
    if (!snapshot.empty()) {
        ReadSnapShotToInstall(snapshot);
    }
    // 启动线程来从raft节点的消息通道中读取日志，用join来阻塞从而使得这个函数不会退出
    std::thread t2(&KvServer::ReadRaftApplyCommandLoop, this);
    t2.join();
}
#pragma once

#include "raftRPC.pb.h"

// 维护raft节点之间的通信服务，充当客户端的作用
// 本质是对protobuf自动生成的stub的封装
class RaftRpcUtil {
private:
    raftRpcProctoc::raftRpc_Stub *stub_;

public:
    // 主动调用其他节点的raft RPC服务相关方法
    bool AppendEntires(raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *response);
    bool InstallSnapShot(raftRpcProctoc::InstallSnapshotRequest *args, raftRpcProctoc::InstallSnapShotResponse *response);
    bool RequestVote(raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *response);

    RaftRpcUtil(const std::string &ip, short port);
    ~RaftRpcUtil();
};
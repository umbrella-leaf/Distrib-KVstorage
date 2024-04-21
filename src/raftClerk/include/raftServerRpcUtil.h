#pragma once

// 与其他KvServer的连接类，注意和raft节点是分开的
#include "KvServerRPC.pb.h"
class raftServerRpcUtil {
private:
    raftKVRpcProctoc::KvServerRpc_Stub* stub;

public:
    bool Get(raftKVRpcProctoc::GetArgs *GetArgs, raftKVRpcProctoc::GetReply *GetReply);
    bool PutAppend(raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply);

    raftServerRpcUtil(const std::string &ip, short port);
    ~raftServerRpcUtil();
};
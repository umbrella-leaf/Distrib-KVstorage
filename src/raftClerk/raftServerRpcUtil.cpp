#include "raftServerRpcUtil.h"
#include "mprpcchannel.h"
#include "mprpccontroller.h"

raftServerRpcUtil::raftServerRpcUtil(const std::string &ip, short port) {
    stub = new raftKVRpcProctoc::KvServerRpc_Stub(new MprpcChannel(ip, port, false));
}

raftServerRpcUtil::~raftServerRpcUtil() {delete stub;}

bool raftServerRpcUtil::Get(raftKVRpcProctoc::GetArgs *GetArgs, raftKVRpcProctoc::GetReply *GetReply) {
    MprpcController controller;
    stub->Get(&controller, GetArgs, GetReply, nullptr);
    return !controller.Failed();
}

bool raftServerRpcUtil::PutAppend(raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply) {
    MprpcController controller;
    stub->PutAppend(&controller, args, reply, nullptr);
    if (controller.Failed()) {
        std::cout << controller.ErrorText() << std::endl;
    }
    return !controller.Failed();
}
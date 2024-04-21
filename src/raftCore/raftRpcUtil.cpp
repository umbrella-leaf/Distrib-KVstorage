#include "raftRpcUtil.h"

#include <mprpcchannel.h>
#include <mprpccontroller.h>

bool RaftRpcUtil::AppendEntires(raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *response) {
    MprpcController controller;
    stub_->AppendEntries(&controller, args, response, nullptr);
    return !controller.Failed();
}

bool RaftRpcUtil::InstallSnapShot(raftRpcProctoc::InstallSnapshotRequest *args, raftRpcProctoc::InstallSnapShotResponse *response) {
    MprpcController controller;
    stub_->InstallSnapShot(&controller, args, response, nullptr);
    return !controller.Failed();
}

bool RaftRpcUtil::RequestVote(raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *response) {
    MprpcController controller;
    stub_->RequestVote(&controller, args, response, nullptr);
    return !controller.Failed();
}

RaftRpcUtil::RaftRpcUtil(const std::string &ip, short port) {
    stub_ = new raftRpcProctoc::raftRpc_Stub(new MprpcChannel(ip, port, true));
}

RaftRpcUtil::~RaftRpcUtil() {
    // 不用delete channel，因为stub在析构时已经删除了channel
    delete stub_;
}


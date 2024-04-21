#include "rpcExample/friend.pb.h"
#include "rpcprovider.h"
#include <cstdint>
#include <vector>

class FriendService: public fixbug::FriendServiceRpc {
public:
    std::vector<std::string> GetFriendList(uint32_t userid) {
        std::cout << "local do GetFriendsList service! userid:" << userid << std::endl;
        std::vector<std::string> vec;
        vec.emplace_back("hu haoyu");
        vec.emplace_back("wang jinshi");
        vec.emplace_back("ma xiaoyan");
        return vec;
    }

    void GetFriendList(::google::protobuf::RpcController *controller, const ::fixbug::GetFriendListRequest *request,
                      ::fixbug::GetFriendListResponse *response, ::google::protobuf::Closure *done) {
        uint32_t userid = request->userid();
        std::vector<std::string> friendList = GetFriendList(userid);
        response->mutable_result()->set_errcode(0);
        response->mutable_result()->set_errmsg("");
        for (std::string &name: friendList) {
            std::string *p = response->add_friends();
            *p = name;
        }
        done->Run();
    }
};

int main(int argc, char **argv) {
    std::string ip = "127.0.0.1";
    short port = 7788;
    
    RpcProvider provider;
    provider.NotifyService(new FriendService());
    provider.Run(1, 7788);
    return 0;
}
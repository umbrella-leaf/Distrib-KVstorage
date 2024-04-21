#pragma once

#include "raftServerRpcUtil.h"
#include <memory>
#include <string>
#include <vector>
class Clerk {
private:
    std::vector<std::shared_ptr<raftServerRpcUtil>> m_servers;
    std::string m_clientId;
    int m_requestId;
    // 最新的领导Id
    int m_recentLeaderId;

    std::string uuid() {
        return std::to_string(rand()) + std::to_string(rand()) + std::to_string(rand()) + std::to_string(rand());
    }

    void PutAppend(const std::string& key, const std::string &value, const std::string &op);

public:
    void Init(const std::string &configName);
    // 对外暴露的三个功能
    std::string Get(const std::string& key);
    void Put(const std::string& key, const std::string& value);
    void Append(const std::string& key, const std::string& value);

public:
    Clerk();
};
#pragma once
#include <google/protobuf/descriptor.h>
#include <memory>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/net/TcpServer.h>
#include <string>
#include <unordered_map>
#include <google/protobuf/service.h>

// 发布Rpc服务的对象节点(可注册多个rpc服务)，内部包含一个muduo服务器
class RpcProvider {
public:
    // 用于注册rpc服务
    void NotifyService(google::protobuf::Service *service);
    
    // 启动rpc服务节点，开始提供rpc调用
    void Run(int nodeIndex, short port);

private:
    muduo::net::EventLoop m_eventLoop;
    std::shared_ptr<muduo::net::TcpServer> m_server;

    // service服务类型信息
    struct ServiceInfo {
        google::protobuf::Service *m_service;
        std::unordered_map<std::string, const google::protobuf::MethodDescriptor *> m_methodMap; 
    };
    // 储存注册的服务对象和对应方法的全部信息
    std::unordered_map<std::string, ServiceInfo> m_serviceMap;

    // socket新连接建立回调
    void OnConnection(const muduo::net::TcpConnectionPtr &);
    // 已有连接的读写事件回调
    void OnMessage(const muduo::net::TcpConnectionPtr &, muduo::net::Buffer *);
    // rpc调用结束时的回调，用于序列化rpc响应并通过TCP连接回送
    void SendRpcResponse(const muduo::net::TcpConnectionPtr &, google::protobuf::Message *);

public:
    ~RpcProvider();
};
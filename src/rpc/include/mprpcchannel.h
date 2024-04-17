#pragma once

// 实际发送与接收rpc消息的类
#include <cstdint>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <string>

class MprpcChannel: public google::protobuf::RpcChannel {
public:
    // 通过Rpc_Stub代理对象调用的rpc方法，最终转发到这里，实现了基类的同名纯虚函数
    // 主要是完成请求序列化和发送，以及响应接收和反序列化工作
    void CallMethod(const google::protobuf::MethodDescriptor *method, google::protobuf::RpcController *controller,
                    const google::protobuf::Message *request, google::protobuf::Message *response,
                    google::protobuf::Closure *done) override;
    MprpcChannel(std::string ip, short port, bool connectNow);

private:
    // 客户端socket连接描述符
    int m_clientFd;
    std::string m_ip;
    const uint16_t m_port;
    // 连接指定ip与端口，并设置m_clientFd
    bool newConnect(const char *ip, uint16_t port, std::string *errMsg);
};
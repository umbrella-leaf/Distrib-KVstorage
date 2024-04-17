#include "mprpcchannel.h"
#include "rpcheader.pb.h"
#include "util.h"
#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

void MprpcChannel::CallMethod(const google::protobuf::MethodDescriptor *method, google::protobuf::RpcController *controller,
                const google::protobuf::Message *request, google::protobuf::Message *response,
                google::protobuf::Closure *done) {
    if (m_clientFd == -1) {
        std::string errMsg;
        bool rt = newConnect(m_ip.c_str(), m_port, &errMsg);
        if (!rt) {
            DPrintf("[func-MprpcChannel::CallMethod]重连接ip：{%s} port{%d}失败", m_ip.c_str(), m_port);
            controller->SetFailed(errMsg);
            return;
        }
        DPrintf("[func-MprpcChannel::CallMethod]连接ip：{%s} port{%d}成功", m_ip.c_str(), m_port);
    }

    const google::protobuf::ServiceDescriptor *sd = method->service();
    const std::string& service_name = sd->name();
    const std::string& method_name = method->name();

    // 获取序列化的参数长度args_size
    uint32_t args_size{};
    std::string args_str;
    if (!request->SerializeToString(&args_str)) {
        controller->SetFailed("serialize request error!");
        return;
    }
    args_size = args_str.size();
    // 填充RpcHeader对象
    RPC::RpcHeader rpcHeader;
    rpcHeader.set_service_name(service_name);
    rpcHeader.set_method_name(method_name);
    rpcHeader.set_args_size(args_size);
    // 序列化RpcHeader
    std::string rpc_header_str;
    if (!rpcHeader.SerializeToString(&rpc_header_str)) {
        controller->SetFailed("serialize rpc header error!");
        return;
    }
    // 使用protobuf自带的CodedOutputStream来构建发送数据流
    std::string send_rpc_str;
    {
        // 为什么用StringOutputStream，因为需要往字符串后添加内容
        // 在rpcProvider读取请求时使用ArrayInputStream，原因是没有StringInputStream
        google::protobuf::io::StringOutputStream string_output(&send_rpc_str);
        google::protobuf::io::CodedOutputStream coded_output(&string_output);

        // 写入header长度
        coded_output.WriteVarint32(static_cast<uint32_t>(rpc_header_str.size()));
        // 然后写入序列化的header
        coded_output.WriteString(rpc_header_str);
    }
    // 最后将请求参数附加到send_rpc_str末尾
    send_rpc_str += args_str;

    // 发送真正的rpc请求(通过socket)，失败会重连
    while (-1 == send(m_clientFd, send_rpc_str.c_str(), send_rpc_str.size(), 0)) {
        char errtxt[512]{0};
        std::sprintf(errtxt, "send error! errno:%d", errno);
        std::cout << "尝试重新连接，对方ip：" << m_ip << " 对方端口" << m_port << std::endl;
        close(m_clientFd);
        m_clientFd = -1;
        std::string errMsg;
        bool rt = newConnect(m_ip.c_str(), m_port, &errMsg);
        if (!rt) {
            controller->SetFailed(errMsg);
            return;
        }
    }
    // 等待rpc响应返回
    char recv_buf[1024]{0};
    // 由于rpc响应中不包含任何长度信息，因此需要全量接收，然后获取实际接收长度
    int recv_size = 0;
    if (-1 == (recv_size = recv(m_clientFd, recv_buf, 1024, 0))) {
        close(m_clientFd);
        m_clientFd = -1;
        char errtxt[512]{0};
        sprintf(errtxt, "recv error! errno:%d", errno);
        controller->SetFailed(errtxt);
        return;
    }

    // 反序列化rpc响应
    if (!response->ParseFromArray(recv_buf, recv_size)) {
        char errtxt[1050]{0};
        sprintf(errtxt, "parse error! response_str:%s", recv_buf);
        controller->SetFailed(errtxt);
        return;
    }
    // 此时response已经反序列化完成
}

bool MprpcChannel::newConnect(const char* ip, uint16_t port, std::string* errMsg) {
    int clientfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (-1 == clientfd) {
        char errtxt[512]{0};
        sprintf(errtxt, "create socket error! errno:%d", errno);
        m_clientFd = -1;
        // 此处char*转string会发生拷贝，遇到\0结束，因此不需要担心errtxt被释放
        *errMsg = errtxt;
        return false;;
    }
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);
    // 连接rpc服务端节点
    if (-1 == connect(clientfd, (struct sockaddr *)&server_addr, sizeof(server_addr))) {
        close(clientfd);
        char errtxt[512]{0};
        sprintf(errtxt, "connect fail! errno:%d", errno);
        m_clientFd = -1;
        *errMsg = errtxt;
        return false;
    }
    m_clientFd = clientfd;
    return true;
}

MprpcChannel::MprpcChannel(std::string ip, short port, bool connectNow): m_ip(ip), m_port(port), m_clientFd(-1) {
    if (!connectNow) {
        return;
    }
    std::string errMsg;
    auto rt = newConnect(ip.c_str(), port, &errMsg);
    int tryCount = 3;
    while (!rt && --tryCount) {
        std::cout << errMsg << std::endl;
        rt = newConnect(ip.c_str(), port, &errMsg);
    }
}
#include "rpcprovider.h"
#include "rpcheader.pb.h"
#include <arpa/inet.h>
#include <fstream>
#include <netdb.h>
#include <string>

/*
服务集合(
    服务名称 => 服务信息{
        服务对象指针, 方法集合(
            方法名称 => 方法描述符
        )
    }
)
*/
// 框架提供给外部使用的，可以注册rpc服务的接口
void RpcProvider::NotifyService(google::protobuf::Service* service) {
    ServiceInfo service_info;

    // 获取服务对象的描述信息以及名称
    const google::protobuf::ServiceDescriptor* pServiceDesc = service->GetDescriptor();
    const std::string& service_name = pServiceDesc->name();
    // 获取服务对象的方法数量
    int methodCount = pServiceDesc->method_count();
    std::cout << "service_name:" << service_name << std::endl;

    // 获取该服务全部的方法名以及方法描述符(以便获取方法索引)
    for (int i = 0; i < methodCount; ++i) {
        const google::protobuf::MethodDescriptor *pmethodDesc = pServiceDesc->method(i);
        const std::string& method_name = pmethodDesc->name();
        service_info.m_methodMap.emplace(method_name, pmethodDesc);
    }
    service_info.m_service = service;
    m_serviceMap.emplace(service_name, service_info);
}

// 启动rpc服务节点，开始提供rpc远程网络调用
void RpcProvider::Run(int nodeIndex, short port) {
    // 获取可用ip
    char* ipC;
    char hname[128];
    struct hostent *hent;
    // 通过gethostname获取本地主机名
    gethostname(hname, sizeof(hname));
    // 通过gethostbyname获取本地主机名对应的主机信息
    hent = gethostbyname(hname);
    // 从主机IP列表中选出非0 IP，注意要使用inet_ntoa来将二进制格式地址转为点分十进制
    // 方法是现将二进制IP转换为in_addr结构体，再调用inet_ntoa
    for (int i = 0; hent->h_addr_list[i]; ++i) {
        ipC = inet_ntoa(*(struct in_addr *)(hent->h_addr_list[i]));
    }
    std::string ip = std::string(ipC);

    // 写入文件 "test.conf"
    std::string node = "node" + std::to_string(nodeIndex);
    std::ofstream outfile;
    // 打开文件，追加写入
    outfile.open("test.conf", std::ios::app);
    if (!outfile.is_open()) {
        std::cout << "打开文件失败！" << std::endl;
        exit(EXIT_FAILURE);
    }
    outfile << node + "ip=" + ip << std::endl;
    outfile << node + "port=" + std::to_string(port) << std::endl;
    outfile.close();

    // 创建服务器
    muduo::net::InetAddress address(ip, port);

    // 创建TcpServer对象
    m_server = std::make_shared<muduo::net::TcpServer>(&m_eventLoop, address, "RpcProvider"); 

    // 绑定连接回调和消息读写回调方法
    m_server->setConnectionCallback([this](const muduo::net::TcpConnectionPtr &conn) {this->OnConnection(conn);});
    m_server->setMessageCallback([this](const muduo::net::TcpConnectionPtr &conn, muduo::net::Buffer *buffer, muduo::Timestamp) {
        this->OnMessage(conn, buffer);
    });


}

// 新的socket连接回调
void RpcProvider::OnConnection(const muduo::net::TcpConnectionPtr &conn) {
    // 如果是客户端断开连接
    if (!conn->connected()) {
        // 服务端也断开连接
        conn->shutdown();
    }
}

/*
protobuf消息格式为：
header长度(varint32) | 头部header(服务名，方法名，参数长度) | 参数
*/
// 接收rpc调用客户端发来的TCP请求并按照上述格式进行消息解析，最后调用指定的服务方法
void RpcProvider::OnMessage(const muduo::net::TcpConnectionPtr &conn, muduo::net::Buffer *buffer) {
    // 获取接收的数据流，读Buffer
    std::string recv_buf = buffer->retrieveAllAsString();

    // 使用protobuf的CodedInputStream来解析数据流
    // 字符串 => 输入流 => 解码输入流
    google::protobuf::io::ArrayInputStream array_input(recv_buf.data(), recv_buf.size());
    google::protobuf::io::CodedInputStream coded_input(&array_input);
    uint32_t header_size{};

    coded_input.ReadVarint32(&header_size);

    // 根据得到的头部长度来读取RpcHeader并反序列化。从而得到rpc请求的服务名称与方法名称、参数等
    std::string rpc_header_str;
    RPC::RpcHeader rpcHeader{};

    // 注意设置读取大小限制
    google::protobuf::io::CodedInputStream::Limit msg_limit = coded_input.PushLimit(header_size);
    coded_input.ReadString(&rpc_header_str, header_size);
    coded_input.PopLimit(msg_limit);
    if (!rpcHeader.ParseFromString(rpc_header_str)) {
        std::cout << "rpc_header_str:" << rpc_header_str << " parse error!" << std::endl;
        return;
    }

    // 读取服务和方法名，参数长度
    const std::string& service_name = rpcHeader.service_name();
    const std::string& method_name = rpcHeader.method_name();
    uint32_t args_size = rpcHeader.args_size();

    // 读取rpc调用的参数
    std::string args_str;
    bool read_arg_success = coded_input.ReadString(&args_str, args_size);
    if (!read_arg_success) {
        return;
    }

    // 根据服务名和方法名来获取service对象以及method描述符
    auto it = m_serviceMap.find(service_name);
    if (it == m_serviceMap.end()) {
        std::cout << "服务: " << service_name << " is not existed!" << std::endl;
        std::cout << "当前已经有的服务列表为:";
        for (auto& [s_name, _]: m_serviceMap) {
            std::cout << s_name << " ";
        }
        std::cout << std::endl;
        return;
    }

    auto mit = it->second.m_methodMap.find(method_name);
    if (mit == it->second.m_methodMap.end()) {
        std::cout << service_name << ":" << method_name << " is not exist!" << std::endl;
        return;
    }

    // 获取服务对象和方法描述符
    google::protobuf::Service *service = it->second.m_service;
    const google::protobuf::MethodDescriptor *method = mit->second;

    // 生成rpc调用的请求对象，以及响应对象
    google::protobuf::Message *request = service->GetRequestPrototype(method).New();
    if (!request->ParseFromString(args_str)) {
        std::cout << "request parse error, content:" << args_str << std::endl;
        return;
    }
    google::protobuf::Message *response = service->GetResponsePrototype(method).New();

    // 给真实方法调用指定一个结束回调Closure，在真实方法调用结束后执行该回调run方法
    google::protobuf::Closure *done = 
        google::protobuf::NewCallback<RpcProvider, const muduo::net::TcpConnectionPtr &, google::protobuf::Message *>(
            this, &RpcProvider::SendRpcResponse, conn, response
        );
    
    // 执行真实调用
    service->CallMethod(method, nullptr, request, response, done);
}

// 真实调用结束后的回调，主要用于序列化响应对象并通过tcp发送回去
// 这里不需要RpcHeader，只需要单纯回送一个序列化响应即可
void RpcProvider::SendRpcResponse(const muduo::net::TcpConnectionPtr &conn, google::protobuf::Message *response) {
    std::string response_str;
    if (!response->SerializeToString(&response_str)) {
        std::cout << "serialize response_str error!" << std::endl;
        return;
    }
    conn->send(response_str);
}

RpcProvider::~RpcProvider() {
    std::cout << "[func - RpcProvider::~RpcProvider()]: ip和port信息：" << m_server->ipPort() << std::endl;
    m_eventLoop.quit();
}

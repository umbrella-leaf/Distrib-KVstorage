#pragma once

#include <google/protobuf/service.h>
#include <string>

/*
    rpc控制器类，在此处唯一的作用是检查响应是否成功
*/
class MprpcController: public google::protobuf::RpcController {
public:
    MprpcController();
    void Reset() override;
    bool Failed() const override;
    std::string ErrorText() const override;
    void SetFailed(const std::string& reason) override;

    // 目前未实现具体功能，但是由于必须重写基类所有纯虚函数，因此必须定义
    void StartCancel() override;
    bool IsCanceled() const override;
    void NotifyOnCancel(google::protobuf::Closure* callback) override;

private:
    bool m_failed; // rpc调用执行结果
    std::string m_errText; // rpc调用错误信息
};
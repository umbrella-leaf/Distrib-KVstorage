#pragma once

#include <fstream>
#include <mutex>
#include <string>
class Persister {
private:
    std::mutex m_mutex;
    std::string m_raftState;
    std::string m_snapshot;
    // 持久化raft状态的文件名称
    const std::string m_raftStateFileName;
    // 持久化数据库快照的文件名称
    const std::string m_snapshotFileName;
    // raft状态的输出流
    std::ofstream m_raftStateOutStream;
    // Snapshot状态的输出流
    std::ofstream m_snapshotOutStream;
    // 保存raft状态的大小（包含日志大小)，避免每次重新读文件来获取具体大小
    long long m_raftStateSize;

public:
    void Save(const std::string& raftState, const std::string& snapshot);
    std::string ReadSnapshot();
    void SaveRaftState(const std::string& data);
    long long RaftStateSize();
    std::string ReadRaftState();
    explicit Persister(int me);
    ~Persister();

private:
    void clearRaftState();
    void clearSnapshot();
    void clearRaftStateAndSnapshot();
};
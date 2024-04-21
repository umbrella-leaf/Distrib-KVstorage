#include "Persister.h"
#include "util.h"
#include <fstream>
#include <ios>
#include <mutex>
#include <string>

void Persister::Save(const std::string& raftState, const std::string& snapshot) {
    std::lock_guard<std::mutex> lg(m_mutex);
    clearRaftStateAndSnapshot();
    m_raftStateOutStream << raftState;
    m_snapshotOutStream << snapshot;
}

std::string Persister::ReadSnapshot() {
    std::lock_guard<std::mutex> lg(m_mutex);
    // 如果写文件流还在打开状态，必须先关闭，不然可能造成问题
    if (m_snapshotOutStream.is_open()) {
        m_snapshotOutStream.close();
    }
    // 在解锁前以写追加模式重新打开snapshot文件
    DEFER {
        m_snapshotOutStream.open(m_snapshotFileName);
    };
    std::fstream ifs(m_snapshotFileName, std::ios_base::in);
    if (!ifs.good()) {
        return "";
    }
    std::string snapshot;
    ifs >> snapshot;
    ifs.close();
    return snapshot;
}

void Persister::SaveRaftState(const std::string& data) {
    std::lock_guard<std::mutex> lg(m_mutex);
    clearRaftState();
    m_raftStateOutStream << data;
    m_raftStateSize += data.size();
}

long long Persister::RaftStateSize() {
    std::lock_guard<std::mutex> lg(m_mutex);
    return m_raftStateSize;
}

std::string Persister::ReadRaftState() {
    std::lock_guard<std::mutex> lg(m_mutex);

    std::fstream ifs(m_raftStateFileName, std::ios_base::in);
    if (!ifs.good()) {
        return "";
    }
    std::string raftState;
    ifs >> raftState;
    ifs.close();
    return raftState;
}

Persister::Persister(const int me)
    : m_raftStateFileName("raftstatePersist" + std::to_string(me) + ".txt"),
      m_snapshotFileName("snapshotPersist" + std::to_string(me) + ".txt"),
      m_raftStateSize(0) {
    // 检查文件状态并清空文件
    bool fileOpenFlag = true;
    std::fstream file(m_raftStateFileName, std::ios::out | std::ios::trunc);
    if (file.is_open()) {
        file.close();
    } else {
        fileOpenFlag = false;
    }

    file = std::fstream(m_snapshotFileName, std::ios::out | std::ios::trunc);
    if (file.is_open()) {
        file.close();
    } else {
        fileOpenFlag = false;
    }
    if (!fileOpenFlag) {
        DPrintf("[func-Persister::Persister] file open error");
    }
    // 绑定raft状态和快照的输出流
    m_raftStateOutStream.open(m_raftStateFileName);
    m_snapshotOutStream.open(m_snapshotFileName);
}

Persister::~Persister() {
    if (m_raftStateOutStream.is_open()) {
        m_raftStateOutStream.close();
    }
    if (m_snapshotOutStream.is_open()) {
        m_snapshotOutStream.close();
    }
}

void Persister::clearRaftState() {
    m_raftStateSize = 0;
    // 关闭文件流
    if (m_raftStateOutStream.is_open()) {
        m_raftStateOutStream.close();
    }
    // 重新打开文件并清空全部内容(ios::trunc)
    m_raftStateOutStream.open(m_raftStateFileName, std::ios::out | std::ios::trunc);
}

void Persister::clearSnapshot() {
    // 关闭文件流
    if (m_snapshotOutStream.is_open()) {
        m_snapshotOutStream.close();
    }
    // 重新打开文件并清空全部内容(ios::trunc)
    m_snapshotOutStream.open(m_snapshotFileName, std::ios::out | std::ios::trunc);
}

void Persister::clearRaftStateAndSnapshot() {
    clearRaftState();
    clearSnapshot();
}
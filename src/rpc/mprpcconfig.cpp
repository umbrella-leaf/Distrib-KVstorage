#include "mprpcconfig.h"
#include <iostream>
#include <string>

// 负责加载并解析配置文件
void MprpcConfig::LoadConfigFile(const char *config_file) {
    FILE *pf = fopen(config_file, "r");
    if (pf == nullptr) {
        std::cout << config_file << " is not existed!" << std::endl;
        exit(EXIT_FAILURE);
    }
    // 逐行读取每个配置项
    while (!feof(pf)) {
        char buf[512]{0};
        // 每次获取一行
        fgets(buf, 512, pf);

        // 去掉字符串前面多余的空格
        std::string read_buf(buf);
        Trim(read_buf);

        // 判断注释
        if (read_buf[0] == '#' || read_buf.empty()) {
            continue;
        }
        // 解析配置项
        int idx = read_buf.find('=');
        if (idx == -1) {
            continue;
        }

        std::string key;
        std::string value;
        key = read_buf.substr(0, idx);
        Trim(key);

        int endidx = read_buf.find('\n', idx);
        value = read_buf.substr(idx + 1, endidx - idx - 1);
        Trim(value);

        m_configMap.emplace(key, value);
    }
    fclose(pf);
}

// 查询配置项信息
std::string MprpcConfig::Load(const std::string &key) {
    auto it = m_configMap.find(key);
    if (it == m_configMap.end()) {
        return "";
    }
    return it->second;
}

// 去掉字符串前后空格
void MprpcConfig::Trim(std::string &src_buf) {
    int idx = src_buf.find_first_not_of(' ');
    if (idx != -1) {
        // 去掉字符串前置空格
        src_buf = src_buf.substr(idx, src_buf.size() - idx);
    }
    // 去掉字符串后置空格
    idx = src_buf.find_last_not_of(' ');
    if (idx != -1) {
        src_buf = src_buf.substr(0, idx + 1);
    }
}
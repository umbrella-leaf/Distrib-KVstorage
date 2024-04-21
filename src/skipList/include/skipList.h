#pragma once

#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/access.hpp>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#define STORE_FILE "store/dumpFile"

static std::string delimiter = ":";

template<typename K, typename V>
class Node {
public:
    Node() {}
    Node(K& key, V& value, int level);
    ~Node();

    const K& get_key() const;
    const V& get_value() const;

    void set_value(V&);

    Node<K, V> **forward;
    int node_level;

private:
    K key;
    V value;
};

template<typename K, typename  V>
Node<K, V>::Node(K& k, V& v, int level) {
    key = k;
    value = v;
    node_level = level;

    forward = new Node<K, V> *[level + 1];
    std::memset(forward, 0, sizeof(Node<K, V> *) * (level + 1));
};

template<typename K, typename  V>
Node<K, V>::~Node() {
    delete[] forward;
}

template<typename K, typename  V>
const K& Node<K, V>::get_key() const {
    return key;
}

template<typename K, typename  V>
const V& Node<K, V>::get_value() const {
    return value;
}

template<typename K, typename  V>
void Node<K, V>::set_value(V& value) {
    this->value = value;
}

// 跳表持久化的实现类
template<typename K, typename  V>
class SkipListDump {
public:
    friend class boost::serialization::access;

    template<class Archive>
    void serialize(Archive &ar, const unsigned int version) {
        ar &keyDumpVt_;
        ar &valDumpVt_;
    }
    std::vector<K> keyDumpVt_;
    std::vector<V> valDumpVt_;

public:
    void insert(const Node<K, V> &node);
};

// 真正的跳表实现类
template<typename K, typename V>
class SkipList {
public:
    SkipList(int);
    ~SkipList();
    int get_random_level();
    Node<K, V> *create_node(K& k, V& v, int level);
    int insert_element(K& k, V& v);
    void display_list();
    bool search_element(K& k, V& v);
    void delete_element(K& k);
    void insert_set_element(K& k, V& v);
    std::string dump_file();
    void load_file(const std::string& dumpStr);
    // 递归删除节点
    void clear(Node<K, V> *);
    int size();

private:
    void get_key_value_from_string(const std::string& str, std::string *key, std::string *value);
    bool is_valid_string(const std::string &str);

private:
    int _max_level;
    int _skip_list_level;
    // 跳表头节点
    Node<K, V> *_header;

    // 跳表节点总数
    int _element_count;
    std::mutex _mtx;
};

template<typename K, typename  V>
Node<K, V> *SkipList<K, V>::create_node(K &k, V &v, int level) {
    Node<K, V> *n = new Node<K, V>(k, v, level);
    return n;
}

// Insert given key and value in skip list
// return 1 means element exists
// return 0 means insert successfully
/*
                           +------------+
                           |  insert 50 |
                           +------------+
level 4     +-->1+                                                      100
                 |
                 |                      insert +----+
level 3         1+-------->10+---------------> | 50 |          70       100
                                               |    |
                                               |    |
level 2         1          10         30       | 50 |          70       100
                                               |    |
                                               |    |
level 1         1    4     10         30       | 50 |          70       100
                                               |    |
                                               |    |
level 0         1    4   9 10         30   40  | 50 |  60      70       100
                                               +----+

*/
template<typename K, typename V>
int SkipList<K, V>::insert_element(K &key, V &value) {
    _mtx.lock();
    Node<K, V> *current = _header;

    Node<K, V> *update[_max_level + 1];
    memset(update, 0, sizeof(Node<K, V> *) * (_max_level + 1));

    for (int i = _skip_list_level; i >= 0; --i) {
        while (current->forward[i] != nullptr && current->forward[i]->get_key() < key) {
            current = current->forward[i];
        }
        update[i] = current;
    }
    // 找到最低一层节点的目标插入位置
    current = current->forward[0];
    // 键已经存在，则插入失败
    if (current != nullptr && current->get_key() == key) {
        std::cout << "key: " << key << ", exists" << std::endl;
        _mtx.unlock();
        return 1;
    }

    int random_level = get_random_level();
    // 随机生成的插入层数如果大于原本层数，就在上层的header节点后插入
    if (random_level > _skip_list_level) {
        for (int i = _skip_list_level + 1; i <= random_level; ++i) {
            update[i] = _header;
        }
        _skip_list_level = random_level;
    }

    // 插入节点，在每一层的update后面插入该节点，实际整个跳表里只有一份
    Node<K, V> *inserted_node = create_node(key, value, random_level);
    for (int i = 0; i <= random_level; ++i) {
        inserted_node->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = inserted_node;
    }
    std::cout << "Successfully inserted key:" << key << ", value:" << value << std::endl;
    _element_count++;

    _mtx.unlock();
    return 0;
}

template<typename K, typename V>
void SkipList<K, V>::display_list() {
    std::cout << "\n*****Skip List*****"
                << "\n";
    for (int i = 0; i <= _skip_list_level; i++) {
        Node<K, V> *node = _header->forward[i];
        std::cout << "Level " << i << ": ";
        while (node != nullptr) {
            std::cout << node->get_key() << ":" << node->get_value() << ";";
            node = node->forward[i];
        }
        std::cout << std::endl;
    }
}

template<typename K, typename V>
std::string SkipList<K, V>::dump_file() {
    Node<K, V> *node = _header->forward[0];
    // 从最底层的第一个节点开始，因为所有实际节点都存在于最底层
    SkipListDump<K, V> dumper;
    while (node != nullptr) {
        dumper.insert(*node);
        node = node->forward[0];
    }
    std::stringstream ss;
    boost::archive::text_oarchive oa(ss);
    oa << dumper;
    return ss.str();
}

template<typename K, typename V>
void SkipList<K, V>::load_file(const std::string &dumpStr) {
    if (dumpStr.empty()) {
        return;
    }
    SkipListDump<K, V> dumper;
    std::stringstream iss(dumpStr);
    boost::archive::text_iarchive ia(iss);
    ia >> dumper;
    for (int i = 0; i < dumper.keyDumpVt_.size(); ++i) {
        insert_element(dumper.keyDumpVt_[i], dumper.valDumpVt_[i]);
    }
}

template<typename K, typename V>
int SkipList<K, V>::size() {
    return _element_count;
}

template<typename K, typename V>
void SkipList<K, V>::get_key_value_from_string(const std::string &str, std::string *key, std::string *value) {
    if (!is_valid_string(str)) {
        return;
    }
    int pos = str.find(delimiter);
    *key = str.substr(0, pos);
    *value = str.substr(pos + 1);
}

template<typename K, typename V>
bool SkipList<K, V>::is_valid_string(const std::string &str) {
    if (str.empty()) {
        return false;
    }
    if (str.find(delimiter) == std::string::npos) {
        return false;
    }
    return true;
}

template<typename K, typename V>
void SkipList<K, V>::delete_element(K &key) {
    _mtx.lock();
    Node<K, V> *current = _header;
    Node<K, V> *update[_max_level + 1];
    memset(update, 0, sizeof(Node<K, V> *) * (_max_level + 1));

    for (int i = _skip_list_level; i >= 0; --i) {
        while (current->forward[i] != nullptr && current->forward[i]->get_key() < key) {
            current = current->forward[i];
        }
        update[i] = current;
    }

    current = current->forward[0];
    if (current != nullptr && current->get_key() == key) {
        // 每一个实际节点存在于0层到某一层
        for (int i = 0; i <= _skip_list_level; ++i) {
            if (update[i]->forward[i] != current) break;
            update[i]->forward[i] = current->forward[i];
        }
        // 从上到下，如果某一层头结点连接的是空节点，那么删除这一层
        while (_skip_list_level > 0 && _header->forward[_skip_list_level] == 0) {
            --_skip_list_level;
        }
        std::cout << "Successfully deleted key " << key << std::endl;
        delete current;
        _element_count--;
    }
    _mtx.unlock();
}

template<typename K, typename V>
void SkipList<K, V>::insert_set_element(K &key, V &value) {
    V oldValue;
    if (search_element(key, oldValue)) {
        delete_element(key);
    }
    insert_element(key, value);
}

// Search for element in skip list
/*
                           +------------+
                           |  select 60 |
                           +------------+
level 4     +-->1+                                                      100
                 |
                 |
level 3         1+-------->10+------------------>50+           70       100
                                                   |
                                                   |
level 2         1          10         30         50|           70       100
                                                   |
                                                   |
level 1         1    4     10         30         50|           70       100
                                                   |
                                                   |
level 0         1    4   9 10         30   40    50+-->60      70       100
*/
template<typename K, typename V>
bool SkipList<K, V>::search_element(K &key, V &value) {
    std::cout << "search_element-----------------" << std::endl;
    Node<K, V> *current = _header;

    for (int i = _skip_list_level; i >= 0; --i) {
        while (current->forward[i] && current->forward[i]->get_key() < key) {
            current = current->forward[i];
        }
    }

    current = current->forward[0];

    if (current && current->get_key() == key) {
        value = current->get_value();
        std::cout << "Found key: " << key << ", value: " << current->get_value() << std::endl;
        return true;
    }

    std::cout << "Not Found Key:" << key << std::endl;
    return false;
}

template <typename K, typename V>
void SkipListDump<K, V>::insert(const Node<K, V> &node) {
  keyDumpVt_.emplace_back(node.get_key());
  valDumpVt_.emplace_back(node.get_value());
}

template<typename K, typename V>
SkipList<K, V>::SkipList(int max_level) {
    _max_level = max_level;
    _skip_list_level = 0;
    _element_count = 0;

    K k;
    V v;
    _header = new Node<K, V>(k, v, _max_level);
};

template<typename K, typename V>
SkipList<K, V>::~SkipList() {
    // 递归删除最底层真实节点
    if (_header->forward[0] != nullptr) {
        clear(_header->forward[0]);
    }
    delete _header;
}

template<typename K, typename V>
void SkipList<K, V>::clear(Node<K, V> *cur) {
    if (cur->forward[0] != nullptr) {
        clear(cur->forward[0]);
    }
    delete cur;
}

template<typename K, typename V>
int SkipList<K, V>::get_random_level() {
    int k = 1;
    while (rand() % 2) {
        ++k;
    }
    k = (k < _max_level) ? k : _max_level;
    // k实际是随机生成得到偶数的次数
    return k;
}
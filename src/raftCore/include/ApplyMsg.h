#pragma once

#include <string>
class ApplyMsg {
public:
    bool CommandValid;
    std::string Command;
    int CommandIndex;
    bool SnapShotValid;
    std::string Snapshot;
    int SnapshotTerm;
    int SnapshotIndex;

public:
    ApplyMsg() 
        : CommandValid(false),
          Command(),
          CommandIndex(-1),
          SnapShotValid(false),
          SnapshotTerm(-1),
          SnapshotIndex(-1) {

    }
};
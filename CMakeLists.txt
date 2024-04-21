# 设置cmake的最低版本和项目名称
cmake_minimum_required(VERSION 3.22)
project(KVRaftCpp)

# 设置cpp版本
set(CMAKE_CXX_STANDARD 20)
# 生成debug版本可供gdb调试，相当于-g参数
set(CMAKE_BUILD_TYPE "Debug")
# 生成compile_commands.json提供给IDE已解决头文件路径问题
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 设置项目可执行文件输出路径
# PROJECT_SOURCE_DIR是最近的带有project()的CMakeLists.txt所在的目录
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
# 设置项目库文件输出路径
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)

# 设置项目编译头文件搜索路径，相当于参数 -I
include_directories(${PROJECT_SOURCE_DIR}/src/common/include)
include_directories(${PROJECT_SOURCE_DIR}/src/fiber/include)
include_directories(${PROJECT_SOURCE_DIR}/src/rpc/include)
include_directories(${PROJECT_SOURCE_DIR}/example)
include_directories(${PROJECT_SOURCE_DIR}/src/raftCore/include)
include_directories(${PROJECT_SOURCE_DIR}/src/raftRpcPro/include)
include_directories(${PROJECT_SOURCE_DIR}/src/raftClerk/include)
include_directories(${PROJECT_SOURCE_DIR}/src/skipList/include)

# 设置项目库文件搜索路，参数-L
link_directories(${PROJECT_SOURCE_DIR}/lib)

# src包含所有相关代码，项目结构为根->src->下面各个模块
add_subdirectory(src)
# example 包含了全部的示例代码
add_subdirectory(example)

add_library(skip_list_on_raft STATIC ${src_rpc} ${src_fiber} ${rpc_example} ${raftsource} ${src_raftCore} ${src_raftRpcPro})
target_link_libraries(skip_list_on_raft muduo_net muduo_base pthread dl)





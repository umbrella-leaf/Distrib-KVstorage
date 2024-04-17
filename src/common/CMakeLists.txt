# 获取当前源文件所在目录绝对路径
get_filename_component(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)

# 将该目录下全部源文件添加到SRC_LIST变量中
aux_source_directory(${SRC_DIR} SRC_LIST)

# 设置src_common为全局变量，并且是内部变量，无法在CMAKE GUI中查看到
# CACHE作用是缓存变量，使其超出作用域(当前目录)时仍有效，INTERNAL则是内部变量
set(src_common ${SRC_LIST} CACHE INTERNAL "src files list for common folder")
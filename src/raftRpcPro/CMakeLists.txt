get_filename_component(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)

aux_source_directory(${SRC_DIR} SRC_LIST)

set(src_raftRpcPro ${SRC_LIST} CACHE INTERNAL "src file list for raftRpcPro folder")
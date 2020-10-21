cmake_minimum_required(VERSION 3.16)
project(web)

set(CMAKE_CXX_STANDARD 11)
aux_source_directory(. SRC_LIST)
add_executable(web ${SRC_LIST} http/http_conn.cpp http/http_conn.h http/threadpool.h
        tools/locker.h tools/lst_timer.h tools/log.h tools/log.cpp tools/block_queue.h
        mysql_cgi/sql_connection_pool.h mysql_cgi/sql_connection_pool.cpp )
target_link_libraries(web -pthread -lmysqlclient )
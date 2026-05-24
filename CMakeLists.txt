cmake_minimum_required(VERSION 3.31)
project(osvcp C)

set(CMAKE_C_STANDARD 11)

add_executable(osvcp main.c
        concurrency/locked-data-structures/2/cloppy_counter_only_global_lock.c
        exersises/concurrency/event-based-concurrency/exercise/tcp-time-server.c
        exersises/concurrency/event-based-concurrency/exercise/aio-file-server.c)

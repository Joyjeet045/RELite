cmake_minimum_required(VERSION 3.16)
project(Relite VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ---------------------------------------------------------------------------
# Core library: all engine code lives here so both the REPL executable and the
# test targets can link against it.
# ---------------------------------------------------------------------------
add_library(relite_core
    src/frontend/lexer.cpp
    src/frontend/ast.cpp
    src/frontend/parser.cpp
    src/frontend/catalog.cpp
    src/frontend/semantic_analyzer.cpp
    src/frontend/db.cpp
    src/vm/value.cpp
    src/vm/tuple.cpp
    src/vm/table_manager.cpp
    src/vm/version_store.cpp
    src/vm/vectorized.cpp
    src/vm/expression_eval.cpp
    src/vm/executor.cpp
    src/vm/executor_engine.cpp
    src/index/bloom_filter.cpp
    src/index/bplus_tree.cpp
    src/index/disk_bplus_tree.cpp
    src/index/index_manager.cpp
    src/txn/lock_manager.cpp
    src/txn/wal.cpp
    src/txn/transaction_manager.cpp
    src/backend/page.cpp
    src/backend/disk_manager.cpp
    src/backend/buffer_pool.cpp
    src/backend/durability.cpp
)
target_include_directories(relite_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# REPL executable.
# ---------------------------------------------------------------------------
add_executable(relite main.cpp)
target_link_libraries(relite PRIVATE relite_core)

# ---------------------------------------------------------------------------
# Benchmark executable (opt-in, not a CTest test).
# ---------------------------------------------------------------------------
add_executable(relite_bench tools/benchmark.cpp)
target_link_libraries(relite_bench PRIVATE relite_core)

# ---------------------------------------------------------------------------
# Tests (self-contained, no external framework required).
# ---------------------------------------------------------------------------
enable_testing()

add_executable(lexer_test tests/lexer_test.cpp)
target_link_libraries(lexer_test PRIVATE relite_core)
add_test(NAME lexer_test COMMAND lexer_test)

add_executable(parser_test tests/parser_test.cpp)
target_link_libraries(parser_test PRIVATE relite_core)
add_test(NAME parser_test COMMAND parser_test)

add_executable(semantic_test tests/semantic_test.cpp)
target_link_libraries(semantic_test PRIVATE relite_core)
add_test(NAME semantic_test COMMAND semantic_test)

add_executable(storage_test tests/storage_test.cpp)
target_link_libraries(storage_test PRIVATE relite_core)
add_test(NAME storage_test COMMAND storage_test)

add_executable(table_manager_test tests/table_manager_test.cpp)
target_link_libraries(table_manager_test PRIVATE relite_core)
add_test(NAME table_manager_test COMMAND table_manager_test)

add_executable(engine_test tests/engine_test.cpp)
target_link_libraries(engine_test PRIVATE relite_core)
add_test(NAME engine_test COMMAND engine_test)

add_executable(index_test tests/index_test.cpp)
target_link_libraries(index_test PRIVATE relite_core)
add_test(NAME index_test COMMAND index_test)

add_executable(txn_test tests/txn_test.cpp)
target_link_libraries(txn_test PRIVATE relite_core)
add_test(NAME txn_test COMMAND txn_test)

add_executable(phase5_test tests/phase5_test.cpp)
target_link_libraries(phase5_test PRIVATE relite_core)
add_test(NAME phase5_test COMMAND phase5_test)

add_executable(phase6_test tests/phase6_test.cpp)
target_link_libraries(phase6_test PRIVATE relite_core)
add_test(NAME phase6_test COMMAND phase6_test)

add_executable(features_test tests/features_test.cpp)
target_link_libraries(features_test PRIVATE relite_core)
add_test(NAME features_test COMMAND features_test)

add_executable(vectorized_test tests/vectorized_test.cpp)
target_link_libraries(vectorized_test PRIVATE relite_core)
add_test(NAME vectorized_test COMMAND vectorized_test)

add_executable(disk_index_test tests/disk_index_test.cpp)
target_link_libraries(disk_index_test PRIVATE relite_core)
add_test(NAME disk_index_test COMMAND disk_index_test)

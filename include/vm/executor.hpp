#pragma once

#include <memory>
#include <vector>

#include "frontend/ast.hpp"
#include "vm/record_id.hpp"
#include "vm/table_manager.hpp"
#include "vm/tuple.hpp"

namespace db::vm {

class AbstractExecutor {
public:
    virtual ~AbstractExecutor() = default;
    virtual void init() = 0;
    virtual bool next(Tuple& outTuple, RecordID& outRid) = 0;
};

class SeqScanExecutor : public AbstractExecutor {
public:
    SeqScanExecutor(TableManager* tables, int tableId, Schema schema);
    void init() override;
    bool next(Tuple& outTuple, RecordID& outRid) override;

private:
    TableManager* tables_;
    int tableId_;
    Schema schema_;
    std::unique_ptr<TableIterator> it_;
};

class FilterExecutor : public AbstractExecutor {
public:
    FilterExecutor(std::unique_ptr<AbstractExecutor> child,
                   const parser::Expression* predicate);
    void init() override;
    bool next(Tuple& outTuple, RecordID& outRid) override;

private:
    std::unique_ptr<AbstractExecutor> child_;
    const parser::Expression* predicate_;
};

class ProjectionExecutor : public AbstractExecutor {
public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> child,
                       std::vector<int> columnIndices);
    void init() override;
    bool next(Tuple& outTuple, RecordID& outRid) override;

private:
    std::unique_ptr<AbstractExecutor> child_;
    std::vector<int> columns_;
};

}

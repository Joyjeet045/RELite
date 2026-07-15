#include "vm/executor.hpp"

#include "vm/expression_eval.hpp"

namespace db::vm {

SeqScanExecutor::SeqScanExecutor(TableManager* tables, int tableId, Schema schema)
    : tables_(tables), tableId_(tableId), schema_(std::move(schema)) {}

void SeqScanExecutor::init() {
    it_ = std::make_unique<TableIterator>(tables_, tableId_);
}

bool SeqScanExecutor::next(Tuple& outTuple, RecordID& outRid) {
    if (!it_ || !it_->valid()) {
        return false;
    }
    outTuple = Tuple::deserialize(it_->bytes(), schema_);
    outRid = it_->rid();
    it_->next();
    return true;
}

FilterExecutor::FilterExecutor(std::unique_ptr<AbstractExecutor> child,
                               const parser::Expression* predicate)
    : child_(std::move(child)), predicate_(predicate) {}

void FilterExecutor::init() { child_->init(); }

bool FilterExecutor::next(Tuple& outTuple, RecordID& outRid) {
    Tuple t;
    RecordID rid;
    while (child_->next(t, rid)) {
        if (predicate_ == nullptr || predicateTrue(*predicate_, t)) {
            outTuple = std::move(t);
            outRid = rid;
            return true;
        }
    }
    return false;
}

ProjectionExecutor::ProjectionExecutor(std::unique_ptr<AbstractExecutor> child,
                                       std::vector<int> columnIndices)
    : child_(std::move(child)), columns_(std::move(columnIndices)) {}

void ProjectionExecutor::init() { child_->init(); }

bool ProjectionExecutor::next(Tuple& outTuple, RecordID& outRid) {
    Tuple t;
    RecordID rid;
    if (!child_->next(t, rid)) {
        return false;
    }
    std::vector<Value> projected;
    projected.reserve(columns_.size());
    for (int idx : columns_) {
        if (idx >= 0 && idx < static_cast<int>(t.size())) {
            projected.push_back(t.at(idx));
        } else {
            projected.push_back(Value::null());
        }
    }
    outTuple = Tuple(std::move(projected));
    outRid = rid;
    return true;
}

}

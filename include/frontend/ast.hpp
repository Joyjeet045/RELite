#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace db::parser {

class ASTVisitor;
class SelectStatement;
class SubqueryExpr;

enum class DataType {
    Int,
    Bool,
    Text,
    Varchar,
    Float,
    Date,
    Timestamp,
};

std::string_view dataTypeName(DataType type);
enum class ComparisonOp {
    Eq,
    Neq,
    Lt,
    Leq,
    Gt,
    Geq,
};

std::string_view comparisonOpName(ComparisonOp op);

std::string expressionToString(const class Expression& e);

enum class LogicalOp {
    And,
    Or,
};

enum class ArithmeticOp {
    Add,
    Sub,
    Mul,
    Div,
};

struct CachedValue {
    enum class Kind { Null, Int, Bool, Text, Float };
    Kind kind = Kind::Null;
    std::int64_t intValue = 0;
    bool boolValue = false;
    double doubleValue = 0.0;
    std::string stringValue;
};

class ASTNode {
public:
    virtual ~ASTNode() = default;
    virtual void accept(ASTVisitor& visitor) = 0;
};

using ASTNodePtr = std::unique_ptr<ASTNode>;

class Expression : public ASTNode {
public:
    std::optional<DataType> resolvedType;
};

using ExpressionPtr = std::unique_ptr<Expression>;

class LiteralExpr : public Expression {
public:
    enum class Kind { Integer, String, Boolean, Null, Float };

    Kind kind = Kind::Integer;
    std::int64_t intValue = 0;
    double doubleValue = 0.0;
    std::string stringValue;
    bool boolValue = false;

    void accept(ASTVisitor& visitor) override;
};

class ColumnRef : public Expression {
public:
    std::string table;
    std::string column;
    std::string alias;

    ExpressionPtr computed;

    int columnIndex = -1;

    bool outerRef = false;
    mutable bool bound = false;
    mutable CachedValue boundValue;

    void accept(ASTVisitor& visitor) override;
};

class BinaryExpr : public Expression {
public:
    ComparisonOp op = ComparisonOp::Eq;
    ExpressionPtr left;
    ExpressionPtr right;

    void accept(ASTVisitor& visitor) override;
};

class LogicalExpr : public Expression {
public:
    LogicalOp op = LogicalOp::And;
    ExpressionPtr left;
    ExpressionPtr right;

    void accept(ASTVisitor& visitor) override;
};

class ArithmeticExpr : public Expression {
public:
    ArithmeticOp op = ArithmeticOp::Add;
    ExpressionPtr left;
    ExpressionPtr right;

    void accept(ASTVisitor& visitor) override;
};

class UnaryExpr : public Expression {
public:
    ExpressionPtr operand;

    void accept(ASTVisitor& visitor) override;
};

class IsNullExpr : public Expression {
public:
    ExpressionPtr operand;
    bool negated = false;

    void accept(ASTVisitor& visitor) override;
};

class InExpr : public Expression {
public:
    ExpressionPtr value;
    std::vector<ExpressionPtr> items;
    std::unique_ptr<SubqueryExpr> subquery;
    bool negated = false;

    void accept(ASTVisitor& visitor) override;
};

class BetweenExpr : public Expression {
public:
    ExpressionPtr value;
    ExpressionPtr lo;
    ExpressionPtr hi;
    bool negated = false;

    void accept(ASTVisitor& visitor) override;
};

class LikeExpr : public Expression {
public:
    ExpressionPtr value;
    ExpressionPtr pattern;
    bool negated = false;

    void accept(ASTVisitor& visitor) override;
};

class FunctionExpr : public Expression {
public:
    std::string name;
    bool star = false;
    bool distinct = false;
    std::unique_ptr<ColumnRef> argument;
    std::string alias;

    void accept(ASTVisitor& visitor) override;
};

class CallExpr : public Expression {
public:
    std::string name;
    std::vector<ExpressionPtr> args;
    DataType castType = DataType::Int;
    bool isCast = false;

    void accept(ASTVisitor& visitor) override;
};

class CaseExpr : public Expression {
public:
    struct Branch {
        ExpressionPtr when;
        ExpressionPtr then;
    };
    std::vector<Branch> branches;
    ExpressionPtr elseExpr;

    void accept(ASTVisitor& visitor) override;
};

class WindowExpr : public Expression {
public:
    std::string name;
    std::unique_ptr<ColumnRef> argument;
    std::vector<std::unique_ptr<ColumnRef>> partitionBy;
    struct OrderKey {
        std::unique_ptr<ColumnRef> column;
        bool ascending = true;
    };
    std::vector<OrderKey> orderBy;

    void accept(ASTVisitor& visitor) override;
};

class SubqueryExpr : public Expression {
public:
    enum class Kind { Scalar, Exists };
    Kind kind = Kind::Scalar;
    std::unique_ptr<SelectStatement> query;

    bool correlated = false;
    std::vector<ColumnRef*> outerRefs;

    mutable bool evaluated = false;
    mutable std::vector<CachedValue> results;

    void accept(ASTVisitor& visitor) override;
};

struct ColumnDefinition {
    std::string name;
    DataType type = DataType::Int;
    int varcharLength = 0;
    std::string refTable;
    std::string refColumn;
    int refOnDelete = 0;
    int refOnUpdate = 0;

    bool notNull = false;
    bool primaryKey = false;
    bool unique = false;
    bool autoIncrement = false;
    bool hasDefault = false;
    CachedValue defaultValue;
    std::shared_ptr<Expression> checkExpr;
};

class CreateStatement : public ASTNode {
public:
    std::string table;
    std::vector<ColumnDefinition> columns;
    std::shared_ptr<SelectStatement> asQuery;

    int tableId = -1;

    void accept(ASTVisitor& visitor) override;
};

class CreateIdxStatement : public ASTNode {
public:
    std::string indexName;
    std::string table;
    std::vector<std::string> columns;

    int tableId = -1;
    std::vector<int> columnIndices;

    void accept(ASTVisitor& visitor) override;
};

class InsertStatement : public ASTNode {
public:
    std::string table;
    std::vector<std::string> columns;
    std::vector<std::vector<ExpressionPtr>> rows;
    std::unique_ptr<SelectStatement> select;
    bool defaultValues = false;

    bool returningStar = false;
    std::vector<std::unique_ptr<ColumnRef>> returning;

    bool hasOnConflict = false;
    bool conflictDoNothing = false;
    std::vector<std::unique_ptr<ColumnRef>> conflictColumns;
    std::vector<std::string> conflictSetColumns;
    std::vector<ExpressionPtr> conflictSetValues;

    int tableId = -1;

    void accept(ASTVisitor& visitor) override;
};

class SelectStatement : public ASTNode {
public:
    bool selectStar = false;
    bool distinct = false;
    bool explain = false;
    std::vector<std::unique_ptr<ColumnRef>> columns;
    std::string table;
    std::string tableAlias;
    bool asOf = false;
    unsigned long long asOfVersion = 0;
    ExpressionPtr where;

    struct OrderKey {
        std::unique_ptr<ColumnRef> column;
        bool ascending = true;
    };
    std::vector<OrderKey> orderBy;

    bool hasLimit = false;
    long long limit = 0;
    long long offset = 0;

    std::vector<std::unique_ptr<FunctionExpr>> aggregates;
    std::vector<std::unique_ptr<ColumnRef>> groupBy;
    ExpressionPtr having;

    enum class JoinKind { Inner, Left, Right, Full, Cross };
    std::string joinTable;
    std::string joinTableAlias;
    ExpressionPtr joinOn;
    int joinTableId = -1;
    JoinKind joinType = JoinKind::Inner;

    struct JoinClause {
        JoinKind kind = JoinKind::Inner;
        std::string table;
        std::string alias;
        ExpressionPtr on;
        int tableId = -1;
    };
    std::vector<JoinClause> extraJoins;

    int tableId = -1;

    void accept(ASTVisitor& visitor) override;
};

class DeleteStatement : public ASTNode {
public:
    std::string table;
    ExpressionPtr where;

    bool returningStar = false;
    std::vector<std::unique_ptr<ColumnRef>> returning;

    int tableId = -1;

    void accept(ASTVisitor& visitor) override;
};

class UpdateStatement : public ASTNode {
public:
    std::string table;
    std::vector<std::string> targetColumns;
    std::vector<ExpressionPtr> values;
    ExpressionPtr where;

    bool returningStar = false;
    std::vector<std::unique_ptr<ColumnRef>> returning;

    int tableId = -1;
    std::vector<int> targetIndices;

    void accept(ASTVisitor& visitor) override;
};

class DropStatement : public ASTNode {
public:
    bool isIndex = false;
    bool isView = false;
    bool truncate = false;
    std::string name;

    int tableId = -1;

    void accept(ASTVisitor& visitor) override;
};

class AlterStatement : public ASTNode {
public:
    enum class Kind { AddColumn, DropColumn };
    Kind kind = Kind::AddColumn;
    std::string table;
    ColumnDefinition column;
    std::string dropColumn;

    int tableId = -1;

    void accept(ASTVisitor& visitor) override;
};

class TransactionStatement : public ASTNode {
public:
    enum class Kind { Begin, Commit, Rollback };
    Kind kind = Kind::Begin;

    void accept(ASTVisitor& visitor) override;
};

class SetOpStatement : public ASTNode {
public:
    enum class Op { Union, Intersect, Except };
    Op op = Op::Union;
    bool all = false;
    ASTNodePtr left;
    std::unique_ptr<SelectStatement> right;

    void accept(ASTVisitor& visitor) override;
};

class CreateViewStatement : public ASTNode {
public:
    std::string name;
    std::shared_ptr<SelectStatement> query;
    std::string source;

    int tableId = -1;

    void accept(ASTVisitor& visitor) override;
};

class ASTVisitor {
public:
    virtual ~ASTVisitor() = default;

    virtual void visit(LiteralExpr& node) = 0;
    virtual void visit(ColumnRef& node) = 0;
    virtual void visit(BinaryExpr& node) = 0;
    virtual void visit(ArithmeticExpr& node) = 0;
    virtual void visit(LogicalExpr& node) = 0;
    virtual void visit(UnaryExpr& node) = 0;
    virtual void visit(IsNullExpr& node) = 0;
    virtual void visit(InExpr& node) = 0;
    virtual void visit(BetweenExpr& node) = 0;
    virtual void visit(LikeExpr& node) = 0;
    virtual void visit(FunctionExpr& node) = 0;
    virtual void visit(CallExpr& node) = 0;
    virtual void visit(CaseExpr& node) = 0;
    virtual void visit(WindowExpr& node) = 0;
    virtual void visit(SubqueryExpr& node) = 0;

    virtual void visit(CreateStatement& node) = 0;
    virtual void visit(CreateIdxStatement& node) = 0;
    virtual void visit(InsertStatement& node) = 0;
    virtual void visit(SelectStatement& node) = 0;
    virtual void visit(DeleteStatement& node) = 0;
    virtual void visit(UpdateStatement& node) = 0;
    virtual void visit(DropStatement& node) = 0;
    virtual void visit(AlterStatement& node) = 0;
    virtual void visit(TransactionStatement& node) = 0;
    virtual void visit(SetOpStatement& node) = 0;
    virtual void visit(CreateViewStatement& node) = 0;
};

}

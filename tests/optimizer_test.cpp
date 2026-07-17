#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "frontend/ast.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "vm/optimizer.hpp"
#include "vm/rewrite.hpp"
#include "vm/value.hpp"

using namespace db;

namespace {

vm::Row row2(long long a, const std::string& b) {
    return {vm::Value::makeInt(a), vm::Value::makeText(b)};
}

/*
 * Verifies the external merge sort (including the spill path), the sort-merge
 * join, and the cost model's algorithm selection.
 */
void run() {
    /* In-memory sort. */
    {
        std::vector<vm::Row> rows = {row2(3, "c"), row2(1, "a"), row2(2, "b")};
        vm::externalSort(rows, {0}, {true}, 100);
        assert(rows.size() == 3);
        assert(rows[0][0].intValue == 1 && rows[2][0].intValue == 3);
    }

    /* Spilling sort: memLimit far below input size forces run files + k-way
     * merge; result must be fully, stably sorted. */
    {
        std::vector<vm::Row> rows;
        for (int i = 0; i < 2000; ++i) {
            rows.push_back(row2((i * 7919) % 1000, "x"));
        }
        vm::externalSort(rows, {0}, {true}, /*memLimitRows=*/64);
        assert(rows.size() == 2000);
        for (std::size_t i = 1; i < rows.size(); ++i) {
            assert(rows[i - 1][0].intValue <= rows[i][0].intValue);
        }
    }

    /* Descending multi-key spill sort. */
    {
        std::vector<vm::Row> rows;
        for (int i = 0; i < 500; ++i) rows.push_back(row2(i % 10, std::to_string(i)));
        vm::externalSort(rows, {0}, {false}, 32);
        for (std::size_t i = 1; i < rows.size(); ++i) {
            assert(rows[i - 1][0].intValue >= rows[i][0].intValue);
        }
    }

    /* Sort-merge join with duplicate keys (cross product per key group). */
    {
        std::vector<vm::Row> left = {row2(1, "l1"), row2(2, "l2"), row2(2, "l3"),
                                     row2(4, "l4")};
        std::vector<vm::Row> right = {row2(2, "r1"), row2(2, "r2"), row2(3, "r3"),
                                      row2(4, "r4")};
        auto joined = vm::mergeJoinInner(left, 0, right, 0, /*memLimitRows=*/2);
        /* key 2: 2 left x 2 right = 4; key 4: 1 x 1 = 1; total 5. */
        assert(joined.size() == 5);
        for (const auto& r : joined) {
            assert(r.size() == 4);
            assert(r[0].intValue == r[2].intValue);
        }
    }

    /* Cost model: small side -> hash; both large -> merge. */
    {
        vm::CostModel cm;
        cm.hashBuildBudget = 1000;
        assert(cm.chooseEquiJoin(50, 1000000) == vm::JoinAlgorithm::Hash);
        assert(cm.chooseEquiJoin(500000, 800000) == vm::JoinAlgorithm::Merge);
        assert(cm.estimateCost(vm::JoinAlgorithm::NestedLoop, 1000, 1000) >
               cm.estimateCost(vm::JoinAlgorithm::Hash, 1000, 1000));
    }

    /* Rule optimizer: constant folding + boolean/arithmetic simplification. */
    {
        auto parse = [](const std::string& s) {
            parser::Lexer lex(s);
            parser::Parser p(lex.tokenize());
            return p.parseWholeExpression();
        };

        /* Constant folding: 2 + 3 * 4 -> 14. */
        auto folded = vm::rewriteExpression(parse("2 + 3 * 4"));
        auto* lit = dynamic_cast<parser::LiteralExpr*>(folded.get());
        assert(lit && lit->kind == parser::LiteralExpr::Kind::Integer &&
               lit->intValue == 14);

        /* Comparison folding: 1 = 1 -> TRUE, 2 > 5 -> FALSE. */
        auto t = vm::rewriteExpression(parse("1 = 1"));
        auto* tl = dynamic_cast<parser::LiteralExpr*>(t.get());
        assert(tl && tl->kind == parser::LiteralExpr::Kind::Boolean && tl->boolValue);
        auto f = vm::rewriteExpression(parse("2 > 5"));
        auto* fl = dynamic_cast<parser::LiteralExpr*>(f.get());
        assert(fl && fl->kind == parser::LiteralExpr::Kind::Boolean && !fl->boolValue);

        /* Arithmetic identity: x + 0 -> x. */
        auto id = vm::rewriteExpression(parse("x + 0"));
        auto* cr = dynamic_cast<parser::ColumnRef*>(id.get());
        assert(cr && cr->column == "x");

        /* Boolean simplification: x > 5 AND 1 = 1 -> (x > 5). */
        auto simp = vm::rewriteExpression(parse("x > 5 AND 1 = 1"));
        auto* bin = dynamic_cast<parser::BinaryExpr*>(simp.get());
        assert(bin && bin->op == parser::ComparisonOp::Gt);

        /* Short-circuit: x > 5 AND 1 = 2 -> FALSE. */
        auto sc = vm::rewriteExpression(parse("x > 5 AND 1 = 2"));
        auto* scl = dynamic_cast<parser::LiteralExpr*>(sc.get());
        assert(scl && scl->kind == parser::LiteralExpr::Kind::Boolean &&
               !scl->boolValue);
    }

    std::cout << "optimizer_test passed\n";
}

}  // namespace

int main() {
    run();
    return 0;
}

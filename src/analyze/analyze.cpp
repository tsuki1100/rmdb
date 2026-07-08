/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "analyze.h"

/**
 * @description: 分析器，进行语义分析和查询重写，需要检查不符合语义规定的部分
 * @param {shared_ptr<ast::TreeNode>} parse parser生成的结果集
 * @return {shared_ptr<Query>} Query 
 */
std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse)
{
    std::shared_ptr<Query> query = std::make_shared<Query>();
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse))
    {
        // 处理表名
        query->tables = std::move(x->tabs);
        /** TODO: 检查表是否存在 */

        // 处理聚合函数
        if (x->has_agg && x->agg) {
            query->has_agg = true;
            query->agg_type = x->agg->agg_type;
            query->agg_alias = x->agg->alias.empty() ? "agg_result" : x->agg->alias;

            std::vector<ColMeta> all_cols;
            get_all_cols(query->tables, all_cols);

            if (x->agg->agg_type == AGG_COUNT && x->agg->col_name.empty()) {
                // COUNT(*) — no column needed, but output as INT
                query->agg_col = {.tab_name = "", .col_name = query->agg_alias};
            } else {
                // SUM/MAX/MIN/COUNT(column) — resolve the column
                TabCol col = {.tab_name = "", .col_name = x->agg->col_name};
                col = check_column(all_cols, col);
                query->agg_col = col;
                query->agg_src_col = x->agg->col_name;  // keep original for executor lookup
            }
            // Also put the alias in cols for output caption
            TabCol caption = {.tab_name = "", .col_name = query->agg_alias};
            query->cols.push_back(caption);
        } else {
            // 处理target list，再target list中添加上表名，例如 a.id
            for (auto &sv_sel_col : x->cols) {
                TabCol sel_col = {.tab_name = sv_sel_col->tab_name, .col_name = sv_sel_col->col_name};
                query->cols.push_back(sel_col);
            }

            std::vector<ColMeta> all_cols;
            get_all_cols(query->tables, all_cols);
            if (query->cols.empty()) {
                // select all columns
                for (auto &col : all_cols) {
                    TabCol sel_col = {.tab_name = col.tab_name, .col_name = col.name};
                    query->cols.push_back(sel_col);
                }
            } else {
                // infer table name from column name
                for (auto &sel_col : query->cols) {
                    sel_col = check_column(all_cols, sel_col);  // 列元数据校验
                }
            }
        }
        //处理where条件
        get_clause(x->conds, query->conds);
        check_clause(query->tables, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        // 处理表名
        query->tables = {x->tab_name};
        // 处理 SET 子句
        for (auto &sv_set_clause : x->set_clauses) {
            SetClause set_clause;
            set_clause.lhs.col_name = sv_set_clause->col_name;
            set_clause.lhs.tab_name = x->tab_name;
            set_clause.rhs = convert_sv_value(sv_set_clause->val);
            query->set_clauses.push_back(set_clause);
        }
        // 处理 WHERE 条件
        get_clause(x->conds, query->conds);
        check_clause(query->tables, query->conds);
        // 检查 SET 子句中的列是否存在且类型匹配
        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);
        for (auto &set_clause : query->set_clauses) {
            check_column(all_cols, set_clause.lhs);
            auto lhs_col_meta = sm_manager_->db_.get_table(set_clause.lhs.tab_name).get_col(set_clause.lhs.col_name);
            if (lhs_col_meta->type != set_clause.rhs.type) {
                // 允许 INT/BIGINT/FLOAT/DATETIME 之间的隐式转换
                if (lhs_col_meta->type == TYPE_DATETIME && set_clause.rhs.type == TYPE_STRING) {
                    int64_t dt;
                    if (!parse_datetime(set_clause.rhs.str_val, dt)) {
                        throw InternalError("Invalid datetime value");
                    }
                    set_clause.rhs = Value();
                    set_clause.rhs.set_datetime(dt);
                } else if (lhs_col_meta->type == TYPE_FLOAT && set_clause.rhs.type == TYPE_INT) {
                    int int_val = set_clause.rhs.int_val;
                    set_clause.rhs = Value();
                    set_clause.rhs.set_float((float)int_val);
                } else if (lhs_col_meta->type == TYPE_BIGINT && set_clause.rhs.type == TYPE_INT) {
                    int64_t bigint_val = set_clause.rhs.int_val;
                    set_clause.rhs = Value();
                    set_clause.rhs.set_bigint(bigint_val);
                } else if (lhs_col_meta->type == TYPE_FLOAT && set_clause.rhs.type == TYPE_BIGINT) {
                    int64_t bigint_val = set_clause.rhs.bigint_val;
                    set_clause.rhs = Value();
                    set_clause.rhs.set_float((float)bigint_val);
                } else {
                    throw IncompatibleTypeError(coltype2str(lhs_col_meta->type), coltype2str(set_clause.rhs.type));
                }
            }
            set_clause.rhs.init_raw(lhs_col_meta->len);
        }
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        //处理where条件
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);        
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        // 处理insert 的values值
        for (auto &sv_val : x->vals) {
            query->values.push_back(convert_sv_value(sv_val));
        }
    } else {
        // do nothing
    }
    query->parse = std::move(parse);
    return query;
}


TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
        // Table name not specified, infer table name from column name
        std::string tab_name;
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = tab_name;
    } else {
        /** TODO: Make sure target column exists */
        
    }
    return target;
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    for (auto &sel_tab_name : tab_names) {
        // 这里db_不能写成get_db(), 注意要传指针
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        }
        conds.push_back(cond);
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds) {
    // auto all_cols = get_all_cols(tab_names);
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    // Get raw values in where clause
    for (auto &cond : conds) {
        // Infer table name from column name
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) {
            cond.rhs_col = check_column(all_cols, cond.rhs_col);
        }
        TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType lhs_type = lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        if (lhs_type != rhs_type) {
            // 允许 INT / BIGINT / FLOAT 之间的隐式转换
            if (cond.is_rhs_val && lhs_type == TYPE_FLOAT && rhs_type == TYPE_INT) {
                int int_val = cond.rhs_val.int_val;
                cond.rhs_val = Value();
                cond.rhs_val.set_float((float)int_val);
                cond.rhs_val.init_raw(lhs_col->len);
            } else if (cond.is_rhs_val && lhs_type == TYPE_INT && rhs_type == TYPE_FLOAT) {
                float float_val = cond.rhs_val.float_val;
                cond.rhs_val = Value();
                cond.rhs_val.set_int((int)float_val);
                cond.rhs_val.init_raw(lhs_col->len);
            } else if (cond.is_rhs_val && lhs_type == TYPE_BIGINT && rhs_type == TYPE_INT) {
                int int_val = cond.rhs_val.int_val;
                cond.rhs_val = Value();
                cond.rhs_val.set_bigint((int64_t)int_val);
                cond.rhs_val.init_raw(lhs_col->len);
            } else if (cond.is_rhs_val && lhs_type == TYPE_FLOAT && rhs_type == TYPE_BIGINT) {
                int64_t bigint_val = cond.rhs_val.bigint_val;
                cond.rhs_val = Value();
                cond.rhs_val.set_float((float)bigint_val);
                cond.rhs_val.init_raw(lhs_col->len);
            } else if (cond.is_rhs_val && lhs_type == TYPE_DATETIME && rhs_type == TYPE_STRING) {
                // DATETIME 列与字符串常量比较 — 解析并转换
                int64_t dt;
                if (!parse_datetime(std::string(cond.rhs_val.str_val), dt)) {
                    throw InternalError("Invalid datetime value in WHERE clause");
                }
                cond.rhs_val = Value();
                cond.rhs_val.set_datetime(dt);
                cond.rhs_val.init_raw(lhs_col->len);
            } else {
                throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
            }
        } else if (cond.is_rhs_val) {
            // 类型匹配，直接 init_raw（必须在类型检查之后，避免 int->bigint 等转换时长度不匹配）
            cond.rhs_val.init_raw(lhs_col->len);
        }
    }
}


Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        if (int_lit->overflow) {
            throw InternalError("BIGINT value out of range");
        }
        // 超出 INT 范围的值用 BIGINT 类型
        if (int_lit->val > INT32_MAX || int_lit->val < INT32_MIN) {
            val.set_bigint(int_lit->val);
        } else {
            val.set_int((int)int_lit->val);
        }
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    std::map<ast::SvCompOp, CompOp> m = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
    };
    return m.at(op);
}

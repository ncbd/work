/**
 * Copyright 2023 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#include "geax-front-end/isogql/GQLAstVisitor.h"
#include "cypher/utils/geax_util.h"
#include "cypher/execution_plan/clause_guard.h"
#include "cypher/execution_plan/pattern_graph_maker.h"

namespace cypher {

geax::frontend::GEAXErrorCode PatternGraphMaker::Build(geax::frontend::AstNode* astNode) {
    cur_pattern_graph_ = -1;
    cur_types_.clear();
    return std::any_cast<geax::frontend::GEAXErrorCode>(astNode->accept(*this));
}

std::any PatternGraphMaker::visit(geax::frontend::GraphPattern* node) {
    auto& path_patterns = node->pathPatterns();
    for (auto path_pattern : path_patterns) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(path_pattern);
    }
    if (node->matchMode().has_value()) {
        NOT_SUPPORT();
    }
    if (node->keep().has_value()) {
        NOT_SUPPORT();
    }
    if (node->where().has_value()) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->where().value());
    }
    if (node->yield().has_value()) {
        NOT_SUPPORT();
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::PathPattern* node) {
    if (node->prefix().has_value()) {
        NOT_SUPPORT();
    }
    auto& chains = node->chains();
    for (auto chain : chains) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(chain);
    }
    if (node->alias().has_value()) {
        auto& pattern_graph = pattern_graphs_[cur_pattern_graph_];
        pattern_graph.symbol_table.symbols.emplace(
            node->alias().value(),
            SymbolNode(symbols_idx_[cur_pattern_graph_]++,
            cypher::SymbolNode::NAMED_PATH, SymbolNode::Scope::LOCAL));

        auto& path_elements = pattern_graph.symbol_table.anot_collection.path_elements;
        std::vector<Node>& nodes = pattern_graph.GetNodes();
        std::vector<Relationship>& relationships = pattern_graph.GetRelationships();
        if (nodes.size() != relationships.size() + 1)
            throw lgraph::CypherException("PathPattern error: " + node->alias().value());
        std::vector<std::shared_ptr<geax::frontend::Ref>> paths;
        paths.reserve(nodes.size() + relationships.size());
        size_t idx;
        std::string alias;
        for (idx = 0; idx < relationships.size(); ++idx) {
            std::shared_ptr<geax::frontend::Ref> node_ref = std::make_shared<geax::frontend::Ref>();
            alias = nodes[idx].Alias();
            node_ref->setName(std::move(alias));
            paths.push_back(node_ref);
            std::shared_ptr<geax::frontend::Ref> relp_ref = std::make_shared<geax::frontend::Ref>();
            alias = relationships[idx].Alias();
            relp_ref->setName(std::move(alias));
            paths.push_back(relp_ref);
        }
        std::shared_ptr<geax::frontend::Ref> node_ref = std::make_shared<geax::frontend::Ref>();
        alias = nodes[idx].Alias();
        node_ref->setName(std::move(alias));
        paths.push_back(node_ref);
        path_elements.emplace(node->alias().value(), paths);
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::PathChain* node) {
    auto head = node->head();
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(head);
    // todo: ...
    ClauseGuard cg(node->type(), cur_types_);
    auto& tails = node->tails();
    if (tails.size() == 0) {
        return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
    }
    for (auto [edge, end_node] : tails) {
        start_t_ = node_t_;
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(end_node);
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(edge);
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::Node* node) {
    ClauseGuard cg(node->type(), cur_types_);
    node_t_ = std::make_shared<Node>();
    if (ClauseGuard::InClause(geax::frontend::AstNodeType::kMatchStatement, cur_types_)) {
        node_t_->derivation_ = Node::Derivation::MATCHED;
    } else if (ClauseGuard::InClause(geax::frontend::AstNodeType::kInsertStatement, cur_types_)) {
        node_t_->derivation_ = Node::Derivation::CREATED;
    } else {
        NOT_SUPPORT();
    }
    auto filler = node->filler();
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(filler);
    auto& pattern_graph = pattern_graphs_[cur_pattern_graph_];
    pattern_graph.AddNode(node_t_.get());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::Edge* node) {
    ClauseGuard cg(node->type(), cur_types_);
    relp_t_ = std::make_shared<Relationship>();
    auto& pattern_graph = pattern_graphs_[cur_pattern_graph_];
    if (ClauseGuard::InClause(geax::frontend::AstNodeType::kMatchStatement, cur_types_)) {
        relp_t_->derivation_ = Relationship::Derivation::MATCHED;
    } else if (ClauseGuard::InClause(geax::frontend::AstNodeType::kInsertStatement, cur_types_)) {
        relp_t_->derivation_ = Relationship::Derivation::CREATED;
    } else {
        NOT_SUPPORT();
    }
    relp_t_->SetLhs(pattern_graph.GetNode(start_t_->Alias()).ID());
    relp_t_->SetRhs(pattern_graph.GetNode(node_t_->Alias()).ID());
    auto filler = node->filler();
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(filler);
    switch (node->direction()) {
    case geax::frontend::EdgeDirection::kPointRight:
        relp_t_->direction_ = parser::LinkDirection::LEFT_TO_RIGHT;
        break;
    case geax::frontend::EdgeDirection::kPointLeft:
        relp_t_->direction_ = parser::LinkDirection::RIGHT_TO_LEFT;
        break;
    case geax::frontend::EdgeDirection::kAnyDirected:
        relp_t_->direction_ = parser::LinkDirection::DIR_NOT_SPECIFIED;
        break;
    default:
        NOT_SUPPORT();
    }
    if (node->hopRange().has_value()) {
        auto min_hop = std::get_if<int64_t>(&std::get<0>(node->hopRange().value()));
        if (min_hop) {
            relp_t_->min_hop_ = *min_hop;
        } else {
            NOT_SUPPORT();
        }
        auto optional_max_hop = std::get<1>(node->hopRange().value());
        if (optional_max_hop.has_value()) {
            auto max_hop = std::get_if<int64_t>(&optional_max_hop.value());
            if (max_hop) {
                relp_t_->max_hop_ = *max_hop > parser::VAR_LEN_EXPAND_MAX_HOP
                                        ? parser::VAR_LEN_EXPAND_MAX_HOP
                                        : *max_hop;
            } else {
                NOT_SUPPORT();
            }
        } else {
            relp_t_->max_hop_ = parser::VAR_LEN_EXPAND_MAX_HOP;
        }
    }
    LOG_DEBUG() << relp_t_->MinHop() << " " << relp_t_->MaxHop();
    pattern_graph.AddRelationship(relp_t_.get());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::ElementFiller* node) {
    // variable
    auto& variable = node->v();
    if (!variable.has_value()) {
        NOT_SUPPORT();
    }
    SymbolNode::Type symbol_node_type;
    if (ClauseGuard::InClause(geax::frontend::AstNodeType::kNode, cur_types_)) {
        node_t_->SetAlias(variable.value());
        symbol_node_type = SymbolNode::Type::NODE;
    } else if (ClauseGuard::InClause(geax::frontend::AstNodeType::kEdge, cur_types_)) {
        relp_t_->SetAlias(variable.value());
        symbol_node_type = SymbolNode::Type::RELATIONSHIP;
    } else {
        NOT_SUPPORT();
    }
    auto& pattern_graph = pattern_graphs_[cur_pattern_graph_];
    pattern_graph.symbol_table.symbols.emplace(
        variable.value(),
        SymbolNode(symbols_idx_[cur_pattern_graph_]++, symbol_node_type, SymbolNode::Scope::LOCAL));
    // label
    auto& label = node->label();
    if (label.has_value()) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(label.value());
    }
    // predicates
    auto& predicates = node->predicates();
    if (predicates.size() > 1) {
        NOT_SUPPORT();
    }
    for (auto predicate : predicates) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(predicate);
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::WhereClause* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->predicate());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::OrderByField* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::PathModePrefix* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::PathSearchPrefix* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::SingleLabel* node) {
    if (ClauseGuard::InClause(geax::frontend::AstNodeType::kNode, cur_types_)) {
        node_t_->SetLabel(node->label());
        return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
    } else if ((ClauseGuard::InClause(geax::frontend::AstNodeType::kEdge, cur_types_))) {
        relp_t_->AddType(node->label());
        return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
    } else if ((ClauseGuard::InClause(geax::frontend::AstNodeType::kIsLabeled, cur_types_))) {
        return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
    }
    NOT_SUPPORT();
}

std::any PatternGraphMaker::visit(geax::frontend::LabelOr* node) {
    if (node->left())
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    if (node->right())
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::LabelAnd* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::LabelNot* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::PropStruct* node) {
    if (ClauseGuard::InClause(geax::frontend::AstNodeType::kMatchStatement, cur_types_)) {
        auto& properties = node->properties();
        if (properties.size() != 1) {
            NOT_SUPPORT();
        }
        auto property = node->properties()[0];
        Property p;
        auto [key, value] = property;
        p.field = key;
        if (value->type() == geax::frontend::AstNodeType::kVString) {
            p.type = Property::VALUE;
            p.value = lgraph::FieldData(((geax::frontend::VString*)value)->val());
        } else if (value->type() == geax::frontend::AstNodeType::kVInt) {
            p.type = Property::VALUE;
            p.value = lgraph::FieldData(((geax::frontend::VInt*)value)->val());
        } else if (value->type() == geax::frontend::AstNodeType::kRef) {
            p.type = Property::VARIABLE;
            p.value = lgraph::FieldData(((geax::frontend::Ref*)value)->name());
        } else {
            NOT_SUPPORT();
        }
        if (ClauseGuard::InClause(geax::frontend::AstNodeType::kNode, cur_types_)) {
            node_t_->SetProperty(p);
            return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
        } else if (ClauseGuard::InClause(geax::frontend::AstNodeType::kEdge, cur_types_)) {
            NOT_SUPPORT();
        }
        NOT_SUPPORT();
    } else if (ClauseGuard::InClause(geax::frontend::AstNodeType::kInsertStatement, cur_types_)) {
        return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
    } else {
         NOT_SUPPORT();
    }
}

std::any PatternGraphMaker::visit(geax::frontend::YieldField* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::TableFunctionClause* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::ReadConsistency* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::AllowAnonymousTable* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::OpConcurrent* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::EdgeOnJoin* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::SetAllProperties* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::UpdateProperties* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::SetLabel* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::SetSingleProperty* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::SetSchemaClause* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::SetGraphClause* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::SetTimeZoneClause* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::SetParamClause* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::ResetAll* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::ResetSchema* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::ResetTimeZone* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::ResetGraph* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::ResetParam* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::GetField* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->expr());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::TupleGet* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->expr());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::Not* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->expr());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::Neg* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->expr());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::Tilde* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->expr());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::VSome* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->expr());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BEqual* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BNotEqual* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BGreaterThan* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BNotSmallerThan* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BSmallerThan* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BNotGreaterThan* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BSafeEqual* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BAdd* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BSub* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BDiv* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BMul* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BMod* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BSquare* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BAnd* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BOr* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BXor* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BBitAnd* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BBitOr* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BBitXor* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BBitLeftShift* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BBitRightShift* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BConcat* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BIndex* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BLike* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BIn* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->left());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->right());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::If* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->condition());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->trueBody());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->falseBody());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::Function* node) {
    for (auto& arg : node->args()) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(arg);
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::Case* node) {
    if (node->input().has_value()) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->input().value());
    }
    for (auto& case_body : node->caseBodies()) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(std::get<0>(case_body));
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(std::get<1>(case_body));
    }
    if (node->elseBody().has_value()) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->elseBody().value());
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::Cast* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->expr());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::MatchCase* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->input());
    for (auto& case_body : node->cases()) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(std::get<0>(case_body));
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(std::get<1>(case_body));
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::AggFunc* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->expr());
    for (auto& expr : node->distinctBy()) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(expr);
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::BAggFunc* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::MultiCount* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::Windowing* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::MkList* node) {
    for (auto elem : node->elems()) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(elem);
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::MkMap* node) {
    for (auto [elem1, elem2] : node->elems()) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(elem1);
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(elem2);
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::MkRecord* node) {
    for (auto [elem1, elem2] : node->elems()) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(elem2);
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::MkSet* node) {
    for (auto elem : node->elems()) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(elem);
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::MkTuple* node) {
    for (auto elem : node->elems()) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(elem);
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::VBool* node) {
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::VInt* node) {
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::VDouble* node) {
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::VString* node) {
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::VDate* node) {
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::VDatetime* node) {
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::VDuration* node) {
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::VTime* node) {
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::VNull* node) {
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::VNone* node) {
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::Ref* node) {
    auto& symbols = pattern_graphs_[cur_pattern_graph_].symbol_table.symbols;
    if (symbols.find(node->name()) == symbols.end()) {
        throw lgraph::CypherException("Unknown variable: " + node->name());
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::Param* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::IsNull* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->expr());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::IsDirected* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::IsNormalized* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::IsSourceOf* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::IsDestinationOf* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::IsLabeled* node) {
    ClauseGuard cg(node->type(), cur_types_);
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->expr());
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->labelTree());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::Same* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::AllDifferent* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::Exists* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::ExplainActivity* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::SessionActivity* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::TransactionActivity* node) {
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->transaction());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::FullTransaction* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::NormalTransaction* node) {
    if (node->endTransaction().has_value()) {
        NOT_SUPPORT();
    }
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(node->query());
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::StartTransaction* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::CommitTransaction* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::RollBackTransaction* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::SessionSet* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::SessionReset* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::ProcedureBody* node) {
    pattern_graphs_.resize(node->statements().size());
    symbols_idx_.resize(node->statements().size(), 0);
    for (auto stmt : node->statements()) {
        cur_pattern_graph_ += 1;
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(stmt);
    }
    /* Introduce argument nodes & relationships */
    // todo: revisit when arguments also in MATCH
    for (auto& graph : pattern_graphs_) {
        int invisible_node_idx = 0;
        for (auto& a : graph.symbol_table.symbols) {
            if (a.second.scope == SymbolNode::ARGUMENT) {
                if (a.second.type == SymbolNode::NODE && graph.GetNode(a.first).Empty()) {
                    graph.AddNode("", a.first, Node::ARGUMENT);
                } else if (a.second.type == SymbolNode::RELATIONSHIP &&
                           graph.GetRelationship(a.first).Empty()) {
                    auto src_alias = std::string(parser::INVISIBLE)
                                         .append("NODE_")
                                         .append(std::to_string(invisible_node_idx++));
                    auto dst_alias = std::string(parser::INVISIBLE)
                                         .append("NODE_")
                                         .append(std::to_string(invisible_node_idx++));
                    auto src_nid = graph.AddNode("", src_alias, Node::ARGUMENT);
                    auto dst_nid = graph.AddNode("", dst_alias, Node::ARGUMENT);
                    graph.AddRelationship(std::set<std::string>{}, src_nid, dst_nid,
                                          parser::LinkDirection::UNKNOWN, a.first,
                                          Relationship::ARGUMENT, {});
                    auto& src_node = graph.GetNode(src_nid);
                    auto& dst_node = graph.GetNode(dst_nid);
                    src_node.Visited() = true;
                    dst_node.Visited() = true;
                }
            }
        }
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::SchemaFromPath* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::BindingValue* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::BindingGraph* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::BindingTable* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::BindingTableInnerQuery* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::BindingTableInnerExpr* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::StatementWithYield* node) {
    auto stmt = node->statement();
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(stmt);
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::QueryStatement* node) {
    auto join_query = node->joinQuery();
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(join_query);
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::StandaloneCallStatement* node) {
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::JoinQueryExpression* node) {
    auto head = node->head();
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(head);
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::JoinRightPart* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::CompositeQueryStatement* node) {
    auto head = node->head();
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(head);
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::AmbientLinearQueryStatement* node) {
    auto& query_stmts = node->queryStatements();
    for (auto query_stmt : query_stmts) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(query_stmt);
    }
    auto projection = node->resultStatement();
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(projection);
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::SelectStatement* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::FocusedQueryStatement* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::FocusedResultStatement* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::MatchStatement* node) {
    ClauseGuard cg(node->type(), cur_types_);
    auto graph = node->graphPattern();
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(graph);
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::FilterStatement* node) {
    ClauseGuard cg(node->type(), cur_types_);
    auto predicate = node->predicate();
    ACCEPT_AND_CHECK_WITH_ERROR_MSG(predicate);
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::CallQueryStatement* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::CallProcedureStatement* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::InlineProcedureCall* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::NamedProcedureCall* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::ForStatement* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::PrimitiveResultStatement* node) {
    auto& items = node->items();
    auto& pattern_graph = pattern_graphs_[cur_pattern_graph_];
    for (auto item : items) {
        auto alias = std::get<0>(item);
        auto expr = std::get<1>(item);
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(expr);
        SymbolNode::Type symbol_type = SymbolNode::Type::CONSTANT;
        if (auto ref = dynamic_cast<geax::frontend::Ref*>(expr)) {
            auto ref_symbol = pattern_graph.symbol_table.symbols.find(ref->name());
            if (ref_symbol != pattern_graph.symbol_table.symbols.end()) {
                symbol_type = ref_symbol->second.type;
            }
        }
        if (pattern_graph.symbol_table.symbols.find(alias) ==
            pattern_graph.symbol_table.symbols.end()) {
            pattern_graph.symbol_table.symbols.emplace(
                alias,
                SymbolNode(symbols_idx_[cur_pattern_graph_]++, symbol_type, SymbolNode::LOCAL));
        }
        if (cur_pattern_graph_ < pattern_graphs_.size() - 1 &&
            pattern_graphs_[cur_pattern_graph_ + 1].symbol_table.symbols.find(alias) ==
                pattern_graphs_[cur_pattern_graph_ + 1].symbol_table.symbols.end()) {
            pattern_graphs_[cur_pattern_graph_ + 1].symbol_table.symbols.emplace(
                alias, SymbolNode(symbols_idx_[cur_pattern_graph_ + 1]++, symbol_type,
                                  SymbolNode::ARGUMENT));
        }
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::CatalogModifyStatement* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::LinearDataModifyingStatement* node) {
    auto& queryStatements = node->queryStatements();
    for (auto queryStatement : queryStatements) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(queryStatement);
    }
    auto& modifyStatements_ = node->modifyStatements();
    for (auto modifyStatement : modifyStatements_) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(modifyStatement);
    }
    if (node->resultStatement().has_value()) {
        auto resultStatement = node->resultStatement().value();
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(resultStatement);
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::InsertStatement* node) {
    ClauseGuard cg(node->type(), cur_types_);
    auto& paths = node->paths();
    for (auto path : paths) {
        ACCEPT_AND_CHECK_WITH_ERROR_MSG(path);
    }
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::ReplaceStatement* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::SetStatement* node) {
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::DeleteStatement* node) {
    return geax::frontend::GEAXErrorCode::GEAX_SUCCEED;
}

std::any PatternGraphMaker::visit(geax::frontend::RemoveStatement* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::MergeStatement* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::OtherWise* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::Union* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::Except* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::Intersect* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::ShowProcessListStatement* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::KillStatement* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::ManagerStatement* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::visit(geax::frontend::DummyNode* node) { NOT_SUPPORT(); }

std::any PatternGraphMaker::reportError() { return error_msg_; }

}  // namespace cypher

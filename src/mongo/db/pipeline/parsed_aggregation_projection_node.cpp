/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/parsed_aggregation_projection_node.h"

namespace mongo {
namespace parsed_aggregation_projection {

using ArrayRecursionPolicy = ProjectionPolicies::ArrayRecursionPolicy;
using ComputedFieldsPolicy = ProjectionPolicies::ComputedFieldsPolicy;
using DefaultIdPolicy = ProjectionPolicies::DefaultIdPolicy;

ProjectionNode::ProjectionNode(ProjectionPolicies policies, std::string pathToNode)
    : _policies(policies), _pathToNode(std::move(pathToNode)) {}

void ProjectionNode::addProjectionForPath(const FieldPath& path) {
    if (path.getPathLength() == 1) {
        _projectedFields.insert(path.fullPath());
        return;
    }
    // FieldPath can't be empty, so it is safe to obtain the first path component here.
    addOrGetChild(path.getFieldName(0).toString())->addProjectionForPath(path.tail());
}

void ProjectionNode::addExpressionForPath(const FieldPath& path,
                                          boost::intrusive_ptr<Expression> expr) {
    // If the computed fields policy is 'kBanComputedFields', we should never reach here.
    invariant(_policies.computedFieldsPolicy == ComputedFieldsPolicy::kAllowComputedFields);
    if (path.getPathLength() == 1) {
        auto fieldName = path.fullPath();
        _expressions[fieldName] = expr;
        _orderToProcessAdditionsAndChildren.push_back(fieldName);
        return;
    }
    // FieldPath can't be empty, so it is safe to obtain the first path component here.
    addOrGetChild(path.getFieldName(0).toString())->addExpressionForPath(path.tail(), expr);
}

boost::intrusive_ptr<Expression> ProjectionNode::getExpressionForPath(const FieldPath& path) const {
    if (path.getPathLength() == 1) {
        if (_expressions.find(path.getFieldName(0)) != _expressions.end()) {
            return _expressions.at(path.getFieldName(0));
        }
        return nullptr;
    }
    if (auto child = getChild(path.getFieldName(0).toString())) {
        return child->getExpressionForPath(path.tail());
    }
    return nullptr;
}

ProjectionNode* ProjectionNode::addOrGetChild(const std::string& field) {
    auto child = getChild(field);
    return child ? child : addChild(field);
}

ProjectionNode* ProjectionNode::addChild(const std::string& field) {
    invariant(!str::contains(field, "."));
    _orderToProcessAdditionsAndChildren.push_back(field);
    auto insertedPair = _children.emplace(std::make_pair(field, makeChild(field)));
    return insertedPair.first->second.get();
}

ProjectionNode* ProjectionNode::getChild(const std::string& field) const {
    auto childIt = _children.find(field);
    return childIt == _children.end() ? nullptr : childIt->second.get();
}

Document ProjectionNode::applyToDocument(const Document& inputDoc) const {
    // Defer to the derived class to initialize the output document, then apply.
    MutableDocument outputDoc{initializeOutputDocument(inputDoc)};
    applyProjections(inputDoc, &outputDoc);
    applyExpressions(inputDoc, &outputDoc);

    // Make sure that we always pass through any metadata present in the input doc.
    outputDoc.copyMetaDataFrom(inputDoc);
    return outputDoc.freeze();
}

void ProjectionNode::applyProjections(const Document& inputDoc, MutableDocument* outputDoc) const {
    // Iterate over the input document so that the projected document retains its field ordering.
    auto it = inputDoc.fieldIterator();
    while (it.more()) {
        auto fieldPair = it.next();
        auto fieldName = fieldPair.first.toString();
        if (_projectedFields.count(fieldName)) {
            outputProjectedField(
                fieldName, applyLeafProjectionToValue(fieldPair.second), outputDoc);
            continue;
        }

        auto childIt = _children.find(fieldName);
        if (childIt != _children.end()) {
            outputProjectedField(
                fieldName, childIt->second->applyProjectionsToValue(fieldPair.second), outputDoc);
        }
    }

    // Ensure we project all specified fields, including those not present in the input document.
    // TODO SERVER-37791: This block is only necessary due to a bug in exclusion semantics.
    if (applyLeafProjectionToValue(Value(true)).missing()) {
        for (auto&& fieldName : _projectedFields) {
            if (inputDoc[fieldName].missing()) {
                outputProjectedField(fieldName, Value(), outputDoc);
            }
        }
    }
}

Value ProjectionNode::applyProjectionsToValue(Value inputValue) const {
    if (inputValue.getType() == BSONType::Object) {
        MutableDocument outputSubDoc{initializeOutputDocument(inputValue.getDocument())};
        applyProjections(inputValue.getDocument(), &outputSubDoc);
        return outputSubDoc.freezeToValue();
    } else if (inputValue.getType() == BSONType::Array) {
        std::vector<Value> values = inputValue.getArray();
        for (auto& value : values) {
            // If this is a nested array and our policy is to not recurse, skip the array.
            // Otherwise, descend into the array and project each element individually.
            const bool shouldSkip = value.isArray() &&
                _policies.arrayRecursionPolicy == ArrayRecursionPolicy::kDoNotRecurseNestedArrays;
            value = (shouldSkip ? transformSkippedValueForOutput(value)
                                : applyProjectionsToValue(value));
        }
        return Value(std::move(values));
    } else {
        // This represents the case where we are projecting children of a field which does not have
        // any children; for instance, applying the projection {"a.b": true} to the document {a: 2}.
        return transformSkippedValueForOutput(inputValue);
    }
}

void ProjectionNode::outputProjectedField(StringData field, Value val, MutableDocument* doc) const {
    doc->setField(field, val);
}

void ProjectionNode::applyExpressions(const Document& root, MutableDocument* outputDoc) const {
    for (auto&& field : _orderToProcessAdditionsAndChildren) {
        auto childIt = _children.find(field);
        if (childIt != _children.end()) {
            outputDoc->setField(
                field, childIt->second->applyExpressionsToValue(root, outputDoc->peek()[field]));
        } else {
            auto expressionIt = _expressions.find(field);
            invariant(expressionIt != _expressions.end());
            outputDoc->setField(
                field,
                expressionIt->second->evaluate(
                    root, &expressionIt->second->getExpressionContext()->variables));
        }
    }
}

Value ProjectionNode::applyExpressionsToValue(const Document& root, Value inputValue) const {
    if (inputValue.getType() == BSONType::Object) {
        MutableDocument outputDoc(inputValue.getDocument());
        applyExpressions(root, &outputDoc);
        return outputDoc.freezeToValue();
    } else if (inputValue.getType() == BSONType::Array) {
        std::vector<Value> values = inputValue.getArray();
        for (auto& value : values) {
            value = applyExpressionsToValue(root, value);
        }
        return Value(std::move(values));
    } else {
        if (subtreeContainsComputedFields()) {
            // Our semantics in this case are to replace whatever existing value we find with a new
            // document of all the computed values. This case represents applying a projection like
            // {"a.b": {$literal: 1}} to the document {a: 1}. This should yield {a: {b: 1}}.
            MutableDocument outputDoc;
            applyExpressions(root, &outputDoc);
            return outputDoc.freezeToValue();
        }
        // We didn't have any expressions, so just skip this value.
        return transformSkippedValueForOutput(inputValue);
    }
}

bool ProjectionNode::subtreeContainsComputedFields() const {
    return (!_expressions.empty()) ||
        std::any_of(_children.begin(), _children.end(), [](const auto& childPair) {
               return childPair.second->subtreeContainsComputedFields();
           });
}

void ProjectionNode::reportProjectedPaths(std::set<std::string>* projectedPaths) const {
    for (auto&& projectedField : _projectedFields) {
        projectedPaths->insert(FieldPath::getFullyQualifiedPath(_pathToNode, projectedField));
    }

    for (auto&& childPair : _children) {
        childPair.second->reportProjectedPaths(projectedPaths);
    }
}

void ProjectionNode::reportComputedPaths(std::set<std::string>* computedPaths,
                                         StringMap<std::string>* renamedPaths) const {
    for (auto&& computedPair : _expressions) {
        // The expression's path is the concatenation of the path to this node, plus the field name
        // associated with the expression.
        auto exprPath = FieldPath::getFullyQualifiedPath(_pathToNode, computedPair.first);
        auto exprComputedPaths = computedPair.second->getComputedPaths(exprPath);
        computedPaths->insert(exprComputedPaths.paths.begin(), exprComputedPaths.paths.end());

        for (auto&& rename : exprComputedPaths.renames) {
            (*renamedPaths)[rename.first] = rename.second;
        }
    }
    for (auto&& childPair : _children) {
        childPair.second->reportComputedPaths(computedPaths, renamedPaths);
    }
}

void ProjectionNode::optimize() {
    for (auto&& expressionIt : _expressions) {
        _expressions[expressionIt.first] = expressionIt.second->optimize();
    }
    for (auto&& childPair : _children) {
        childPair.second->optimize();
    }
}

Document ProjectionNode::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    MutableDocument outputDoc;
    serialize(explain, &outputDoc);
    return outputDoc.freeze();
}

void ProjectionNode::serialize(boost::optional<ExplainOptions::Verbosity> explain,
                               MutableDocument* output) const {
    // Determine the boolean value for projected fields in the explain output.
    const bool projVal = !applyLeafProjectionToValue(Value(true)).missing();

    // Always put "_id" first if it was projected (implicitly or explicitly).
    if (_projectedFields.find("_id") != _projectedFields.end()) {
        output->addField("_id", Value(projVal));
    }

    for (auto&& projectedField : _projectedFields) {
        if (projectedField != "_id") {
            output->addField(projectedField, Value(projVal));
        }
    }

    for (auto&& field : _orderToProcessAdditionsAndChildren) {
        auto childIt = _children.find(field);
        if (childIt != _children.end()) {
            MutableDocument subDoc;
            childIt->second->serialize(explain, &subDoc);
            output->addField(field, subDoc.freezeToValue());
        } else {
            invariant(_policies.computedFieldsPolicy == ComputedFieldsPolicy::kAllowComputedFields);
            auto expressionIt = _expressions.find(field);
            invariant(expressionIt != _expressions.end());
            output->addField(field, expressionIt->second->serialize(static_cast<bool>(explain)));
        }
    }
}

}  // namespace parsed_aggregation_projection
}  // namespace mongo

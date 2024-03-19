/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/cmd_with_let_shape.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::query_shape {

namespace {
static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");

class CmdWithLetShapeTest : public unittest::Test {};

struct DummyInnerComponent : public CmdSpecificShapeComponents {
    DummyInnerComponent(){};
    void HashValue(absl::HashState state) const override {}
    size_t size() const final {
        return sizeof(*this);
    }
};

TEST_F(CmdWithLetShapeTest, SizeOfLetShapeComponent) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto let = fromjson(R"({x: 4, y: "str"})");
    auto innerComponents = std::make_unique<DummyInnerComponent>();
    auto components = std::make_unique<LetShapeComponent>(let, expCtx, *innerComponents);

    const auto minimumSize = sizeof(CmdSpecificShapeComponents) + sizeof(BSONObj) + sizeof(bool) +
        sizeof(void*) /*CmdSpecificShapeComponents&*/ +
        static_cast<size_t>(components->shapifiedLet.objsize()) +
        components->unownedInnerComponents.size();

    ASSERT_GTE(components->size(), minimumSize);
    ASSERT_LTE(components->size(), minimumSize + 8 /*padding*/);
}

TEST_F(CmdWithLetShapeTest, SizeOfComponentWithAndWithoutLet) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto let = fromjson(R"({x: 4, y: "str"})");
    auto innerComponents = std::make_unique<DummyInnerComponent>();
    auto componentsWithLet = std::make_unique<LetShapeComponent>(let, expCtx, *innerComponents);
    auto componentsWithNoLet =
        std::make_unique<LetShapeComponent>(boost::none, expCtx, *innerComponents);

    ASSERT_LT(componentsWithNoLet->size(), componentsWithLet->size());
}

}  // namespace
}  // namespace mongo::query_shape

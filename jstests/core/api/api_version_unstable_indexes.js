/**
 * Ensure the indexes excluded from API version 1 cannot be used for query planning with
 * "APIStrict: true". Currently, "geoHaystack", "text", "columnstore", and sparse indexes are
 * excluded from API version 1. Note "geoHaystack" index has been deprecated after 4.9.
 *
 * @tags: [
 *   uses_api_parameters,
 *   assumes_read_concern_local,
 *   # the following tags are needed for the columnstore tests
 *   requires_fcv_63,
 *   tenant_migration_incompatible,
 *   does_not_support_stepdowns,
 *   not_allowed_with_security_token
 * ]
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");      // For 'getWinningPlan'.
load("jstests/libs/fixture_helpers.js");   // For 'isMongos'.
load("jstests/libs/columnstore_util.js");  // For 'setUpServerForColumnStoreIndexTest'.

const collName = "api_verision_unstable_indexes";
const coll = db[collName];
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 1, subject: "coffee", author: "xyz", views: 50},
    {_id: 2, subject: "Coffee Shopping", author: "efg", views: 5},
    {_id: 3, subject: "Baking a cake", author: "abc", views: 90},
    {_id: 4, subject: "baking", author: "xyz", views: 100},
]));

assert.commandWorked(coll.createIndex({subject: "text"}));
assert.commandWorked(coll.createIndex({"views": 1}, {sparse: true}));

// The "text" index, "subject_text", can be used normally.
if (!FixtureHelpers.isMongos(db)) {
    const explainRes = assert.commandWorked(
        db.runCommand({explain: {"find": collName, "filter": {$text: {$search: "coffee"}}}}));
    assert.eq(getWinningPlan(explainRes.queryPlanner).indexName, "subject_text", explainRes);
}

// No "text" index can be used for $text search as the "text" index is excluded from API version 1.
assert.commandFailedWithCode(db.runCommand({
    explain: {"find": collName, "filter": {$text: {$search: "coffee"}}},
    apiVersion: "1",
    apiStrict: true
}),
                             ErrorCodes.NoQueryExecutionPlans);

// Can not hint a sparse index which is excluded from API version 1 with 'apiStrict: true'.
assert.commandFailedWithCode(db.runCommand({
    "find": collName,
    "filter": {views: 50},
    "hint": {views: 1},
    apiVersion: "1",
    apiStrict: true
}),
                             ErrorCodes.BadValue);

if (!FixtureHelpers.isMongos(db)) {
    const explainRes = assert.commandWorked(
        db.runCommand({explain: {"find": collName, "filter": {views: 50}, "hint": {views: 1}}}));
    assert.eq(getWinningPlan(explainRes.queryPlanner).inputStage.indexName, "views_1", explainRes);
}

if (setUpServerForColumnStoreIndexTest(db)) {
    // Column store indexes cannot be created with apiStrict: true.
    assert.commandFailedWithCode(db.runCommand({
        createIndexes: coll.getName(),
        indexes: [{key: {"$**": "columnstore"}, name: "$**_columnstore"}],
        apiVersion: "1",
        apiStrict: true
    }),
                                 ErrorCodes.APIStrictError);

    // Column store indexes cannot be used for query planning with apiStrict: true.
    coll.createIndex({"$**": "columnstore"});
    assert.commandFailedWithCode(db.runCommand({
        "find": coll.getName(),
        "projection": {_id: 0, x: 1},
        "hint": {"$**": "columnstore"},
        apiVersion: "1",
        apiStrict: true
    }),
                                 ErrorCodes.BadValue);
}
})();

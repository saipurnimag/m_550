/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_util.h"

#include <absl/container/node_hash_map.h>
#include <algorithm>
#include <boost/optional.hpp>
#include <cmath>
#include <fmt/format.h>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_find_and_modify_image_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/resharding/document_source_resharding_add_resume_id.h"
#include "mongo/db/s/resharding/document_source_resharding_iterate_transaction.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace resharding {

namespace {
/**
 * Given a constant rate of time per unit of work:
 *    totalTime / totalWork == elapsedTime / elapsedWork
 * Solve for remaining time.
 *    remainingTime := totalTime - elapsedTime
 *                  == (totalWork * (elapsedTime / elapsedWork)) - elapsedTime
 *                  == elapsedTime * (totalWork / elapsedWork - 1)
 */
Milliseconds estimateRemainingTime(Milliseconds elapsedTime, double elapsedWork, double totalWork) {
    elapsedWork = std::min(elapsedWork, totalWork);
    double remainingMsec = 1.0 * elapsedTime.count() * (totalWork / elapsedWork - 1);
    return Milliseconds(Milliseconds::rep(std::round(remainingMsec)));
}
}  // namespace

using namespace fmt::literals;

BSONObj serializeAndTruncateReshardingErrorIfNeeded(Status originalError) {
    BSONObjBuilder originalBob;
    originalError.serializeErrorToBSON(&originalBob);
    auto originalObj = originalBob.obj();

    if (originalObj.objsize() <= kReshardErrorMaxBytes ||
        originalError.code() == ErrorCodes::ReshardCollectionTruncatedError) {
        // The provided originalError either meets the size constraints or has already been
        // truncated (and is just slightly larger than 2000 bytes to avoid complicating the
        // truncation math).
        return originalObj;
    }

    // ReshardCollectionAborted has special internal handling. It should always have a short, fixed
    // error message so it never exceeds the size limit and requires truncation and error code
    // substitution.
    invariant(originalError.code() != ErrorCodes::ReshardCollectionAborted);

    auto originalErrorStr = originalError.toString();
    auto truncatedErrorStr =
        str::UTF8SafeTruncation(StringData(originalErrorStr), kReshardErrorMaxBytes);
    Status truncatedError{ErrorCodes::ReshardCollectionTruncatedError, truncatedErrorStr};
    BSONObjBuilder truncatedBob;
    truncatedError.serializeErrorToBSON(&truncatedBob);
    return truncatedBob.obj();
}

DonorShardEntry makeDonorShard(ShardId shardId,
                               DonorStateEnum donorState,
                               boost::optional<Timestamp> minFetchTimestamp,
                               boost::optional<Status> abortReason) {
    DonorShardContext donorCtx;
    donorCtx.setState(donorState);
    emplaceMinFetchTimestampIfExists(donorCtx, minFetchTimestamp);
    emplaceTruncatedAbortReasonIfExists(donorCtx, abortReason);

    return DonorShardEntry{std::move(shardId), std::move(donorCtx)};
}

RecipientShardEntry makeRecipientShard(ShardId shardId,
                                       RecipientStateEnum recipientState,
                                       boost::optional<Status> abortReason) {
    RecipientShardContext recipientCtx;
    recipientCtx.setState(recipientState);
    emplaceTruncatedAbortReasonIfExists(recipientCtx, abortReason);

    return RecipientShardEntry{std::move(shardId), std::move(recipientCtx)};
}

NamespaceString constructTemporaryReshardingNss(const NamespaceString& nss,
                                                const UUID& sourceUuid) {
    auto tempCollPrefix = nss.isTimeseriesBucketsCollection()
        ? NamespaceString::kTemporaryTimeseriesReshardingCollectionPrefix
        : NamespaceString::kTemporaryReshardingCollectionPrefix;
    return NamespaceStringUtil::deserialize(
        boost::none,
        nss.db_forSharding(),
        fmt::format("{}{}", tempCollPrefix, sourceUuid.toString()),
        SerializationContext::stateDefault());
}

std::set<ShardId> getRecipientShards(OperationContext* opCtx,
                                     const NamespaceString& sourceNss,
                                     const UUID& reshardingUUID) {
    const auto& tempNss = constructTemporaryReshardingNss(sourceNss, reshardingUUID);
    auto* catalogCache = Grid::get(opCtx)->catalogCache();
    auto [cm, _] = catalogCache->getTrackedCollectionRoutingInfo(opCtx, tempNss);

    std::set<ShardId> recipients;
    cm.getAllShardIds(&recipients);
    return recipients;
}

void checkForHolesAndOverlapsInChunks(std::vector<ReshardedChunk>& chunks,
                                      const KeyPattern& keyPattern) {
    std::sort(chunks.begin(), chunks.end(), [](const ReshardedChunk& a, const ReshardedChunk& b) {
        return SimpleBSONObjComparator::kInstance.evaluate(a.getMin() < b.getMin());
    });
    // Check for global minKey and maxKey
    uassert(ErrorCodes::BadValue,
            "Chunk range must start at global min for new shard key",
            SimpleBSONObjComparator::kInstance.evaluate(chunks.front().getMin() ==
                                                        keyPattern.globalMin()));
    uassert(ErrorCodes::BadValue,
            "Chunk range must end at global max for new shard key",
            SimpleBSONObjComparator::kInstance.evaluate(chunks.back().getMax() ==
                                                        keyPattern.globalMax()));

    boost::optional<BSONObj> prevMax = boost::none;
    for (const auto& chunk : chunks) {
        if (prevMax) {
            uassert(ErrorCodes::BadValue,
                    "Chunk ranges must be contiguous",
                    SimpleBSONObjComparator::kInstance.evaluate(prevMax.value() == chunk.getMin()));
        }
        prevMax = boost::optional<BSONObj>(chunk.getMax());
    }
}

void validateReshardedChunks(const std::vector<ReshardedChunk>& chunks,
                             OperationContext* opCtx,
                             const KeyPattern& keyPattern) {
    std::vector<ReshardedChunk> validChunks;
    for (const auto& chunk : chunks) {
        uassertStatusOK(
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, chunk.getRecipientShardId()));
        validChunks.push_back(chunk);
    }
    checkForHolesAndOverlapsInChunks(validChunks, keyPattern);
}

Timestamp getHighestMinFetchTimestamp(const std::vector<DonorShardEntry>& donorShards) {
    invariant(!donorShards.empty());

    auto maxMinFetchTimestamp = Timestamp::min();
    for (auto& donor : donorShards) {
        auto donorFetchTimestamp = donor.getMutableState().getMinFetchTimestamp();
        uassert(4957300,
                "All donors must have a minFetchTimestamp, but donor {} does not."_format(
                    StringData{donor.getId()}),
                donorFetchTimestamp.has_value());
        if (maxMinFetchTimestamp < donorFetchTimestamp.value()) {
            maxMinFetchTimestamp = donorFetchTimestamp.value();
        }
    }
    return maxMinFetchTimestamp;
}

void checkForOverlappingZones(std::vector<ReshardingZoneType>& zones) {
    std::sort(
        zones.begin(), zones.end(), [](const ReshardingZoneType& a, const ReshardingZoneType& b) {
            return SimpleBSONObjComparator::kInstance.evaluate(a.getMin() < b.getMin());
        });

    boost::optional<BSONObj> prevMax = boost::none;
    for (const auto& zone : zones) {
        if (prevMax) {
            uassert(ErrorCodes::BadValue,
                    "Zone ranges must not overlap",
                    SimpleBSONObjComparator::kInstance.evaluate(prevMax.value() <= zone.getMin()));
        }
        prevMax = boost::optional<BSONObj>(zone.getMax());
    }
}

std::vector<BSONObj> buildTagsDocsFromZones(const NamespaceString& tempNss,
                                            std::vector<ReshardingZoneType>& zones,
                                            const ShardKeyPattern& shardKey) {
    std::vector<BSONObj> tags;
    tags.reserve(zones.size());
    for (auto& zone : zones) {
        zone.setMin(shardKey.getKeyPattern().extendRangeBound(zone.getMin(), false));
        zone.setMax(shardKey.getKeyPattern().extendRangeBound(zone.getMax(), false));
        ChunkRange range(zone.getMin(), zone.getMax());
        TagsType tag(tempNss, zone.getZone().toString(), range);
        tags.push_back(tag.toBSON());
    }

    return tags;
}

std::vector<ReshardingZoneType> getZonesFromExistingCollection(OperationContext* opCtx,
                                                               const NamespaceString& sourceNss) {
    std::vector<ReshardingZoneType> zones;
    const auto collectionZones = uassertStatusOK(
        ShardingCatalogManager::get(opCtx)->localCatalogClient()->getTagsForCollection(opCtx,
                                                                                       sourceNss));

    for (const auto& zone : collectionZones) {
        ReshardingZoneType newZone(zone.getTag(), zone.getMinKey(), zone.getMaxKey());
        zones.push_back(newZone);
    }
    return zones;
}

std::unique_ptr<Pipeline, PipelineDeleter> createOplogFetchingPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ReshardingDonorOplogId& startAfter,
    UUID collUUID,
    const ShardId& recipientShard) {
    using Doc = Document;
    using Arr = std::vector<Value>;
    using V = Value;
    const Value EXISTS = V{Doc{{"$exists", true}}};
    const Value DNE = V{Doc{{"$exists", false}}};

    Pipeline::SourceContainer stages;
    // The node receiving the query verifies continuity of oplog entries (i.e: that the recipient
    // hasn't fallen off the oplog). This stage provides the input timestamp that the donor uses for
    // verification.
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"ts", Doc{{"$gte", startAfter.getTs()}}}}.toBson(), expCtx));

    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"$or",
             // Only capture CRUD operations relevant for the `destinedRecipient`.
             Arr{V{Doc{{"op", Doc{{"$in", Arr{V{"i"_sd}, V{"u"_sd}, V{"d"_sd}, V{"n"_sd}}}}},
                       {"ui", collUUID},
                       {"destinedRecipient", recipientShard.toString()}}},
                 // Capture all commands. One cannot determine if a command is relevant to the
                 // `destinedRecipient` until after oplog chaining via `prevOpTime` is resolved.
                 V{Doc{{"op", "c"_sd},
                       {"o.applyOps", EXISTS},
                       {"o.partialTxn", DNE},
                       {"o.prepare", DNE}}},
                 V{Doc{{"op", "c"_sd}, {"o.commitTransaction", EXISTS}}},
                 V{Doc{{"op", "c"_sd}, {"o.abortTransaction", EXISTS}}},
                 V{Doc{{"op", "c"_sd}, {"ui", collUUID}}}}}}
            .toBson(),
        expCtx));

    // Emits transaction entries chronologically.
    stages.emplace_back(DocumentSourceReshardingIterateTransaction::create(
        expCtx, true /* includeCommitTransactionTimestamp */));

    // Converts oplog entries with kNeedsRetryImageFieldName into the old style pair of
    // update/delete oplog and pre/post image no-op oplog.
    stages.emplace_back(DocumentSourceFindAndModifyImageLookup::create(
        expCtx, true /* includeCommitTransactionTimestamp */));

    // Adds _id to all events in the stream.
    stages.emplace_back(DocumentSourceReshardingAddResumeId::create(expCtx));

    // Filter out applyOps entries which do not contain any relevant operations.
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"$or",
             Arr{V{Doc{{"op", Doc{{"$ne", "c"_sd}}}}},
                 V{Doc{{"op", "c"_sd}, {"o.applyOps", DNE}}},
                 V{Doc{{"op", "c"_sd},
                       {"o.applyOps",
                        Doc{{"$elemMatch",
                             Doc{{"destinedRecipient", recipientShard.toString()},
                                 {"ui", collUUID}}}}}}}}}}
            .toBson(),
        expCtx));

    // Now that the chained oplog entries are adjacent with an annotated `ReshardingDonorOplogId`,
    // the pipeline can prune anything earlier than the resume time.
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"_id", Doc{{"$gt", startAfter.toBSON()}}}}.toBson(), expCtx));

    // Filter out anything inside of an `applyOps` specifically destined for another shard. This
    // ensures zone restrictions are obeyed. Data will never be sent to a shard that it isn't meant
    // to end up on.
    stages.emplace_back(DocumentSourceAddFields::create(
        Doc{{"o.applyOps",
             Doc{{"$cond",
                  Doc{{"if", Doc{{"$eq", Arr{V{"$op"_sd}, V{"c"_sd}}}}},
                      {"then",
                       Doc{{"$filter",
                            Doc{{"input", "$o.applyOps"_sd},
                                {"cond",
                                 Doc{{"$and",
                                      Arr{V{Doc{{"$eq", Arr{V{"$$this.ui"_sd}, V{collUUID}}}}},
                                          V{Doc{{"$eq",
                                                 Arr{V{"$$this.destinedRecipient"_sd},
                                                     V{recipientShard.toString()}}}}}}}}}}}}},
                      {"else", "$o.applyOps"_sd}}}}}}
            .toBson(),
        expCtx));

    return Pipeline::create(std::move(stages), expCtx);
}

bool isFinalOplog(const repl::OplogEntry& oplog) {
    if (oplog.getOpType() != repl::OpTypeEnum::kNoop) {
        return false;
    }

    auto o2Field = oplog.getObject2();
    if (!o2Field) {
        return false;
    }

    return o2Field->getField("type").valueStringDataSafe() == kReshardFinalOpLogType;
}

bool isFinalOplog(const repl::OplogEntry& oplog, UUID reshardingUUID) {
    if (!isFinalOplog(oplog)) {
        return false;
    }

    return uassertStatusOK(UUID::parse(oplog.getObject2()->getField("reshardingUUID"))) ==
        reshardingUUID;
}

NamespaceString getLocalOplogBufferNamespace(UUID existingUUID, ShardId donorShardId) {
    return NamespaceString::makeReshardingLocalOplogBufferNSS(existingUUID,
                                                              donorShardId.toString());
}

NamespaceString getLocalConflictStashNamespace(UUID existingUUID, ShardId donorShardId) {
    return NamespaceString::makeReshardingLocalConflictStashNSS(existingUUID,
                                                                donorShardId.toString());
}

void doNoopWrite(OperationContext* opCtx, StringData opStr, const NamespaceString& nss) {
    writeConflictRetry(opCtx, opStr, NamespaceString::kRsOplogNamespace, [&] {
        AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);

        const std::string msg = str::stream() << opStr << " on " << nss.toStringForErrorMsg();
        WriteUnitOfWork wuow(opCtx);
        opCtx->getClient()->getServiceContext()->getOpObserver()->onInternalOpMessage(
            opCtx,
            {},
            boost::none,
            BSON("msg" << msg),
            boost::none,
            boost::none,
            boost::none,
            boost::none,
            boost::none);
        wuow.commit();
    });
}

boost::optional<Milliseconds> estimateRemainingRecipientTime(bool applyingBegan,
                                                             int64_t bytesCopied,
                                                             int64_t bytesToCopy,
                                                             Milliseconds timeSpentCopying,
                                                             int64_t oplogEntriesApplied,
                                                             int64_t oplogEntriesFetched,
                                                             Milliseconds timeSpentApplying) {
    if (applyingBegan && oplogEntriesFetched == 0) {
        return Milliseconds(0);
    }
    if (oplogEntriesApplied > 0 && oplogEntriesFetched > 0) {
        // All fetched oplogEntries must be applied. Some of them already have been.
        return estimateRemainingTime(timeSpentApplying, oplogEntriesApplied, oplogEntriesFetched);
    }
    if (bytesCopied > 0 && bytesToCopy > 0) {
        // Until the time to apply batches of oplog entries is measured, we assume that applying all
        // of them will take as long as copying did.
        return estimateRemainingTime(timeSpentCopying, bytesCopied, 2 * bytesToCopy);
    }
    return {};
}

void validateShardDistribution(const std::vector<ShardKeyRange>& shardDistribution,
                               OperationContext* opCtx,
                               const ShardKeyPattern& keyPattern) {
    boost::optional<bool> hasMinMax = boost::none;
    std::vector<ShardKeyRange> validShards;
    stdx::unordered_set<ShardId> shardIds;
    for (const auto& shard : shardDistribution) {
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shard.getShard()));
        uassert(ErrorCodes::InvalidOptions,
                "ShardKeyRange should have a pair of min/max or none of them",
                !(shard.getMax().has_value() ^ shard.getMin().has_value()));
        uassert(ErrorCodes::InvalidOptions,
                "ShardKeyRange min should follow shard key's keyPattern",
                (!shard.getMin().has_value()) || keyPattern.isShardKey(*shard.getMin()));
        uassert(ErrorCodes::InvalidOptions,
                "ShardKeyRange max should follow shard key's keyPattern",
                (!shard.getMax().has_value()) || keyPattern.isShardKey(*shard.getMax()));
        if (hasMinMax && !(*hasMinMax)) {
            uassert(ErrorCodes::InvalidOptions,
                    "Non-explicit shardDistribution should have unique shardIds",
                    shardIds.find(shard.getShard()) == shardIds.end());
        }

        // Check all shardKeyRanges have min/max or none of them has min/max.
        if (hasMinMax.has_value()) {
            uassert(ErrorCodes::InvalidOptions,
                    "All ShardKeyRanges should have the same min/max pattern",
                    !(*hasMinMax ^ shard.getMax().has_value()));
        } else {
            hasMinMax = shard.getMax().has_value();
        }

        validShards.push_back(shard);
        shardIds.insert(shard.getShard());
    }

    // If the shardDistribution contains min/max, validate whether they are continuous and complete.
    if (hasMinMax && *hasMinMax) {
        std::sort(validShards.begin(),
                  validShards.end(),
                  [](const ShardKeyRange& a, const ShardKeyRange& b) {
                      return SimpleBSONObjComparator::kInstance.evaluate(*a.getMin() < *b.getMin());
                  });

        uassert(
            ErrorCodes::InvalidOptions,
            "ShardKeyRange must start at global min for the new shard key",
            SimpleBSONObjComparator::kInstance.evaluate(validShards.front().getMin().value() ==
                                                        keyPattern.getKeyPattern().globalMin()));
        uassert(ErrorCodes::InvalidOptions,
                "ShardKeyRange must end at global max for the new shard key",
                SimpleBSONObjComparator::kInstance.evaluate(
                    validShards.back().getMax().value() == keyPattern.getKeyPattern().globalMax()));

        boost::optional<BSONObj> prevMax = boost::none;
        for (const auto& shard : validShards) {
            if (prevMax) {
                uassert(ErrorCodes::InvalidOptions,
                        "ShardKeyRanges must be continuous",
                        SimpleBSONObjComparator::kInstance.evaluate(prevMax.value() ==
                                                                    *shard.getMin()));
            }
            prevMax = *shard.getMax();
        }
    }
}

bool isMoveCollection(boost::optional<ProvenanceEnum> provenance) {
    return provenance &&
        (provenance.get() == ProvenanceEnum::kMoveCollection ||
         provenance.get() == ProvenanceEnum::kBalancerMoveCollection);
}

}  // namespace resharding
}  // namespace mongo

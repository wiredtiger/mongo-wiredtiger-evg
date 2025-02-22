/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/db/pipeline/document_source_change_stream.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/bson/bson_helper.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_check_resume_token.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_lookup_change_post_image.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;
using boost::optional;
using std::list;
using std::string;
using std::vector;

// The $changeStream stage is an alias for many stages, but we need to be able to serialize
// and re-parse the pipeline. To make this work, the 'transformation' stage will serialize itself
// with the original specification, and all other stages that are created during the alias expansion
// will not serialize themselves.
REGISTER_MULTI_STAGE_ALIAS(changeStream,
                           DocumentSourceChangeStream::LiteParsed::parse,
                           DocumentSourceChangeStream::createFromBson);

constexpr StringData DocumentSourceChangeStream::kDocumentKeyField;
constexpr StringData DocumentSourceChangeStream::kFullDocumentField;
constexpr StringData DocumentSourceChangeStream::kIdField;
constexpr StringData DocumentSourceChangeStream::kNamespaceField;
constexpr StringData DocumentSourceChangeStream::kUuidField;
constexpr StringData DocumentSourceChangeStream::kUpdateDescriptionField;
constexpr StringData DocumentSourceChangeStream::kOperationTypeField;
constexpr StringData DocumentSourceChangeStream::kStageName;
constexpr StringData DocumentSourceChangeStream::kClusterTimeField;
constexpr StringData DocumentSourceChangeStream::kTxnNumberField;
constexpr StringData DocumentSourceChangeStream::kLsidField;
constexpr StringData DocumentSourceChangeStream::kUpdateOpType;
constexpr StringData DocumentSourceChangeStream::kDeleteOpType;
constexpr StringData DocumentSourceChangeStream::kReplaceOpType;
constexpr StringData DocumentSourceChangeStream::kInsertOpType;
constexpr StringData DocumentSourceChangeStream::kInvalidateOpType;
constexpr StringData DocumentSourceChangeStream::kNewShardDetectedOpType;

constexpr StringData DocumentSourceChangeStream::kRegexAllCollections;
constexpr StringData DocumentSourceChangeStream::kRegexAllDBs;
constexpr StringData DocumentSourceChangeStream::kRegexCmdColl;

namespace {

static constexpr StringData kOplogMatchExplainName = "$_internalOplogMatch"_sd;
}  // namespace

intrusive_ptr<DocumentSourceOplogMatch> DocumentSourceOplogMatch::create(
    BSONObj filter, const intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceOplogMatch(std::move(filter), expCtx);
}

const char* DocumentSourceOplogMatch::getSourceName() const {
    // This is used in error reporting, particularly if we find this stage in a position other
    // than first, so report the name as $changeStream.
    return DocumentSourceChangeStream::kStageName.rawData();
}

DocumentSource::StageConstraints DocumentSourceOplogMatch::constraints(
    Pipeline::SplitState pipeState) const {

    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kFirst,
                                 HostTypeRequirement::kAnyShard,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage);
    constraints.isIndependentOfAnyCollection =
        pExpCtx->ns.isCollectionlessAggregateNS() ? true : false;
    return constraints;
}

/**
 * Only serialize this stage for explain purposes, otherwise keep it hidden so that we can
 * properly alias.
 */
Value DocumentSourceOplogMatch::serialize(optional<ExplainOptions::Verbosity> explain) const {
    if (explain) {
        return Value(Document{{kOplogMatchExplainName, Document{}}});
    }
    return Value();
}

DocumentSourceOplogMatch::DocumentSourceOplogMatch(BSONObj filter,
                                                   const intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceMatch(std::move(filter), expCtx) {}

namespace {
/**
 * This stage is used internally for change notifications to close cursor after returning
 * "invalidate" entries.
 * It is not intended to be created by the user.
 */
class DocumentSourceCloseCursor final : public DocumentSource, public SplittableDocumentSource {
public:
    GetNextResult getNext() final;

    const char* getSourceName() const final {
        // This is used in error reporting.
        return "$changeStream";
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        // This stage should never be in the shards part of a split pipeline.
        invariant(pipeState != Pipeline::SplitState::kSplitForShards);
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                (pipeState == Pipeline::SplitState::kUnsplit ? HostTypeRequirement::kNone
                                                             : HostTypeRequirement::kMongoS),
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kNotAllowed,
                ChangeStreamRequirement::kChangeStreamStage};
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        // This stage is created by the DocumentSourceChangeStream stage, so serializing it
        // here would result in it being created twice.
        return Value();
    }

    static boost::intrusive_ptr<DocumentSourceCloseCursor> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceCloseCursor(expCtx);
    }

    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return nullptr;
    }

    std::list<boost::intrusive_ptr<DocumentSource>> getMergeSources() final {
        // This stage must run on mongos to ensure it sees any invalidation in the correct order,
        // and to ensure that all remote cursors are cleaned up properly. We also must include a
        // mergingPresorted $sort stage to communicate to the AsyncResultsMerger that we need to
        // merge the streams in a particular order.
        const bool mergingPresorted = true;
        const long long noLimit = -1;
        auto sortMergingPresorted =
            DocumentSourceSort::create(pExpCtx,
                                       change_stream_constants::kSortSpec,
                                       noLimit,
                                       DocumentSourceSort::kMaxMemoryUsageBytes,
                                       mergingPresorted);
        return {sortMergingPresorted, this};
    }

private:
    /**
     * Use the create static method to create a DocumentSourceCloseCursor.
     */
    DocumentSourceCloseCursor(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(expCtx) {}

    bool _shouldCloseCursor = false;
};

DocumentSource::GetNextResult DocumentSourceCloseCursor::getNext() {
    pExpCtx->checkForInterrupt();

    // Close cursor if we have returned an invalidate entry.
    if (_shouldCloseCursor) {
        uasserted(ErrorCodes::CloseChangeStream, "Change stream has been invalidated");
    }

    auto nextInput = pSource->getNext();
    if (!nextInput.isAdvanced())
        return nextInput;

    auto doc = nextInput.getDocument();
    const auto& kOperationTypeField = DocumentSourceChangeStream::kOperationTypeField;
    DocumentSourceChangeStream::checkValueType(
        doc[kOperationTypeField], kOperationTypeField, BSONType::String);
    auto operationType = doc[kOperationTypeField].getString();
    if (operationType == DocumentSourceChangeStream::kInvalidateOpType) {
        // Pass the invalidation forward, so that it can be included in the results, or
        // filtered/transformed by further stages in the pipeline, then throw an exception
        // to close the cursor on the next call to getNext().
        _shouldCloseCursor = true;
    }

    return nextInput;
}

}  // namespace

void DocumentSourceChangeStream::checkValueType(const Value v,
                                                const StringData filedName,
                                                BSONType expectedType) {
    uassert(40532,
            str::stream() << "Entry field \"" << filedName << "\" should be "
                          << typeName(expectedType)
                          << ", found: "
                          << typeName(v.getType()),
            (v.getType() == expectedType));
}

//
// Helpers for building the oplog filter.
//
namespace {

/**
 * Constructs the filter which will match 'applyOps' oplog entries that are:
 * 1) Part of a transaction
 * 2) Have sub-entries which should be returned in the change stream
 */
BSONObj getTxnApplyOpsFilter(BSONElement nsMatch, const NamespaceString& nss) {
    BSONObjBuilder applyOpsBuilder;
    applyOpsBuilder.append("op", "c");
    applyOpsBuilder.append("lsid", BSON("$exists" << true));
    applyOpsBuilder.append("txnNumber", BSON("$exists" << true));
    const std::string& kApplyOpsNs = "o.applyOps.ns";
    applyOpsBuilder.appendAs(nsMatch, kApplyOpsNs);
    return applyOpsBuilder.obj();
}
}  // namespace

DocumentSourceChangeStream::ChangeStreamType DocumentSourceChangeStream::getChangeStreamType(
    const NamespaceString& nss) {

    // If we have been permitted to run on admin, 'allChangesForCluster' must be true.
    return (nss.isAdminDB()
                ? ChangeStreamType::kAllChangesForCluster
                : (nss.isCollectionlessAggregateNS() ? ChangeStreamType::kSingleDatabase
                                                     : ChangeStreamType::kSingleCollection));
}

std::string DocumentSourceChangeStream::getNsRegexForChangeStream(const NamespaceString& nss) {
    auto type = getChangeStreamType(nss);
    switch (type) {
        case ChangeStreamType::kSingleCollection:
            // Match the target namespace exactly.
            return "^" + nss.ns() + "$";
        case ChangeStreamType::kSingleDatabase:
            // Match all namespaces that start with db name, followed by ".", then NOT followed by
            // '$' or 'system.'
            return "^" + nss.db() + kRegexAllCollections;
        case ChangeStreamType::kAllChangesForCluster:
            // Match all namespaces that start with any db name other than admin, config, or local,
            // followed by ".", then NOT followed by '$' or 'system.'
            return "^" + kRegexAllDBs + kRegexAllCollections;
        default:
            MONGO_UNREACHABLE;
    }
}


BSONObj DocumentSourceChangeStream::buildMatchFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Timestamp startFrom,
    bool startFromInclusive) {
    auto nss = expCtx->ns;

    ChangeStreamType sourceType = getChangeStreamType(nss);

    // 1) Supported commands that have the target db namespace (e.g. test.$cmd) in "ns" field.
    BSONArrayBuilder invalidatingCommands;
    invalidatingCommands.append(BSON("o.dropDatabase" << 1));

    if (sourceType == ChangeStreamType::kSingleCollection) {
        invalidatingCommands.append(BSON("o.drop" << nss.coll()));
        invalidatingCommands.append(BSON("o.renameCollection" << nss.ns()));
        if (expCtx->collation.isEmpty()) {
            // If the user did not specify a collation, they should be using the collection's
            // default collation. So a "create" command which has any collation present would
            // invalidate the change stream, since that must mean the stream was created before the
            // collection existed and used the simple collation, which is no longer the default.
            invalidatingCommands.append(
                BSON("o.create" << nss.coll() << "o.collation" << BSON("$exists" << true)));
        }
    } else {
        // For change streams on an entire database, the stream is invalidated if any collections in
        // that database are dropped or renamed. For cluster-wide streams, drops or renames of any
        // collection in any database (aside from the internal databases admin, config and local)
        // will invalidate the stream.
        invalidatingCommands.append(BSON("o.drop" << BSON("$exists" << true)));
        invalidatingCommands.append(BSON("o.renameCollection" << BSON("$exists" << true)));
    }

    // For cluster-wide $changeStream, match the command namespace of any database other than admin,
    // config, or local. Otherwise, match only against the target db's command namespace.
    auto cmdNsFilter = (sourceType == ChangeStreamType::kAllChangesForCluster
                            ? BSON("ns" << BSONRegEx("^" + kRegexAllDBs + kRegexCmdColl))
                            : BSON("ns" << nss.getCommandNS().ns()));

    // 1.1) Commands that are on target db(s) and one of the above invalidating commands.
    auto commandsOnTargetDb =
        BSON("$and" << BSON_ARRAY(cmdNsFilter << BSON("$or" << invalidatingCommands.arr())));

    // 1.2) Supported commands that have arbitrary db namespaces in "ns" field.
    auto renameDropTarget = (sourceType == ChangeStreamType::kAllChangesForCluster
                                 ? BSON("o.to" << BSON("$exists" << true))
                                 : BSON("o.to" << nss.ns()));

    // All supported commands that are either (1.1) or (1.2).
    BSONObj commandMatch = BSON("op"
                                << "c"
                                << OR(commandsOnTargetDb, renameDropTarget));

    // 2.1) Normal CRUD ops.
    auto normalOpTypeMatch = BSON("op" << NE << "n");

    // 2.2) A chunk gets migrated to a new shard that doesn't have any chunks.
    auto chunkMigratedMatch = BSON("op"
                                   << "n"
                                   << "o2.type"
                                   << "migrateChunkToNewShard");

    // 2) Supported operations on the target namespace.
    BSONObj nsMatch = BSON("ns" << BSONRegEx(getNsRegexForChangeStream(nss)));
    auto opMatch = BSON(nsMatch["ns"] << OR(normalOpTypeMatch, chunkMigratedMatch));

    // 3) Look for 'applyOps' which were created as part of a transaction.
    BSONObj applyOps = getTxnApplyOpsFilter(nsMatch["ns"], nss);

    // Match oplog entries after "start" and are either supported (1) commands or (2) operations,
    // excepting those tagged "fromMigrate". Include the resume token, if resuming, so we can verify
    // it was still present in the oplog.
    return BSON("$and" << BSON_ARRAY(BSON("ts" << (startFromInclusive ? GTE : GT) << startFrom)
                                     << BSON(OR(opMatch, commandMatch, applyOps))
                                     << BSON("fromMigrate" << NE << true)));
}

namespace {

/**
 * Parses the resume options in 'spec', optionally populating the resume stage and cluster time to
 * start from.  Throws an AssertionException if not running on a replica set or multiple resume
 * options are specified.
 */
void parseResumeOptions(const intrusive_ptr<ExpressionContext>& expCtx,
                        const DocumentSourceChangeStreamSpec& spec,
                        ServerGlobalParams::FeatureCompatibility::Version fcv,
                        intrusive_ptr<DocumentSource>* resumeStageOut,
                        boost::optional<Timestamp>* startFromOut) {
    if (!expCtx->inMongos) {
        auto replCoord = repl::ReplicationCoordinator::get(expCtx->opCtx);
        uassert(40573,
                "The $changeStream stage is only supported on replica sets",
                replCoord &&
                    replCoord->getReplicationMode() ==
                        repl::ReplicationCoordinator::Mode::modeReplSet);
        *startFromOut = replCoord->getMyLastAppliedOpTime().getTimestamp();
    }

    if (auto resumeAfter = spec.getResumeAfter()) {
        ResumeToken token = resumeAfter.get();
        ResumeTokenData tokenData = token.getData();
        uassert(40645,
                "The resume token is invalid (no UUID), possibly from an invalidate.",
                tokenData.uuid);
        auto resumeNamespace =
            UUIDCatalog::get(expCtx->opCtx).lookupNSSByUUID(tokenData.uuid.get());
        // If the resume token's UUID does not exist - implying that it has been dropped in the time
        // since the resume token was generated - then we prohibit resuming the stream, because we
        // can no longer determine whether that collection had a default collation. However, the
        // concept of a default collation does not exist at the database or cluster levels, and we
        // therefore skip this check for whole-database and cluster-wide change streams.
        if (!expCtx->inMongos && expCtx->isSingleNamespaceAggregation()) {
            uassert(40615,
                    "The resume token UUID does not exist. Has the collection been dropped?",
                    !resumeNamespace.isEmpty());
        }
        *startFromOut = tokenData.clusterTime;
        if (expCtx->needsMerge) {
            *resumeStageOut =
                DocumentSourceShardCheckResumability::create(expCtx, tokenData.clusterTime);
        } else {
            *resumeStageOut =
                DocumentSourceEnsureResumeTokenPresent::create(expCtx, std::move(token));
        }
    }

    auto resumeAfterClusterTime = spec.getResumeAfterClusterTimeDeprecated();
    auto startAtClusterTime = spec.getStartAtClusterTime();

    uassert(40674,
            "Only one type of resume option is allowed, but multiple were found.",
            !(*resumeStageOut) || (!resumeAfterClusterTime && !startAtClusterTime));

    if (resumeAfterClusterTime) {
        if (fcv >= ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40) {
            warning() << "The '$_resumeAfterClusterTime' option is deprecated, please use "
                         "'startAtClusterTime' instead.";
        }
        *startFromOut = resumeAfterClusterTime->getTimestamp();
    }

    // New field name starting in 4.0 is 'startAtClusterTime'.
    if (startAtClusterTime) {
        uassert(ErrorCodes::QueryFeatureNotAllowed,
                str::stream() << "The startAtClusterTime option is not allowed in the current "
                                 "feature compatibility version. See "
                              << feature_compatibility_version_documentation::kCompatibilityLink
                              << " for more information.",
                fcv >= ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);
        uassert(50573,
                str::stream()
                    << "Do not specify both "
                    << DocumentSourceChangeStreamSpec::kStartAtClusterTimeFieldName
                    << " and "
                    << DocumentSourceChangeStreamSpec::kResumeAfterClusterTimeDeprecatedFieldName
                    << " in a $changeStream stage.",
                !resumeAfterClusterTime);
        *startFromOut = startAtClusterTime->getTimestamp();
        *resumeStageOut = DocumentSourceShardCheckResumability::create(expCtx, **startFromOut);
    }
}

}  // namespace

list<intrusive_ptr<DocumentSource>> DocumentSourceChangeStream::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    // A change stream is a tailable + awaitData cursor.
    expCtx->tailableMode = TailableModeEnum::kTailableAndAwaitData;

    auto spec = DocumentSourceChangeStreamSpec::parse(IDLParserErrorContext("$changeStream"),
                                                      elem.embeddedObject());

    const auto fcv = serverGlobalParams.featureCompatibility.getVersion();
    // Make sure that it is legal to run this $changeStream before proceeding.
    DocumentSourceChangeStream::assertIsLegalSpecification(expCtx, spec, fcv);

    boost::optional<Timestamp> startFrom;
    intrusive_ptr<DocumentSource> resumeStage = nullptr;
    parseResumeOptions(expCtx, spec, fcv, &resumeStage, &startFrom);

    auto fullDocOption = spec.getFullDocument();
    uassert(40575,
            str::stream() << "unrecognized value for the 'fullDocument' option to the "
                             "$changeStream stage. Expected \"default\" or "
                             "\"updateLookup\", got \""
                          << fullDocOption
                          << "\"",
            fullDocOption == "updateLookup"_sd || fullDocOption == "default"_sd);

    const bool shouldLookupPostImage = (fullDocOption == "updateLookup"_sd);

    list<intrusive_ptr<DocumentSource>> stages;

    // There might not be a starting point if we're on mongos, otherwise we should either have a
    // 'resumeAfter' starting point, or should start from the latest majority committed operation.
    invariant(expCtx->inMongos || static_cast<bool>(startFrom));
    if (startFrom) {
        const bool startFromInclusive = (resumeStage != nullptr);
        stages.push_back(DocumentSourceOplogMatch::create(
            buildMatchFilter(expCtx, *startFrom, startFromInclusive), expCtx));
    }

    stages.push_back(createTransformationStage(expCtx, elem.embeddedObject(), fcv));
    if (resumeStage) {
        stages.push_back(resumeStage);
    }
    if (!expCtx->needsMerge) {
        // There should only be one close cursor stage. If we're on the shards and producing input
        // to be merged, do not add a close cursor stage, since the mongos will already have one.
        stages.push_back(DocumentSourceCloseCursor::create(expCtx));

        // There should be only one post-image lookup stage.  If we're on the shards and producing
        // input to be merged, the lookup is done on the mongos.
        if (shouldLookupPostImage) {
            stages.push_back(DocumentSourceLookupChangePostImage::create(expCtx));
        }
    }
    return stages;
}

BSONObj DocumentSourceChangeStream::replaceResumeTokenInCommand(const BSONObj originalCmdObj,
                                                                const BSONObj resumeToken) {
    Document originalCmd(originalCmdObj);
    auto pipeline = originalCmd[AggregationRequest::kPipelineName].getArray();
    // A $changeStream must be the first element of the pipeline in order to be able
    // to replace (or add) a resume token.
    invariant(!pipeline[0][DocumentSourceChangeStream::kStageName].missing());

    MutableDocument changeStreamStage(
        pipeline[0][DocumentSourceChangeStream::kStageName].getDocument());
    changeStreamStage[DocumentSourceChangeStreamSpec::kResumeAfterFieldName] = Value(resumeToken);

    // If the command was initially specified with a startAtClusterTime, we need to remove it
    // to use the new resume token.
    changeStreamStage[DocumentSourceChangeStreamSpec::kStartAtClusterTimeFieldName] = Value();
    pipeline[0] =
        Value(Document{{DocumentSourceChangeStream::kStageName, changeStreamStage.freeze()}});
    MutableDocument newCmd(originalCmd);
    newCmd[AggregationRequest::kPipelineName] = Value(pipeline);
    return newCmd.freeze().toBson();
}

void DocumentSourceChangeStream::assertIsLegalSpecification(
    const intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec,
    ServerGlobalParams::FeatureCompatibility::Version fcv) {
    // Change stream on an entire database is a new 4.0 feature.
    uassert(ErrorCodes::QueryFeatureNotAllowed,
            str::stream() << "$changeStream on an entire database is not allowed in the current "
                             "feature compatibility version. See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << " for more information.",
            !expCtx->ns.isCollectionlessAggregateNS() ||
                fcv >= ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);

    // If 'allChangesForCluster' is true, the stream must be opened on the 'admin' database with
    // {aggregate: 1}.
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "A $changeStream with 'allChangesForCluster:true' may only be opened "
                             "on the 'admin' database, and with no collection name; found "
                          << expCtx->ns.ns(),
            !spec.getAllChangesForCluster() ||
                (expCtx->ns.isAdminDB() && expCtx->ns.isCollectionlessAggregateNS()));

    // Prevent $changeStream from running on internal databases. A stream may run against the
    // 'admin' database iff 'allChangesForCluster' is true.
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "$changeStream may not be opened on the internal " << expCtx->ns.db()
                          << " database",
            expCtx->ns.isAdminDB() ? spec.getAllChangesForCluster()
                                   : (!expCtx->ns.isLocal() && !expCtx->ns.isConfigDB()));

    // Prevent $changeStream from running on internal collections in any database.
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "$changeStream may not be opened on the internal " << expCtx->ns.ns()
                          << " collection",
            !expCtx->ns.isSystem());
}

intrusive_ptr<DocumentSource> DocumentSourceChangeStream::createTransformationStage(
    const intrusive_ptr<ExpressionContext>& expCtx,
    BSONObj changeStreamSpec,
    ServerGlobalParams::FeatureCompatibility::Version fcv) {
    // Mark the transformation stage as independent of any collection if the change stream is
    // watching all collections in the database.
    const bool isIndependentOfAnyCollection = expCtx->ns.isCollectionlessAggregateNS();
    return intrusive_ptr<DocumentSource>(new DocumentSourceChangeStreamTransform(
        expCtx, changeStreamSpec, fcv, isIndependentOfAnyCollection));
}
}  // namespace mongo

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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/hex.h"

namespace mongo {
namespace {

// Always notifies prepare conflict waiters when a transaction commits or aborts, even when the
// transaction is not prepared. This should always be enabled if WTPrepareConflictForReads is
// used, which fails randomly. If this is not enabled, no prepare conflicts will be resolved,
// because the recovery unit may not ever actually be in a prepared state.
MONGO_FAIL_POINT_DEFINE(WTAlwaysNotifyPrepareConflictWaiters);

logv2::LogSeverity kSlowTransactionSeverity = logv2::LogSeverity::Debug(1);

MONGO_FAIL_POINT_DEFINE(doUntimestampedWritesForIdempotencyTests);

}  // namespace

using Section = WiredTigerOperationStats::Section;

//慢日志中的"storage":{"data":{"bytesRead":3679,"timeReadingMicros":58}}
std::map<int, std::pair<StringData, Section>> WiredTigerOperationStats::_statNameMap = {
    {WT_STAT_SESSION_BYTES_READ, std::make_pair("bytesRead"_sd, Section::DATA)},
    {WT_STAT_SESSION_BYTES_WRITE, std::make_pair("bytesWritten"_sd, Section::DATA)},
    {WT_STAT_SESSION_LOCK_DHANDLE_WAIT, std::make_pair("handleLock"_sd, Section::WAIT)},
    {WT_STAT_SESSION_READ_TIME, std::make_pair("timeReadingMicros"_sd, Section::DATA)},
    {WT_STAT_SESSION_WRITE_TIME, std::make_pair("timeWritingMicros"_sd, Section::DATA)},
    {WT_STAT_SESSION_LOCK_SCHEMA_WAIT, std::make_pair("schemaLock"_sd, Section::WAIT)},
    {WT_STAT_SESSION_CACHE_TIME, std::make_pair("cache"_sd, Section::WAIT)}};

std::shared_ptr<StorageStats> WiredTigerOperationStats::getCopy() {
    std::shared_ptr<WiredTigerOperationStats> copy = std::make_shared<WiredTigerOperationStats>();
    *copy += *this;
    return copy;
}

//WiredTigerRecoveryUnit::getOperationStatistics() 
//获取存储引擎统计信息
void WiredTigerOperationStats::fetchStats(WT_SESSION* session,
                                          const std::string& uri,
                                          const std::string& config) {
    invariant(session);

    WT_CURSOR* c = nullptr;
    const char* cursorConfig = config.empty() ? nullptr : config.c_str();
    int ret = session->open_cursor(session, uri.c_str(), nullptr, cursorConfig, &c);
    uassert(ErrorCodes::CursorNotFound, "Unable to open statistics cursor", ret == 0);

    invariant(c);
    ON_BLOCK_EXIT([&] { c->close(c); });

    const char* desc;
    uint64_t value;
    int32_t key;
    while (c->next(c) == 0 && c->get_key(c, &key) == 0) {
        fassert(51035, c->get_value(c, &desc, nullptr, &value) == 0);
        _stats[key] = WiredTigerUtil::castStatisticsValue<long long>(value);
    }

    // Reset the statistics so that the next fetch gives the recent values.
    invariantWTOK(c->reset(c));
}

//这个是慢日志打印相关的信息
//慢日志中的"storage":{"data":{"bytesRead":3679,"timeReadingMicros":58}}
BSONObj WiredTigerOperationStats::toBSON() {
    BSONObjBuilder bob;
    std::unique_ptr<BSONObjBuilder> dataSection;
    std::unique_ptr<BSONObjBuilder> waitSection;

	//_stats来源上面的WiredTigerOperationStats::fetchStats
    for (auto const& stat : _stats) {
        // Find the user consumable name for this statistic.
        auto statIt = _statNameMap.find(stat.first);
        invariant(statIt != _statNameMap.end());

        auto statName = statIt->second.first;
        Section subs = statIt->second.second;
        long long val = stat.second;
        // Add this statistic only if higher than zero.
        if (val > 0) {
            // Gather the statistic into its own subsection in the BSONObj.
            switch (subs) {
                case Section::DATA:
                    if (!dataSection)
                        dataSection = std::make_unique<BSONObjBuilder>();

                    dataSection->append(statName, val);
                    break;
                case Section::WAIT:
                    if (!waitSection)
                        waitSection = std::make_unique<BSONObjBuilder>();

                    waitSection->append(statName, val);
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }

    if (dataSection)
        bob.append("data", dataSection->obj());
    if (waitSection)
        bob.append("timeWaitingMicros", waitSection->obj());

    return bob.obj();
}

WiredTigerOperationStats& WiredTigerOperationStats::operator+=(
    const WiredTigerOperationStats& other) {
    for (auto const& otherStat : other._stats) {
        _stats[otherStat.first] += otherStat.second;
    }
    return (*this);
}

StorageStats& WiredTigerOperationStats::operator+=(const StorageStats& other) {
    *this += checked_cast<const WiredTigerOperationStats&>(other);
    return (*this);
}

WiredTigerRecoveryUnit::WiredTigerRecoveryUnit(WiredTigerSessionCache* sc)
    : WiredTigerRecoveryUnit(sc, sc->getKVEngine()->getOplogManager()) {}

WiredTigerRecoveryUnit::WiredTigerRecoveryUnit(WiredTigerSessionCache* sc,
                                               WiredTigerOplogManager* oplogManager)
    : _sessionCache(sc), _oplogManager(oplogManager) {}

WiredTigerRecoveryUnit::~WiredTigerRecoveryUnit() {
    invariant(!_inUnitOfWork(), toString(_getState()));
    _abort();
}

//WiredTigerRecoveryUnit::doCommitUnitOfWork() 
//事务提交
void WiredTigerRecoveryUnit::_commit() {
    // Since we cannot have both a _lastTimestampSet and a _commitTimestamp, we set the
    // commit time as whichever is non-empty. If both are empty, then _lastTimestampSet will
    // be boost::none and we'll set the commit time to that.
    auto commitTime = _commitTimestamp.isNull() ? _lastTimestampSet : _commitTimestamp;

    bool notifyDone = !_prepareTimestamp.isNull();
    if (_session && _isActive()) {
		//事务提交
        _txnClose(true);
    }
    _setState(State::kCommitting);

    if (MONGO_unlikely(WTAlwaysNotifyPrepareConflictWaiters.shouldFail())) {
        notifyDone = true;
    }

    if (notifyDone) {
        _sessionCache->notifyPreparedUnitOfWorkHasCommittedOrAborted();
    }

    commitRegisteredChanges(commitTime);
    _setState(State::kInactive);
}

//事务回滚
void WiredTigerRecoveryUnit::_abort() {
    bool notifyDone = !_prepareTimestamp.isNull();
    if (_session && _isActive()) {
        _txnClose(false);
    }
    _setState(State::kAborting);

    if (notifyDone || MONGO_unlikely(WTAlwaysNotifyPrepareConflictWaiters.shouldFail())) {
        _sessionCache->notifyPreparedUnitOfWorkHasCommittedOrAborted();
    }

    abortRegisteredChanges();
    _setState(State::kInactive);
}

//WriteUnitOfWork::WriteUnitOfWork
void WiredTigerRecoveryUnit::beginUnitOfWork(OperationContext* opCtx) {
    invariant(!_inUnitOfWork(), toString(_getState()));
    invariant(!_isCommittingOrAborting(),
              str::stream() << "cannot begin unit of work while commit or rollback handlers are "
                               "running: "
                            << toString(_getState()));
    _setState(_isActive() ? State::kActive : State::kInactiveInUnitOfWork);
}

//WriteUnitOfWork::prepare()
void WiredTigerRecoveryUnit::prepareUnitOfWork() {
    invariant(_inUnitOfWork(), toString(_getState()));
    invariant(!_prepareTimestamp.isNull());

    auto session = getSession();
    WT_SESSION* s = session->getSession();

    LOGV2_DEBUG(22410,
                1,
                "preparing transaction at time: {prepareTimestamp}",
                "prepareTimestamp"_attr = _prepareTimestamp);

    const std::string conf = "prepare_timestamp=" + integerToHex(_prepareTimestamp.asULL());
    // Prepare the transaction.
    invariantWTOK(s->prepare_transaction(s, conf.c_str()));
}

//WriteUnitOfWork::commit()->RecoveryUnit::commitUnitOfWork->WiredTigerRecoveryUnit::doCommitUnitOfWork()调用
//事务提交
void WiredTigerRecoveryUnit::doCommitUnitOfWork() {
    invariant(_inUnitOfWork(), toString(_getState()));
    _commit();
}

//事务回滚
void WiredTigerRecoveryUnit::doAbortUnitOfWork() {
    invariant(_inUnitOfWork(), toString(_getState()));
    _abort();
}

void WiredTigerRecoveryUnit::_ensureSession() {
    if (!_session) {
        _session = _sessionCache->getSession();
    }
}

//持久化
bool WiredTigerRecoveryUnit::waitUntilDurable(OperationContext* opCtx) {
    invariant(!_inUnitOfWork(), toString(_getState()));
    invariant(!opCtx->lockState()->isLocked() || storageGlobalParams.repair);

    // Flushes the journal log to disk. Checkpoints all data if journaling is disabled.
    //WiredTigerSessionCache::waitUntilDurable  
    _sessionCache->waitUntilDurable(opCtx,
                                    WiredTigerSessionCache::Fsync::kJournal,
                                    WiredTigerSessionCache::UseJournalListener::kUpdate);

	LOGV2_DEBUG(122418,
        5,
        "yang test .......WiredTigerRecoveryUnit::waitUntilDurable");

    return true;
}

//参考https://mongoing.com/archives/77853   Recover To Timestamp Rollback
bool WiredTigerRecoveryUnit::waitUntilUnjournaledWritesDurable(OperationContext* opCtx,
                                                               bool stableCheckpoint) {
    invariant(!_inUnitOfWork(), toString(_getState()));
    invariant(!opCtx->lockState()->isLocked() || storageGlobalParams.repair);

    // Take a checkpoint, rather than only flush the (oplog) journal, in order to lock in stable
    // writes to unjournaled tables.
    //
    // If 'stableCheckpoint' is set, then we will only checkpoint data up to and including the
    // stable_timestamp set on WT at the time of the checkpoint. Otherwise, we will checkpoint all
    // of the data.
    WiredTigerSessionCache::Fsync fsyncType = stableCheckpoint
        ? WiredTigerSessionCache::Fsync::kCheckpointStableTimestamp
        : WiredTigerSessionCache::Fsync::kCheckpointAll;
	//WiredTigerSessionCache::waitUntilDurable  
    _sessionCache->waitUntilDurable(
        opCtx, fsyncType, WiredTigerSessionCache::UseJournalListener::kUpdate);


	LOGV2_DEBUG(122418,
        5,
        "yang test .......WiredTigerRecoveryUnit::waitUntilUnjournaledWritesDurable");

    return true;
}

void WiredTigerRecoveryUnit::assertInActiveTxn() const {
    if (_isActive()) {
        return;
    }
    LOGV2_FATAL(28575,
                "Recovery unit is not active. Current state: {currentState}",
                "Recovery unit is not active.",
                "currentState"_attr = _getState());
}

boost::optional<int64_t> WiredTigerRecoveryUnit::getOplogVisibilityTs() {
    if (!_isOplogReader) {
        return boost::none;
    }

    getSession();
    return _oplogVisibleTs;
}

//查询流程AutoGetCollectionForRead::AutoGetCollectionForRead-> opCtx->recoveryUnit()->getPointInTimeReadTimestamp() 
//  -> WiredTigerRecoveryUnit::getPointInTimeReadTimestamp() ->WiredTigerRecoveryUnit::getSession()


//写:insertDocuments->oplog.cpp中的getNextOpTimes->LocalOplogInfo::getNextOpTimes->WiredTigerRecoveryUnit::preallocateSnapshot()

// Begin a new transaction, if one is not already started. //事务begin_transaction封装
WiredTigerSession* WiredTigerRecoveryUnit::getSession() {
    if (!_isActive()) {
        _txnOpen();
        _setState(_inUnitOfWork() ? State::kActive : State::kActiveNotInUnitOfWork);
    }
    return _session.get();
}

WiredTigerSession* WiredTigerRecoveryUnit::getSessionNoTxn() {
    _ensureSession();
    WiredTigerSession* session = _session.get();

    // Handling queued drops can be slow, which is not desired for internal operations like FTDC
    // sampling. Disable handling of queued drops for such sessions.
    session->dropQueuedIdentsAtSessionEndAllowed(false);
    return session;
}

//recover_unit.h中的abandonSnapshot()调用 
//释放掉该快照，可以参考下https://mongoing.com/archives/5476
void WiredTigerRecoveryUnit::doAbandonSnapshot() {
    invariant(!_inUnitOfWork(), toString(_getState()));
    if (_isActive()) {
        // Can't be in a WriteUnitOfWork, so safe to rollback
        _txnClose(false);
    }
    _setState(State::kInactive);
}

//insertDocuments->oplog.cpp中的getNextOpTimes->LocalOplogInfo::getNextOpTimes->WiredTigerRecoveryUnit::preallocateSnapshot()
//LocalOplogInfo::getNextOpTimes
void WiredTigerRecoveryUnit::preallocateSnapshot() {
    // Begin a new transaction, if one is not already started.
    getSession(); //事务begin_transaction封装
}

void WiredTigerRecoveryUnit::refreshSnapshot() {
    // First, start a new transaction at the same timestamp as the current one.  Then end the
    // current transaction.  This overlap will prevent WT from cleaning up history required to serve
    // the read timestamp.

    // Currently, this code only works for kNoOverlap or kNoTimestamp.
    invariant(_timestampReadSource == ReadSource::kNoOverlap ||
              _timestampReadSource == ReadSource::kNoTimestamp);
    invariant(_isActive());
    invariant(!_inUnitOfWork());
    invariant(!_noEvictionAfterRollback);

    auto newSession = _sessionCache->getSession();
    WiredTigerBeginTxnBlock txnOpen(newSession->getSession(),
                                    _prepareConflictBehavior,
                                    _roundUpPreparedTimestamps,
                                    RoundUpReadTimestamp::kNoRoundForce);
    if (_timestampReadSource != ReadSource::kNoTimestamp) {
        auto status = txnOpen.setReadSnapshot(_readAtTimestamp);
        fassert(5035300, status);
    }
    txnOpen.done();

    // Now end the previous transaction.
    auto wtSession = _session->getSession();
    auto wtRet = wtSession->rollback_transaction(wtSession, nullptr);
    invariantWTOK(wtRet);
    LOGV2_DEBUG(5035301,
                3,
                "WT begin_transaction & rollback_transaction",
                "snapshotId"_attr = getSnapshotId().toNumber());

    _session = std::move(newSession);
}


//WiredTigerRecoveryUnit::_commit(true)  WiredTigerRecoveryUnit::_abort(false)  
//WiredTigerRecoveryUnit::doAbandonSnapshot(false)
void WiredTigerRecoveryUnit::_txnClose(bool commit) {//事务提交或者事务回滚在这里面
    invariant(_isActive(), toString(_getState()));
    WT_SESSION* s = _session->getSession();
    if (_timer) { //定时器把事务满操作慢日志记录下来
        const int transactionTime = _timer->millis();
        // `serverGlobalParams.slowMs` can be set to values <= 0. In those cases, give logging a
        // break.
        if (transactionTime >= std::max(1, serverGlobalParams.slowMS)) {
            LOGV2_DEBUG(22411,
                        kSlowTransactionSeverity.toInt(),
                        "Slow WT transaction. Lifetime of SnapshotId {getSnapshotId_toNumber} was "
                        "{transactionTime}ms",
                        "getSnapshotId_toNumber"_attr = getSnapshotId().toNumber(),
                        "transactionTime"_attr = transactionTime);
        }
    }

    int wtRet;
    if (commit) {
		//事务提交，注意这里，如果_commitTimestamp不为null，并且_durableTimestamp不为null，则conf会为空，这样就实现了引擎层timestamp对应快照的释放
        StringBuilder conf;
        if (!_commitTimestamp.isNull()) {
            // There is currently no scenario where it is intentional to commit before the current
            // read timestamp.
            invariant(_readAtTimestamp.isNull() || _commitTimestamp >= _readAtTimestamp);

            if (MONGO_likely(!doUntimestampedWritesForIdempotencyTests.shouldFail())) {
                conf << "commit_timestamp=" << integerToHex(_commitTimestamp.asULL()) << ",";
            }
            _isTimestamped = true;
        }

        if (!_durableTimestamp.isNull()) {
            conf << "durable_timestamp=" << integerToHex(_durableTimestamp.asULL());
        }

        if (_mustBeTimestamped) {
            invariant(_isTimestamped);
        }

        wtRet = s->commit_transaction(s, conf.str().c_str());

        LOGV2_DEBUG(22412,
                    3,
                    "WT commit_transaction for snapshot id {snapshotId}",
                    "WT commit_transaction",
                    "snapshotId"_attr = getSnapshotId().toNumber());
    } else {//事务回滚
        StringBuilder config;
        if (_noEvictionAfterRollback) {
            // The only point at which rollback_transaction() can time out is in the bonus-eviction
            // phase. If the timeout expires here, the function will stop the eviction and return
            // success. It cannot return an error due to timeout.
            config << "operation_timeout_ms=1,";
        }

        wtRet = s->rollback_transaction(s, config.str().c_str());

        LOGV2_DEBUG(22413,
                    3,
                    "WT rollback_transaction for snapshot id {snapshotId}",
                    "WT rollback_transaction",
                    "snapshotId"_attr = getSnapshotId().toNumber());
    }

    if (_isTimestamped) {
        if (!_orderedCommit) {
            // We only need to update oplog visibility where commits can be out-of-order with
            // respect to their assigned optime. This will ensure the oplog read timestamp gets
            // updated when oplog 'holes' are filled: the last commit filling the last hole will
            // prompt the oplog read timestamp to be forwarded.
            //
            // This should happen only on primary nodes.
            _oplogManager->triggerOplogVisibilityUpdate();
        }
        _isTimestamped = false;
    }
    invariantWTOK(wtRet);

    invariant(!_lastTimestampSet || _commitTimestamp.isNull(),
              str::stream() << "Cannot have both a _lastTimestampSet and a "
                               "_commitTimestamp. _lastTimestampSet: "
                            << _lastTimestampSet->toString()
                            << ". _commitTimestamp: " << _commitTimestamp.toString());

    // We reset the _lastTimestampSet between transactions. Since it is legal for one
    // transaction on a RecoveryUnit to call setTimestamp() and another to call
    // setCommitTimestamp().
    _lastTimestampSet = boost::none;

    _prepareTimestamp = Timestamp();
    _durableTimestamp = Timestamp();
    _catalogConflictTimestamp = Timestamp();
    _roundUpPreparedTimestamps = RoundUpPreparedTimestamps::kNoRound;
    _isOplogReader = false;
    _oplogVisibleTs = boost::none;
    _orderedCommit = true;  // Default value is true; we assume all writes are ordered.
    _mustBeTimestamped = false;
}

//waitForReadConcernImpl    getMore    applyCursorReadConcern
Status WiredTigerRecoveryUnit::obtainMajorityCommittedSnapshot() {
    invariant(_timestampReadSource == ReadSource::kMajorityCommitted);
    auto snapshotName = _sessionCache->snapshotManager().getMinSnapshotForNextCommittedRead();
    if (!snapshotName) {
        return {ErrorCodes::ReadConcernMajorityNotAvailableYet,
                "Read concern majority reads are currently not possible."};
    }
    _majorityCommittedSnapshot = *snapshotName;
    return Status::OK();
}

//注意这里面会启用事务WT begin_transaction
boost::optional<Timestamp> WiredTigerRecoveryUnit::getPointInTimeReadTimestamp() {
	LOGV2_DEBUG(122418,
        5,
        "yang test .......WiredTigerRecoveryUnit::getPointInTimeReadTimestamp");

    // After a ReadSource has been set on this RecoveryUnit, callers expect that this method returns
    // the read timestamp that will be used for current or future transactions. Because callers use
    // this timestamp to inform visiblity of operations, it is therefore necessary to open a
    // transaction to establish a read timestamp, but only for ReadSources that are expected to have
    // read timestamps.
    switch (_timestampReadSource) {
        case ReadSource::kNoTimestamp:
            return boost::none;
        case ReadSource::kMajorityCommitted:
            // This ReadSource depends on a previous call to obtainMajorityCommittedSnapshot() and
            // does not require an open transaction to return a valid timestamp.
            invariant(!_majorityCommittedSnapshot.isNull());
            return _majorityCommittedSnapshot;
        case ReadSource::kProvided:
            // The read timestamp is set by the user and does not require a transaction to be open.
            invariant(!_readAtTimestamp.isNull());
            return _readAtTimestamp;

        // The following ReadSources can only establish a read timestamp when a transaction is
        // opened.
        case ReadSource::kNoOverlap:
        case ReadSource::kLastApplied:
        case ReadSource::kAllDurableSnapshot:
            break;
    }

    // Ensure a transaction is opened.
    //注意这里面会启用事务WT begin_transaction
    getSession();

    switch (_timestampReadSource) {
        case ReadSource::kLastApplied:
        case ReadSource::kNoOverlap:
            // The lastApplied and allDurable timestamps are not always available if the system has
            // not accepted writes, so it is not possible to invariant that it exists as other
            // ReadSources do.
            if (!_readAtTimestamp.isNull()) {
                return _readAtTimestamp;
            }
            return boost::none;
        case ReadSource::kAllDurableSnapshot:
            invariant(!_readAtTimestamp.isNull());
            return _readAtTimestamp;

        // The follow ReadSources returned values in the first switch block.
        case ReadSource::kNoTimestamp:
        case ReadSource::kMajorityCommitted:
        case ReadSource::kProvided:
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

//WiredTigerRecoveryUnit::getSession()  
//事务begin_transaction封装       
void WiredTigerRecoveryUnit::_txnOpen() { //也就是对应begin_transaction
    invariant(!_isActive(), toString(_getState()));
    invariant(!_isCommittingOrAborting(),
              str::stream() << "commit or rollback handler reopened transaction: "
                            << toString(_getState()));
    _ensureSession();

    // Only start a timer for transaction's lifetime if we're going to log it.
    if (shouldLog(kSlowTransactionSeverity)) {
        _timer.reset(new Timer());
    }
    WT_SESSION* session = _session->getSession();

    switch (_timestampReadSource) {
        case ReadSource::kNoTimestamp: {
            if (_isOplogReader) {
                _oplogVisibleTs = static_cast<std::int64_t>(_oplogManager->getOplogReadTimestamp());
            }
			//WiredTigerBeginTxnBlock::WiredTigerBeginTxnBlock
			//WiredTigerBeginTxnBlock::done()
			//事务begin_transaction封装
            WiredTigerBeginTxnBlock(session, _prepareConflictBehavior, _roundUpPreparedTimestamps)
                .done();
            break;
        }
        case ReadSource::kMajorityCommitted: {
            // We reset _majorityCommittedSnapshot to the actual read timestamp used when the
            // transaction was started.
            _majorityCommittedSnapshot =
                _sessionCache->snapshotManager().beginTransactionOnCommittedSnapshot(
                    session, _prepareConflictBehavior, _roundUpPreparedTimestamps);
            break;
        }
        case ReadSource::kLastApplied: {
            _readAtTimestamp = _beginTransactionAtLastAppliedTimestamp(session);
            break;
        }
        case ReadSource::kNoOverlap: {
            _readAtTimestamp = _beginTransactionAtNoOverlapTimestamp(session);
            break;
        }
        case ReadSource::kAllDurableSnapshot: {
            if (_readAtTimestamp.isNull()) {
                _readAtTimestamp = _beginTransactionAtAllDurableTimestamp(session);
                break;
            }
            // Intentionally continue to the next case to read at the _readAtTimestamp.
        }
        case ReadSource::kProvided: {
            WiredTigerBeginTxnBlock txnOpen(
                session, _prepareConflictBehavior, _roundUpPreparedTimestamps);
            auto status = txnOpen.setReadSnapshot(_readAtTimestamp);

            if (!status.isOK() && status.code() == ErrorCodes::BadValue) {
                uasserted(ErrorCodes::SnapshotTooOld,
                          str::stream() << "Read timestamp " << _readAtTimestamp.toString()
                                        << " is older than the oldest available timestamp.");
            }
            uassertStatusOK(status);
            txnOpen.done();
            break;
        }
    }

    LOGV2_DEBUG(22414,
                3,
                "WT begin_transaction",
                "snapshotId"_attr = getSnapshotId().toNumber(),
                "readSource"_attr = toString(_timestampReadSource));
}



Timestamp WiredTigerRecoveryUnit::_beginTransactionAtAllDurableTimestamp(WT_SESSION* session) {
    WiredTigerBeginTxnBlock txnOpen(session,
                                    _prepareConflictBehavior,
                                    _roundUpPreparedTimestamps,
                                    RoundUpReadTimestamp::kRound);
    Timestamp txnTimestamp = _sessionCache->getKVEngine()->getAllDurableTimestamp();
    auto status = txnOpen.setReadSnapshot(txnTimestamp);
    fassert(50948, status);

    // Since this is not in a critical section, we might have rounded to oldest between
    // calling getAllDurable and setReadSnapshot.  We need to get the actual read timestamp we
    // used.
    auto readTimestamp = _getTransactionReadTimestamp(session);
    txnOpen.done();

	LOGV2_DEBUG(122418,
        5,
        "yang test .......WiredTigerRecoveryUnit::_beginTransactionAtAllDurableTimestamp");
    return readTimestamp;
}

namespace {
/**
 * This mutex serializes starting read transactions at lastApplied. This throttles new transactions
 * so they do not overwhelm the WiredTiger spinlock that manages the global read timestamp queue.
 * Because this queue can grow larger than the number of active transactions, the MongoDB ticketing
 * system alone cannot restrict its growth and thus bound the amount of time the queue is locked.
 * TODO: WT-6709
 */
Mutex _lastAppliedTxnMutex = MONGO_MAKE_LATCH("lastAppliedTxnMutex");
}  // namespace

Timestamp WiredTigerRecoveryUnit::_beginTransactionAtLastAppliedTimestamp(WT_SESSION* session) {
    auto lastApplied = _sessionCache->snapshotManager().getLastApplied();
    if (!lastApplied) {
        // When there is not a lastApplied timestamp available, read without a timestamp. Do not
        // round up the read timestamp to the oldest timestamp.

        // There is a race that allows new transactions to start between the time we check for a
        // read timestamp and start our transaction, which can temporarily violate the contract of
        // kLastApplied. That is, writes will be visible that occur after the lastApplied time. This
        // is only possible for readers that start immediately after an initial sync that did not
        // replicate any oplog entries. Future transactions will start reading at a timestamp once
        // timestamped writes have been made.
        WiredTigerBeginTxnBlock txnOpen(
            session, _prepareConflictBehavior, _roundUpPreparedTimestamps);
        LOGV2_DEBUG(4847500, 2, "no read timestamp available for kLastApplied");
        txnOpen.done();
        return Timestamp();
    }

    {
        stdx::lock_guard<Mutex> lock(_lastAppliedTxnMutex);
        WiredTigerBeginTxnBlock txnOpen(session,
                                        _prepareConflictBehavior,
                                        _roundUpPreparedTimestamps,
                                        RoundUpReadTimestamp::kRound);
        auto status = txnOpen.setReadSnapshot(*lastApplied);
        fassert(4847501, status);
        txnOpen.done();
    }

	LOGV2_DEBUG(122418,
        5,
        "yang test .......WiredTigerRecoveryUnit::_beginTransactionAtLastAppliedTimestamp");
    // We might have rounded to oldest between calling getLastApplied and setReadSnapshot. We
    // need to get the actual read timestamp we used.
    return _getTransactionReadTimestamp(session);
}

Timestamp WiredTigerRecoveryUnit::_beginTransactionAtNoOverlapTimestamp(WT_SESSION* session) {

    auto lastApplied = _sessionCache->snapshotManager().getLastApplied();
    Timestamp allDurable = Timestamp(_sessionCache->getKVEngine()->getAllDurableTimestamp());

    // When using timestamps for reads and writes, it's important that readers and writers don't
    // overlap with the timestamps they use. In other words, at any point in the system there should
    // be a timestamp T such that writers only commit at times greater than T and readers only read
    // at, or earlier than T. This time T is called the no-overlap point. Using the `kNoOverlap`
    // ReadSource will compute the most recent known time that is safe to read at.

    // The no-overlap point is computed as the minimum of the storage engine's all_durable time
    // and replication's last applied time. On primaries, the last applied time is updated as
    // transactions commit, which is not necessarily in the order they appear in the oplog. Thus
    // the all_durable time is an appropriate value to read at.

    // On secondaries, however, the all_durable time, as computed by the storage engine, can
    // advance before oplog application completes a batch. This is because the all_durable time
    // is only computed correctly if the storage engine is informed of commit timestamps in
    // increasing order. Because oplog application processes a batch of oplog entries out of order,
    // the timestamping requirement is not satisfied. Secondaries, however, only update the last
    // applied time after a batch completes. Thus last applied is a valid no-overlap point on
    // secondaries.

    // By taking the minimum of the two values, storage can compute a legal time to read at without
    // knowledge of the replication state. The no-overlap point is the minimum of the all_durable
    // time, which represents the point where no transactions will commit any earlier, and
    // lastApplied, which represents the highest optime a node has applied, a point no readers
    // should read afterward.
    Timestamp readTimestamp = (lastApplied) ? std::min(*lastApplied, allDurable) : allDurable;

    if (readTimestamp.isNull()) {
        // When there is not an all_durable or lastApplied timestamp available, read without a
        // timestamp. Do not round up the read timestamp to the oldest timestamp.

        // There is a race that allows new transactions to start between the time we check for a
        // read timestamp and start our transaction, which can temporarily violate the contract of
        // kNoOverlap. That is, writes will be visible that occur after the all_durable time. This
        // is only possible for readers that start immediately after an initial sync that did not
        // replicate any oplog entries. Future transactions will start reading at a timestamp once
        // timestamped writes have been made.
        WiredTigerBeginTxnBlock txnOpen(
            session, _prepareConflictBehavior, _roundUpPreparedTimestamps);
        LOGV2_DEBUG(4452900, 1, "no read timestamp available for kNoOverlap");
        txnOpen.done();
        return readTimestamp;
    }

    WiredTigerBeginTxnBlock txnOpen(session,
                                    _prepareConflictBehavior,
                                    _roundUpPreparedTimestamps,
                                    RoundUpReadTimestamp::kRound);
    auto status = txnOpen.setReadSnapshot(readTimestamp);
    fassert(51066, status);
    txnOpen.done();

    // We might have rounded to oldest between calling getAllDurable and setReadSnapshot. We
    // need to get the actual read timestamp we used.
    readTimestamp = _getTransactionReadTimestamp(session);
	LOGV2_DEBUG(122418,
        5,
        "yang test .......WiredTigerRecoveryUnit::_beginTransactionAtNoOverlapTimestamp");

    return readTimestamp;
}

Timestamp WiredTigerRecoveryUnit::_getTransactionReadTimestamp(WT_SESSION* session) {
	LOGV2_DEBUG(122418,
        5,
        "yang test .......WiredTigerRecoveryUnit::_getTransactionReadTimestamp");

    char buf[(2 * 8 /*bytes in hex*/) + 1 /*nul terminator*/];
    auto wtstatus = session->query_timestamp(session, buf, "get=read");
    invariantWTOK(wtstatus);
    uint64_t read_timestamp;
    fassert(50949, NumberParser().base(16)(buf, &read_timestamp));
    return Timestamp(read_timestamp);
}

//session.commitTransaction();的时候会调用
//WiredTigerRecordStore::_insertRecords   WiredTigerRecordStore::oplogDiskLocRegister 
Status WiredTigerRecoveryUnit::setTimestamp(Timestamp timestamp) {
    _ensureSession();
    LOGV2_DEBUG(22415,
                3,
                "WT set timestamp of future write operations to {timestamp}",
                "timestamp"_attr = timestamp);
    WT_SESSION* session = _session->getSession();
    invariant(_inUnitOfWork(), toString(_getState()));
    invariant(_prepareTimestamp.isNull());
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp set to " << _commitTimestamp.toString()
                            << " and trying to set WUOW timestamp to " << timestamp.toString());
    invariant(_readAtTimestamp.isNull() || timestamp >= _readAtTimestamp,
              str::stream() << "future commit timestamp " << timestamp.toString()
                            << " cannot be older than read timestamp "
                            << _readAtTimestamp.toString());

    _lastTimestampSet = timestamp;

    // Starts the WT transaction associated with this session.
    getSession();

    if (MONGO_unlikely(doUntimestampedWritesForIdempotencyTests.shouldFail())) {
        _isTimestamped = true;
        return Status::OK();
    }

    const std::string conf = "commit_timestamp=" + integerToHex(timestamp.asULL());
    auto rc = session->timestamp_transaction(session, conf.c_str());
    if (rc == 0) {
        _isTimestamped = true;
    }
    return wtRCToStatus(rc, "timestamp_transaction");
}

//StorageEngineImpl::_dropCollectionsNoTimestamp  TimestampBlock::TimestampBlock
//TransactionParticipant::Participant::commitPreparedTransaction
void WiredTigerRecoveryUnit::setCommitTimestamp(Timestamp timestamp) {
    // This can be called either outside of a WriteUnitOfWork or in a prepared transaction after
    // setPrepareTimestamp() is called. Prepared transactions ensure the correct timestamping
    // semantics and the set-once commitTimestamp behavior is exactly what prepared transactions
    // want.
    invariant(!_inUnitOfWork() || !_prepareTimestamp.isNull(), toString(_getState()));
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp set to " << _commitTimestamp.toString()
                            << " and trying to set it to " << timestamp.toString());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to set commit timestamp to " << timestamp.toString());
    invariant(!_isTimestamped);

    _commitTimestamp = timestamp;
}

//上面的WiredTigerRecoveryUnit::_txnClose  WiredTigerRecoveryUnit::setTimestamp调用WT存储引擎接口使用
Timestamp WiredTigerRecoveryUnit::getCommitTimestamp() const {
    return _commitTimestamp;
}


//上面得WiredTigerRecoveryUnit::_txnClose  WiredTigerRecoveryUnit::setTimestamp调用WT存储引擎接口使用
void WiredTigerRecoveryUnit::setDurableTimestamp(Timestamp timestamp) {
    invariant(
        _durableTimestamp.isNull(),
        str::stream() << "Trying to reset durable timestamp when it was already set. wasSetTo: "
                      << _durableTimestamp.toString() << " setTo: " << timestamp.toString());

    _durableTimestamp = timestamp;
}

Timestamp WiredTigerRecoveryUnit::getDurableTimestamp() const {
    return _durableTimestamp;
}

void WiredTigerRecoveryUnit::clearCommitTimestamp() {
    invariant(!_inUnitOfWork(), toString(_getState()));
    invariant(!_commitTimestamp.isNull());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to clear commit timestamp.");
    invariant(!_isTimestamped);

    _commitTimestamp = Timestamp();
}

//上面的WiredTigerRecoveryUnit::prepareUnitOfWork()使用
void WiredTigerRecoveryUnit::setPrepareTimestamp(Timestamp timestamp) {
    invariant(_inUnitOfWork(), toString(_getState()));
    invariant(_prepareTimestamp.isNull(),
              str::stream() << "Trying to set prepare timestamp to " << timestamp.toString()
                            << ". It's already set to " << _prepareTimestamp.toString());
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp is " << _commitTimestamp.toString()
                            << " and trying to set prepare timestamp to " << timestamp.toString());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to set prepare timestamp to " << timestamp.toString());

    _prepareTimestamp = timestamp;
}

Timestamp WiredTigerRecoveryUnit::getPrepareTimestamp() const {
    invariant(_inUnitOfWork(), toString(_getState()));
    invariant(!_prepareTimestamp.isNull());
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp is " << _commitTimestamp.toString()
                            << " and trying to get prepare timestamp of "
                            << _prepareTimestamp.toString());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to get prepare timestamp of "
                            << _prepareTimestamp.toString());

    return _prepareTimestamp;
}

//WiredTigerRecoveryUnit::refreshSnapshot()  WiredTigerRecoveryUnit::_txnOpen() _beginTransactionAtAllDurableTimestamp等使用
void WiredTigerRecoveryUnit::setPrepareConflictBehavior(PrepareConflictBehavior behavior) {
    // If there is an open storage transaction, it is not valid to try to change the behavior of
    // ignoring prepare conflicts, since that behavior is applied when the transaction is opened.
    invariant(
        !_isActive(),
        str::stream() << "Current state: " << toString(_getState())
                      << ". Invalid internal state while setting prepare conflict behavior to: "
                      << static_cast<int>(behavior));

    _prepareConflictBehavior = behavior;
}

PrepareConflictBehavior WiredTigerRecoveryUnit::getPrepareConflictBehavior() const {
    return _prepareConflictBehavior;
}

//
//上面的WiredTigerRecoveryUnit::refreshSnapshot()  WiredTigerRecoveryUnit::_txnOpen() _beginTransactionAtLastAppliedTimestamp
void WiredTigerRecoveryUnit::setRoundUpPreparedTimestamps(bool value) {
    // This cannot be called after WiredTigerRecoveryUnit::_txnOpen.
    invariant(!_isActive(),
              str::stream() << "Can't change round up prepared timestamps flag "
                            << "when current state is " << toString(_getState()));
    _roundUpPreparedTimestamps =
        (value) ? RoundUpPreparedTimestamps::kRound : RoundUpPreparedTimestamps::kNoRound;
}

//上面的WiredTigerRecoveryUnit::refreshSnapshot()  getPointInTimeReadTimestamp  WiredTigerRecoveryUnit::_txnOpen()等调用
void WiredTigerRecoveryUnit::setTimestampReadSource(ReadSource readSource,
                                                    boost::optional<Timestamp> provided) {
    LOGV2_DEBUG(22416,
                3,
                "setting timestamp read source",
                "readSource"_attr = toString(readSource),
                "provided"_attr = ((provided) ? provided->toString() : "none"));
    invariant(!_isActive() || _timestampReadSource == readSource,
              str::stream() << "Current state: " << toString(_getState())
                            << ". Invalid internal state while setting timestamp read source: "
                            << toString(readSource) << ", provided timestamp: "
                            << (provided ? provided->toString() : "none"));
    invariant(!provided == (readSource != ReadSource::kProvided));
    invariant(!(provided && provided->isNull()));

    _timestampReadSource = readSource;
    _readAtTimestamp = (provided) ? *provided : Timestamp();
}

RecoveryUnit::ReadSource WiredTigerRecoveryUnit::getTimestampReadSource() const {
    return _timestampReadSource;
}

void WiredTigerRecoveryUnit::beginIdle() {
    // Close all cursors, we don't want to keep any old cached cursors around.
    if (_session) {
        _session->closeAllCursors("");
    }
}

////CurOp::completeAndLogOperation   TransactionMetricsObserver::onTransactionOperation
std::shared_ptr<StorageStats> WiredTigerRecoveryUnit::getOperationStatistics() const {
    std::shared_ptr<WiredTigerOperationStats> statsPtr(nullptr);

    if (!_session)
        return statsPtr;

	LOGV2_DEBUG(122418,
        5,
        "yang test .......WiredTigerRecoveryUnit::getOperationStatistics");


    WT_SESSION* s = _session->getSession();
    invariant(s);

    statsPtr = std::make_shared<WiredTigerOperationStats>();
	//WiredTigerOperationStats::fetchStats  获取存储引擎统计信息
    statsPtr->fetchStats(s, "statistics:session", "statistics=(fast)");

    return statsPtr;
}

void WiredTigerRecoveryUnit::setCatalogConflictingTimestamp(Timestamp timestamp) {
    // This cannot be called after a storage snapshot is allocated.
    invariant(!_isActive(), toString(_getState()));
    invariant(_timestampReadSource == ReadSource::kNoTimestamp,
              str::stream() << "Illegal to set catalog conflicting timestamp for a read source "
                            << static_cast<int>(_timestampReadSource));
    invariant(_catalogConflictTimestamp.isNull(),
              str::stream() << "Trying to set catalog conflicting timestamp to "
                            << timestamp.toString() << ". It's already set to "
                            << _catalogConflictTimestamp.toString());
    invariant(!timestamp.isNull());

    _catalogConflictTimestamp = timestamp;
}

Timestamp WiredTigerRecoveryUnit::getCatalogConflictingTimestamp() const {
    return _catalogConflictTimestamp;
}

}  // namespace mongo

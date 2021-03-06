﻿/*
	Copyright (c) 2017 TOSHIBA Digital Solutions Corporation

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as
	published by the Free Software Foundation, either version 3 of the
	License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/*!
	@file
	@brief Definition of SyncManager
*/

#ifndef SYNC_MANAGER_H_
#define SYNC_MANAGER_H_

#include "partition_table.h"

class CheckpointService;
struct ManagerSet;
class SyncService;
class TransactionService;

typedef util::VariableSizeAllocatorTraits<256, 1024 * 1024, 1024 * 1024 * 2>
	SyncVariableSizeAllocatorTraits;
typedef util::VariableSizeAllocator<util::Mutex,
	SyncVariableSizeAllocatorTraits>
	SyncVariableSizeAllocator;

static const int32_t DEFAULT_DETECT_SYNC_ERROR_COUNT = 3;


/*!
	@brief Operation type related with Synchronization
*/
enum SyncOperationType {
	OP_SHORTTERM_SYNC_REQUEST,
	OP_SHORTTERM_SYNC_START,
	OP_SHORTTERM_SYNC_START_ACK,
	OP_SHORTTERM_SYNC_LOG,
	OP_SHORTTERM_SYNC_LOG_ACK,
	OP_SHORTTERM_SYNC_END,
	OP_SHORTTERM_SYNC_END_ACK,
	OP_LONGTERM_SYNC_REQUEST,
	OP_LONGTERM_SYNC_START,
	OP_LONGTERM_SYNC_START_ACK,
	OP_LONGTERM_SYNC_CHUNK,
	OP_LONGTERM_SYNC_CHUNK_ACK,
	OP_LONGTERM_SYNC_LOG,
	OP_LONGTERM_SYNC_LOG_ACK,
	OP_SYNC_TIMEOUT,
	OP_DROP_PARTITION,
	OP_LONGTERM_SYNC_PREPARE_ACK,
	SYNC_OPERATION_TYPE_MAX
};

/*!
	@brief Synchronization ID
*/
struct SyncId {
	static const int32_t UNDEF_CONTEXT_ID = -1;
	static const int32_t INITIAL_CONTEXT_VERSION = 0;


	SyncId()
		: contextId_(UNDEF_CONTEXT_ID),
		contextVersion_(INITIAL_CONTEXT_VERSION) {}

	SyncId(int32_t contextId, uint64_t contextVersion)
		: contextId_(contextId), contextVersion_(contextVersion) {}

	~SyncId() {}

	void reset() {
		contextId_ = UNDEF_CONTEXT_ID;
		contextVersion_ = INITIAL_CONTEXT_VERSION;
	}

	bool operator==(const SyncId &id) const {
		if (contextId_ == id.contextId_) {
			return (contextVersion_ == id.contextVersion_);
		}
		else {
			return false;
		}
	}

	bool isValid() {
		return (contextId_ != UNDEF_CONTEXT_ID);
	}

	std::string dump() {
		util::NormalOStringStream ss;
		ss << "{contextId:" << contextId_ << ", version:" << contextVersion_
		<< "}";
		return ss.str();
	}


	int32_t contextId_;
	uint64_t contextVersion_;
};

/*!
	@brief Represents the statistics of synchronization
*/
struct SyncOptStat {
	SyncOptStat(uint32_t partitionNum) {
		allocateList_.assign(partitionNum, 0);
		referenceCounter_.assign(partitionNum, 0);
		existContextCounter_.assign(partitionNum, 0);
		totalAllocateList_.assign(partitionNum, 0);
		partitionNum_ = static_cast<uint32_t>(allocateList_.size());
	}

	void clear() {
		for (PartitionId pId = 0; pId < partitionNum_; pId++) {
			allocateList_[pId] = 0;
			referenceCounter_[pId] = 0;
			totalAllocateList_[pId] = 0;
			existContextCounter_[pId] = 0;
		}
	}

	void statAllocate(PartitionId pId, uint32_t size) {
		allocateList_[pId] += size;
		referenceCounter_[pId]++;
		totalAllocateList_[pId]++;
	}

	void statFree(PartitionId pId, uint32_t size) {
		allocateList_[pId] -= size;
		referenceCounter_[pId]--;
	}

	void setContext(PartitionId pId) {
		existContextCounter_[pId]++;
	}
	void freeContext(PartitionId pId) {
		existContextCounter_[pId]--;
	}

	uint64_t getAllocateSize() {
		uint64_t totalAllocateSize = 0;
		for (PartitionId pId = 0; pId < partitionNum_; pId++) {
			totalAllocateSize += allocateList_[pId];
		}
		return totalAllocateSize;
	}

	uint64_t getTotalAllocateSize() {
		uint64_t totalAllocateSize = 0;
		for (PartitionId pId = 0; pId < partitionNum_; pId++) {
			totalAllocateSize += totalAllocateList_[pId];
		}
		return totalAllocateSize;
	}

	uint64_t getUnfixCount() {
		uint64_t totalUnfixCount = 0;
		for (PartitionId pId = 0; pId < partitionNum_; pId++) {
			totalUnfixCount += referenceCounter_[pId];
		}
		return totalUnfixCount;
	}

	uint64_t getContextCount() {
		uint64_t totalContextCount = 0;
		for (PartitionId pId = 0; pId < partitionNum_; pId++) {
			totalContextCount += existContextCounter_[pId];
		}
		return totalContextCount;
	}

	std::string dump() {
		util::NormalOStringStream ss;
		ss << "allocate info:{";
		for (PartitionId pId = 0; pId < partitionNum_; pId++) {
			ss << " {pId=" << pId << ", allocate:" << allocateList_[pId]
			<< ", ref:" << referenceCounter_[pId] << "}";
		}
		ss << "}";
		return ss.str();
	}

	std::vector<uint64_t> allocateList_;
	std::vector<uint32_t> referenceCounter_;
	std::vector<uint64_t> totalAllocateList_;
	std::vector<uint64_t> existContextCounter_;

	uint32_t partitionNum_;
};

/*!
	@brief Represents contextual information around the current synchronization
*/
class SyncContext {
	friend class SyncManager;
	friend class SyncManagerTest;
	struct SendBackup;

public:

	SyncContext();
	~SyncContext();
	void incrementCounter(NodeId syncTargetNodeId);
	void setDump() {
		isDump_ = true;
	}
	bool isDump() {
		return isDump_;
	}

	void setSendReady() {
		isSendReady_ = true;
	}

	void setPartitionTable(PartitionTable *pt) {
		pt_ = pt;
	}

	bool isSendReady() {
		return isSendReady_;
	}

	void resetCounter() {
		for (size_t i = 0; i < sendBackups_.size(); i++) {
			sendBackups_[i].isAcked_ = true;
		}
		numSendBackup_ = static_cast<uint32_t>(sendBackups_.size());
	}

	bool decrementCounter(NodeId syncTargetNodeId);

	int32_t getCounter() {
		return numSendBackup_;
	}

	void getSyncTargetNodeIds(util::XArray<NodeId> &backups) {
		for (size_t pos = 0; pos < sendBackups_.size(); pos++) {
			backups.push_back(sendBackups_[pos].nodeId_);
		}
	}

	void setSyncTargetLsn(NodeId syncTargetNodeId, LogSequentialNumber lsn);

	void setSyncTargetLsnWithSyncId(
		NodeId syncTargetNodeId, LogSequentialNumber lsn, SyncId backupSyncId);

	LogSequentialNumber getSyncTargetLsn(NodeId syncTargetNodeId) const;

	bool getSyncTargetLsnWithSyncId(NodeId syncTargetNodeId,
		LogSequentialNumber &backupLsn, SyncId &backupSyncId);

	void getCatchupSyncId(SyncId &syncId) {
		if (sendBackups_.size() == 0) {
			return;
	}
		syncId = sendBackups_[0].backupSyncId_;
	}

	int32_t getId() const {
		return id_;
	}

	void setId(int32_t id) {
		id_ = id;
	}

	uint64_t getVersion() const {
		return version_;
	}

	NodeId getRecvNodeId() {
		return recvNodeId_;
	}

	void setRecvNodeId(NodeId recvNodeId) {
		recvNodeId_ = recvNodeId;
	}

	void updateVersion() {
		version_++;
	}

	PartitionRevision &getPartitionRevision() {
		return ptRev_;
	}

	void setPartitionRevision(PartitionRevision &ptRev) {
		ptRev_ = ptRev;
	}

	PartitionId getPartitionId() const {
		return pId_;
	}

	void setPartitionId(PartitionId pId) {
		pId_ = pId;
	}

	StatementId createStatementId() {
		return ++nextStmtId_;
	}

	StatementId getStatementId() const {
		return nextStmtId_;
	}

	uint32_t getCounter() const {
		return numSendBackup_;
	}

	void setSyncCheckpointCompleted() {
		isSyncCpCompleted_ = true;
	}

	bool isSyncCheckpointCompleted() {
		return isSyncCpCompleted_;
	}

	void setSyncCheckpointPending(bool flag) {
		isSyncCpPending_ = flag;
	}

	bool isSyncCheckpointPending() {
		return isSyncCpPending_;
	}

	void setSyncStartCompleted(bool flag) {
		isSyncStartCompleted_ = flag;
	}

	bool isSyncStartCompleted() {
		return isSyncStartCompleted_;
	}


	void setPartitionStatus(PartitionStatus status) {
		status_ = status;
	}

	PartitionStatus getPartitionStatus() {
		return status_;
	}

	void getChunkInfo(int32_t &chunkNum, int32_t &chunkSize) {
		chunkNum = chunkNum_;
		chunkSize = chunkBaseSize_;
	}

	int32_t getChunkNum() {
		return chunkNum_;
	}

	void incProcessedChunkNum(int32_t chunkNum = 1) {
		processedChunkNum_ += chunkNum;
	}

	int32_t getProcessedChunkNum() {
		return processedChunkNum_;
	}

	LogSequentialNumber getStartLsn() {
		return startLsn_;
	}

	LogSequentialNumber getEndLsn() {
		return endLsn_;
	}

	void incProcessedLogNum(int64_t logSize) {
		processedLogSize_ += logSize;
		processedLogNum_++;
	}
	int32_t getProcessedLogNum() {
		return processedLogNum_;
	}
	int64_t getProcessedLogSize() {
		return processedLogSize_;
	}
	void setProcessedLsn(LogSequentialNumber startLsn, LogSequentialNumber endLsn) {
		if (processedLogNum_ == 0) {
			startLsn_ = startLsn;
		}
		endLsn_ = endLsn;
	}
	void setSequentialNumber(int64_t syncId) {
		syncSequentialNumber_ = syncId;
	}
	int64_t getSequentialNumber() {
		return syncSequentialNumber_;
	}

	void setUsed() {
		used_ = true;
	}

	void setUnuse() {
		used_ = false;
	}

	void clear(SyncVariableSizeAllocator &alloc, SyncOptStat *stat);

	void setNextEmptyChain(SyncContext *next) {
		nextEmptyChain_ = next;
	}

	SyncContext *getNextEmptyChain() const {
		return nextEmptyChain_;
	}

	void getSyncId(SyncId &syncId) {
		syncId.contextId_ = id_;
		syncId.contextVersion_ = version_;
	}

	void copyLogBuffer(SyncVariableSizeAllocator &alloc,
		const uint8_t *logBuffer, int32_t logBufferSize, SyncOptStat *stat);

	void copyChunkBuffer(SyncVariableSizeAllocator &alloc,
		const uint8_t *chunkBuffer, int32_t chunkSize, int32_t chunkNum,
		SyncOptStat *stat);

	void freeBuffer(
		SyncVariableSizeAllocator &alloc, SyncType syncType, SyncOptStat *stat);

	void getLogBuffer(uint8_t *&logBuffer, int32_t &logBufferSize);

	void getChunkBuffer(uint8_t *&chunkBuffer, int32_t chunkNo);

	std::string dump(uint8_t detailMode = 0);

	bool checkTotalTime(int64_t checkTime = 15000) {
		if (mode_ == MODE_LONGTERM_SYNC) {
			return true;
		}
		else {
			if (totalTime_ >= checkTime) {
				return true;
			}
			else {
				return false;
			}
		}
	}

	void startAll();
	void endAll() {
		totalTime_	 += (watch_.elapsedNanos() / 1000 / 1000);
	}
	void start(util::Stopwatch &watch) {
		watch.reset();
		watch.start();
	}
	void endLog(util::Stopwatch &watch) { 
		actualLogTime_	 += (watch.elapsedNanos() / 1000 / 1000);
	}
	void endChunk(util::Stopwatch &watch) { 
		actualChunkTime_	 += (watch.elapsedNanos() / 1000 / 1000);
	}
	void endChunkAll() {
		chunkLeadTime_ = (watch_.elapsedNanos() / 1000 / 1000);
	}
	void setSyncMode(SyncMode mode, PartitionRoleStatus roleStatus) {
		mode_ = mode;
		roleStatus_ = roleStatus;
	}
	SyncMode getSyncMode() {
		return mode_;
	}
	PartitionRoleStatus getPartitionRoleStatus() {
		return roleStatus_;
	}

	void endCheck() {
		if (mode_ == MODE_LONGTERM_SYNC) {
			endAll();
		}
	}
	std::string getSyncModeStr() {
		if (mode_ == MODE_SHORTTERM_SYNC) {
			return "SHORT_TERM_SYNC";
		}
		else {
			return "LONG_TERM_SYNC";
		}
	}


private:

	struct SendBackup {
		SendBackup()
			: nodeId_(UNDEF_NODEID), isAcked_(false), lsn_(UNDEF_LSN) {}

		SendBackup(NodeId nodeId)
			: nodeId_(nodeId), isAcked_(false), lsn_(UNDEF_LSN) {}

		~SendBackup() {}

		NodeId nodeId_;
		bool isAcked_;
		LogSequentialNumber lsn_;
		SyncId backupSyncId_;
	};



	int32_t id_;
	PartitionId pId_;
	uint64_t version_;
	bool used_;
	uint32_t numSendBackup_;
	StatementId nextStmtId_;
	NodeId recvNodeId_;

	bool isSyncCpCompleted_;
	bool isSyncCpPending_;
	bool isSyncStartCompleted_;
	SyncContext *nextEmptyChain_;
	PartitionRevision ptRev_;

	util::NormalXArray<SendBackup> sendBackups_;

	int32_t processedChunkNum_;

	uint8_t *logBuffer_;

	int32_t logBufferSize_;

	uint8_t *chunkBuffer_;

	int32_t chunkBufferSize_;

	int32_t chunkBaseSize_;

	int32_t chunkNum_;

	int32_t chunkNo_;

	PartitionStatus status_;

	int32_t queueSize_;

	SyncMode mode_;
	PartitionRoleStatus roleStatus_;
	int32_t processedLogNum_;
	int64_t processedLogSize_;
	int64_t actualLogTime_;
	int64_t actualChunkTime_;
	int64_t chunkLeadTime_;
	int64_t totalTime_;
	LogSequentialNumber startLsn_;
	LogSequentialNumber endLsn_;
	int64_t syncSequentialNumber_;
	int64_t globalSequentialNumber_;
	util::Stopwatch watch_;
	PartitionTable *pt_;

	bool isDump_;
	bool isSendReady_;
};

struct SyncStatus {
	SyncStatus() {
		clear();
	}
	void clear() {
		pId_ = -1;
		ssn_ = -1;
		chunkNum_ = 0;
		startLsn_ = 0;
		endLsn_ = 0;
		errorCount_ = 0;
	}
	PartitionId checkAndUpdate(SyncContext *targetContext);



	PartitionId pId_;
	int64_t ssn_;
	int32_t chunkNum_;
	LogSequentialNumber startLsn_;
	LogSequentialNumber endLsn_;
	int32_t errorCount_;
};

/*!
	@brief SyncManager
*/
class SyncManager {
	class SyncConfig;
	class ExtraConfig;
	class SyncContextTable;

	friend class SyncManagerTest;

public:
	static const uint32_t SYNC_MODE_NORMAL = 0;
	static const uint32_t SYNC_MODE_RETRY_CHUNK = 1;
	static const int32_t DEFAULT_LOG_SYNC_MESSAGE_MAX_SIZE;
	static const int32_t DEFAULT_CHUNK_SYNC_MESSAGE_MAX_SIZE;

	struct LongSyncEntry {
		LongSyncEntry() : syncSequentialNumber_(-1), isOwner_(true) {}
		LongSyncEntry(SyncId &syncId, PartitionRevision &ptRev, int64_t syncSequentialNumber, bool isOwner) :
				syncId_(syncId), ptRev_(ptRev),
				syncSequentialNumber_(syncSequentialNumber), isOwner_(isOwner) {
		}
		SyncId syncId_;
		PartitionRevision ptRev_;
		int64_t syncSequentialNumber_;
		bool isOwner_;
	};


	SyncOptStat syncOptStat_;

	SyncOptStat *getSyncOptStat() {
		return &syncOptStat_;
	}

	SyncManager(const ConfigTable &configTable, PartitionTable *pt);

	~SyncManager();

	void initialize(ManagerSet *mgrSet);
	SyncContext *createSyncContext(EventContext &ec, PartitionId pId, PartitionRevision &ptRev,
			SyncMode syncMode, PartitionRoleStatus roleStatus);

	ClusterManager *getClusterManager() {
		return clsMgr_;
	}

	uint64_t getContextCount() {
		uint64_t retVal = 0;
		for (size_t pos = 0; pos < pt_->getPartitionNum(); pos++) {
			
				retVal += syncContextTables_[pos]->getUsedNum();
		}
		return retVal;
	}

	void checkCurrentContext(EventContext &ec, PartitionId pId, bool isOwner, SyncMode mode = MODE_SHORTTERM_SYNC);
	void checkCurrentContextWithLock(EventContext &ec, PartitionId pId, bool isOwner, SyncMode mode = MODE_SHORTTERM_SYNC);

	struct LongSyncEntryManager {
		LongSyncEntryManager(util::StackAllocator &alloc, int32_t partitionNum) :
				alloc_(alloc), syncEntryList_(alloc), syncCatchupEntryList_(alloc), currentPId_(UNDEF_PARTITIONID),
				currentSyncSequentialNumber_(-1), currentCatchupPId_(UNDEF_PARTITIONID), currentSyncCatchupSequentialNumber_(-1) {
			LongSyncEntry syncEntry;
			syncEntryList_.assign(partitionNum, syncEntry);
			syncCatchupEntryList_.assign(partitionNum, syncEntry);
		}
		void setCurrentSyncId(PartitionId pId,
				SyncContext *context, PartitionRevision &ptRev);
		void resetCurrentSyncId(PartitionId pId, bool isOwner);

		LongSyncEntry &getEntry(PartitionId pId) {
			return syncEntryList_[pId];
		}

		void getCurrentSyncId(PartitionId &pId, SyncId &syncId, PartitionRevision &ptRev, bool isOwner) {
			if (isOwner) {
				pId = currentPId_;
				if (pId != UNDEF_PARTITIONID) {
					syncId = syncEntryList_[pId].syncId_;
					ptRev = syncEntryList_[pId].ptRev_;
				}
			}
			else {
				pId = currentCatchupPId_;
				if (pId != UNDEF_PARTITIONID) {
					syncId = syncCatchupEntryList_[pId].syncId_;
					ptRev = syncCatchupEntryList_[pId].ptRev_;
				}
			}
		}
		util::StackAllocator &alloc_;
		util::Vector<LongSyncEntry> syncEntryList_;
		util::Vector<LongSyncEntry> syncCatchupEntryList_;

		PartitionId currentPId_;
		int64_t currentSyncSequentialNumber_;
		PartitionId currentCatchupPId_;
		int64_t currentSyncCatchupSequentialNumber_;
	};

	void setCurrentSyncId(
		PartitionId pId, SyncContext *context, PartitionRevision &ptRev) {
		longSyncEntryManager_.setCurrentSyncId(pId, context, ptRev);
	}

	void getCurrentSyncId(
		PartitionId &pId, SyncId &syncId, PartitionRevision &ptRev, bool isOwner) {
			longSyncEntryManager_.getCurrentSyncId(pId, syncId, ptRev, isOwner);
	}

	PartitionId checkCurrentSyncStatus();

	util::FixedSizeAllocator<util::Mutex> &getFixedSizeAllocator() {
		return fixedSizeAlloc_;
	}

	SyncContext *getSyncContext(PartitionId pId, SyncId &syncId);

	void removeSyncContext(EventContext &ec, PartitionId pId, SyncContext *&context, bool isFailed);

	void removePartition(PartitionId pId);

	void checkExecutable(
		SyncOperationType operation, PartitionId pId, PartitionRole &role);

	SyncVariableSizeAllocator &getVariableSizeAllocator() {
		return varSizeAlloc_;
	}
	SyncConfig &getConfig() {
		return syncConfig_;
	};

	ExtraConfig &getExtraConfig() {
		return extraConfig_;
	};

	PartitionTable *getPartitionTable() {
		return pt_;
	}

	std::string dumpAll();

	std::string dump(PartitionId pId);

	int32_t getActiveContextNum();

	Size_t getChunkSize() {
		return chunkSize_;
	}

	void setSyncMode(int32_t mode) {
		syncMode_ = mode;
	}

	int32_t getSyncMode() {
		return syncMode_;
	}
	uint8_t *getChunkBuffer(PartitionGroupId pgId) {
		return &chunkBufferList_[chunkSize_ * pgId];
	}

private:
	static const uint32_t DEFAULT_CONTEXT_SLOT_NUM = 1;



	class SyncContextTable {
		friend class SyncManager;

	public:
		SyncContextTable(util::StackAllocator &alloc, PartitionId pId,
			uint32_t numInitialSlot, SyncVariableSizeAllocator *varAlloc);

		~SyncContextTable();

		SyncContext *createSyncContext(PartitionRevision &ptRev);

		SyncContext *getSyncContext(int32_t id, uint64_t version) const;

		void removeSyncContext(SyncVariableSizeAllocator &varSizeAlloc,
			SyncContext *&context, SyncOptStat *stat);

		int32_t getUsedNum() {
			return numUsed_;
		}

	private:
		static const uint32_t SLOT_SIZE = 128;

		const PartitionId pId_;
		int32_t numCounter_;
		SyncContext *freeList_;
		int32_t numUsed_;
		util::StackAllocator *alloc_;
		util::XArray<SyncContext *> slots_;
		SyncVariableSizeAllocator *varSizeAlloc_;
	};

	/*!
		@brief Represents config for SyncManager
	*/
	class SyncConfig {
	public:
		SyncConfig(const ConfigTable &config)
			: syncTimeoutInterval_(changeTimeSecToMill(
				config.get<int32_t>(CONFIG_TABLE_SYNC_TIMEOUT_INTERVAL))),
			maxMessageSize_(static_cast<int32_t>(config.get<int32_t>(
				CONFIG_TABLE_SYNC_LONG_SYNC_MAX_MESSAGE_SIZE))),
			sendChunkSizeLimit_(static_cast<int32_t>(ConfigTable::megaBytesToBytes(config.get<int32_t>(
				CONFIG_TABLE_SYNC_CHUNK_MAX_MESSAGE_SIZE)))), blockSize_((config.get<int32_t>(
				CONFIG_TABLE_DS_STORE_BLOCK_SIZE))) {
			if (config.get<int32_t>(CONFIG_TABLE_SYNC_LONG_SYNC_MAX_MESSAGE_SIZE) ==
				static_cast<int32_t>(ConfigTable::megaBytesToBytes(DEFAULT_LOG_SYNC_MESSAGE_MAX_SIZE))) {
				maxMessageSize_ = (static_cast<int32_t>(ConfigTable::megaBytesToBytes(config.get<int32_t>(
					CONFIG_TABLE_SYNC_LOG_MAX_MESSAGE_SIZE))));
			}
			sendChunkNum_ = (sendChunkSizeLimit_ / blockSize_) + 1;
		}

		int32_t getSyncTimeoutInterval() const {
			return syncTimeoutInterval_;
		}

		int32_t getMaxMessageSize() const {
			return maxMessageSize_;
		}

		bool setMaxMessageSize(int32_t maxMessageSize) {
			maxMessageSize_ = maxMessageSize;
			return true;
		}

		bool setMaxChunkMessageSize(int32_t maxMessageSize) {
			sendChunkSizeLimit_ = maxMessageSize;
			sendChunkNum_ = (sendChunkSizeLimit_ / blockSize_) + 1;
			return true;
		}
		int32_t getSendChunkNum() {
			return sendChunkNum_;
		}

	private:
		int32_t syncTimeoutInterval_;
		int32_t maxMessageSize_;
		int32_t sendChunkNum_;
		int32_t sendChunkSizeLimit_;
		int32_t blockSize_;
	};

	/*!
		@brief Represents extra config for SyncManager
	*/
	class ExtraConfig {
	public:
		static const int32_t SYC_APPROXIMATE_GAP_LSN = 100;
		static const int32_t SYC_LOCKCONFLICT_INTERVAL = 30000;
		static const int32_t SYC_APPROXIMATE_WAIT_INTERVAL = 10000;

		static const int32_t SYC_SHORTTERM_LIMIT_QUEUE_SIZE = 10000;
		static const int32_t SYC_SHORTTERM_LOWLOAD_LOG_INTERVAL = 0;
		static const int32_t SYC_SHORTTERM_HIGHLOAD_LOG_INTERVAL = 0;

		static const int32_t SYC_LONGTERM_LIMIT_QUEUE_SIZE = 40;
		static const int32_t SYC_LONGTERM_LOWLOAD_LOG_INTERVAL = 0;
		static const int32_t SYC_LONGTERM_HIGHLOAD_LOG_INTERVAL = 100;
		static const int32_t SYC_LONGTERM_LOWLOAD_CHUNK_INTERVAL = 0;
		static const int32_t SYC_LONGTERM_HIGHLOAD_CHUNK_INTERVAL = 100;

		static const int32_t SYC_LONGTERM_DUMP_CHUNK_INTERVAL = 5000;

		ExtraConfig(const ConfigTable &config)
			: 
		longtermNearestLsnGap_(config.get<int32_t>(
				CONFIG_TABLE_SYNC_APPROXIMATE_GAP_LSN)),
			lockConflictPendingInterval_(changeTimeSecToMill(
				config.get<int32_t>(CONFIG_TABLE_SYNC_LOCKCONFLICT_INTERVAL))),
			longtermNearestInterval_(changeTimeSecToMill(
				config.get<int32_t>(CONFIG_TABLE_SYNC_APPROXIMATE_WAIT_INTERVAL))),

			shorttermLimitQueueSize_(config.get<int32_t>(
			CONFIG_TABLE_SYNC_SHORTTERM_LIMIT_QUEUE_SIZE)),
			shorttermLowLoadLogInterval_(
				config.get<int32_t>(CONFIG_TABLE_SYNC_SHORTTERM_LOWLOAD_LOG_INTERVAL)),
			shorttermHighLoadLogInterval_(
				config.get<int32_t>(CONFIG_TABLE_SYNC_SHORTTERM_HIGHLOAD_LOG_INTERVAL)),
			longtermLimitQueueSize_(
				config.get<int32_t>(CONFIG_TABLE_SYNC_LONGTERM_LIMIT_QUEUE_SIZE)),
			longtermLowLoadLogInterval_(
				config.get<int32_t>(CONFIG_TABLE_SYNC_LONGTERM_LOWLOAD_LOG_INTERVAL)),	
			longtermHighLoadLogInterval_(
				config.get<int32_t>(CONFIG_TABLE_SYNC_LONGTERM_HIGHLOAD_LOG_INTERVAL)),
			longtermLowLoadChunkInterval_(
				config.get<int32_t>(CONFIG_TABLE_SYNC_LONGTERM_LOWLOAD_CHUNK_INTERVAL)),
			longtermHighLoadChunkInterval_(
				config.get<int32_t>(CONFIG_TABLE_SYNC_LONGTERM_HIGHLOAD_CHUNK_INTERVAL)),
			longtermDumpChunkInterval_(
					config.get<int32_t>(CONFIG_TABLE_SYNC_LONGTERM_DUMP_CHUNK_INTERVAL)) {}

		int32_t getLongtermDumpChunkInterval() const {
			return longtermDumpChunkInterval_;
		}

		int32_t getLockConflictPendingInterval() const {
			return lockConflictPendingInterval_;
		}

		bool setLongtermDumpInterval(int32_t size) {
			if (size < 0 || size > INT32_MAX) {
				return false;
			}
			longtermDumpChunkInterval_ = size;
			return true;
		}

		bool setApproximateLsnGap(int32_t gap) {
			if (gap < 0 || gap > INT32_MAX) {
				return false;
			}
			longtermNearestLsnGap_ = gap;
			return true;
		}

		int32_t getApproximateGapLsn() {
			return longtermNearestLsnGap_;
		}

		bool setApproximateWaitInterval(int32_t interval) {
			if (interval < 0 || interval > INT32_MAX) {
				return false;
			}
			longtermNearestInterval_ = interval;
			return true;
		}

		int32_t getApproximateWaitInterval() {
			return longtermNearestInterval_;
		}

		bool setLockWaitInterval(int32_t interval) {
			if (interval < 0 || interval > INT32_MAX) {
				return false;
			}
			lockConflictPendingInterval_ = interval;
			return true;
		}

		bool setLimitShorttermQueueSize(int32_t size) {
			if (size < 0 || size > INT32_MAX) {
				return false;
			}
			shorttermLimitQueueSize_ = size;
			return true;
		}

		int32_t getLimitShorttermQueueSize() {
			return shorttermLimitQueueSize_;
		}

		bool setLimitLongtermQueueSize(int32_t size) {
			if (size < 0 || size > INT32_MAX) {
				return false;
			}
			longtermLimitQueueSize_ = size;
			return true;
		}

		int32_t getLimitLongtermQueueSize() {
			return longtermLimitQueueSize_;
		}

		bool setShorttermLowLoadLogWaitInterval(int32_t size) {
			if (size < 0 || size > INT32_MAX) {
				return false;
			}
			shorttermLowLoadLogInterval_ = size;
			return true;
		}

		int32_t getShorttermLowLoadLogWaitInterval() {
			return shorttermLowLoadLogInterval_;
		}

		bool setShorttermHighLoadLogWaitInterval(int32_t size) {
			if (size < 0 || size > INT32_MAX) {
				return false;
			}
			shorttermHighLoadLogInterval_ = size;
			return true;
		}

		int32_t getShorttermHighLoadLogWaitInterval() {
			return shorttermHighLoadLogInterval_;
		}

		bool setLongtermLowLoadLogWaitInterval(int32_t size) {
			if (size < 0 || size > INT32_MAX) {
				return false;
			}
			longtermLowLoadLogInterval_ = size;
			return true;
		}

		int32_t getLongtermLowLoadLogWaitInterval() {
			return longtermLowLoadLogInterval_;
		}

		bool setLongtermHighLoadLogWaitInterval(int32_t size) {
			if (size < 0 || size > INT32_MAX) {
				return false;
			}
			longtermHighLoadLogInterval_ = size;
			return true;
		}

		int32_t getLongtermHighLoadLogWaitInterval() {
			return longtermHighLoadLogInterval_;
		}

		bool setLongtermLowLoadChunkWaitInterval(int32_t size) {
			if (size < 0 || size > INT32_MAX) {
				return false;
			}
			longtermLowLoadChunkInterval_ = size;
			return true;
		}

		int32_t getLongtermLowLoadChunkWaitInterval() {
			return longtermLowLoadChunkInterval_;
		}

		bool setLongtermHighLoadChunkWaitInterval(int32_t size) {
			if (size < 0 || size > INT32_MAX) {
				return false;
			}
			longtermHighLoadChunkInterval_ = size;
			return true;
		}

		int32_t getLongtermHighLoadChunkWaitInterval() {
			return longtermHighLoadChunkInterval_;
		}

	private:
		int32_t longtermNearestLsnGap_;
		int32_t lockConflictPendingInterval_;
		int32_t longtermNearestInterval_;
		int32_t shorttermLimitQueueSize_;
		int32_t shorttermLowLoadLogInterval_;
		int32_t shorttermHighLoadLogInterval_;
		int32_t longtermLimitQueueSize_;
		int32_t longtermLowLoadLogInterval_;
		int32_t longtermHighLoadLogInterval_;
		int32_t longtermLowLoadChunkInterval_;
		int32_t longtermHighLoadChunkInterval_;
		int32_t longtermDumpChunkInterval_;
	};

	struct Config : public ConfigTable::ParamHandler {
		Config() : syncMgr_(NULL){};
		void setUpConfigHandler(
			SyncManager *syncManager, ConfigTable &configTable);
		virtual void operator()(
			ConfigTable::ParamId id, const ParamValue &value);
		SyncManager *syncMgr_;
	};



	util::RWLock &getAllocLock() {
		return allocLock;
	}

	void createPartition(PartitionId pId);





	static class ConfigSetUpHandler : public ConfigTable::SetUpHandler {
		virtual void operator()(ConfigTable &config);
	} configSetUpHandler_;



	util::FixedSizeAllocator<util::Mutex> fixedSizeAlloc_;

	util::StackAllocator alloc_;

	SyncVariableSizeAllocator varSizeAlloc_;

	util::RWLock allocLock;

	SyncContextTable **syncContextTables_;
	PartitionTable *pt_;

	SyncConfig syncConfig_;
	ExtraConfig extraConfig_;

	uint8_t *chunkBufferList_;
	Size_t chunkSize_;
	int32_t syncMode_;
	Config config_;

	int64_t syncSequentialNumber_;
	LongSyncEntryManager longSyncEntryManager_;
	CheckpointService *cpSvc_;
	SyncService *syncSvc_;
	TransactionService *txnSvc_;
	ClusterManager *clsMgr_;
	SyncStatus currentSyncStatus_;
};

class LongtermSyncInfo {
public:
		LongtermSyncInfo() : contextId_(-1), contextVersion_(0),
				syncSequentialNumber_(0) {}
		LongtermSyncInfo(int32_t contextId, uint64_t contextVersion, int64_t syncSequentialNumber) : 
				contextId_(contextId), contextVersion_(contextVersion), syncSequentialNumber_(syncSequentialNumber) {}
		bool check() {
			return true;
		}
		MSGPACK_DEFINE(contextId_, contextVersion_, syncSequentialNumber_);
		int32_t getId() {
			return contextId_;
		}
		uint64_t  getVersion() {
			return contextVersion_;
		}
		uint64_t getSequentialNumber() {
			return syncSequentialNumber_;
		}
		void copy(LongtermSyncInfo &info) {
			contextId_ = info.getId();
			contextVersion_ = info.getVersion();
			syncSequentialNumber_ = info.getSequentialNumber();
		}
		std::string dump() const {
			util::NormalOStringStream ss;
			ss << contextId_ << ", "  << contextVersion_ << ", " << syncSequentialNumber_;
			return ss.str().c_str();
		}

private:
		int32_t contextId_;
		uint64_t contextVersion_;
		int64_t syncSequentialNumber_;
};

#endif

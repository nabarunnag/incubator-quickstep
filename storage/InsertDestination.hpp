/**
 *   Copyright 2011-2015 Quickstep Technologies LLC.
 *   Copyright 2015-2016 Pivotal Software, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 **/

#ifndef QUICKSTEP_STORAGE_INSERT_DESTINATION_HPP_
#define QUICKSTEP_STORAGE_INSERT_DESTINATION_HPP_

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

#include "catalog/CatalogRelation.hpp"
#include "catalog/CatalogTypedefs.hpp"
#include "catalog/PartitionScheme.hpp"
#include "query_execution/QueryExecutionMessages.pb.h"
#include "query_execution/QueryExecutionTypedefs.hpp"
#include "query_execution/QueryExecutionUtil.hpp"
#include "storage/InsertDestinationInterface.hpp"
#include "storage/StorageBlock.hpp"
#include "storage/StorageBlockInfo.hpp"
#include "storage/StorageBlockLayout.hpp"
#include "threading/SpinMutex.hpp"
#include "threading/ThreadIDBasedMap.hpp"
#include "types/containers/Tuple.hpp"
#include "utility/Macros.hpp"

#include "glog/logging.h"

#include "gtest/gtest_prod.h"

#include "tmb/id_typedefs.h"
#include "tmb/tagged_message.h"

namespace tmb { class MessageBus; }

namespace quickstep {

class CatalogRelationSchema;
class StorageManager;
class ValueAccessor;

namespace merge_run_operator {
class RunCreator;
}  // namespace merge_run_operator

namespace serialization { class InsertDestination; }

/** \addtogroup Storage
 *  @{
 */

/**
 * @brief Base class for different strategies for getting blocks to insert
 *        tuples into.
 **/
class InsertDestination : public InsertDestinationInterface {
 public:
  /**
   * @brief Constructor.
   *
   * @param storage_manager The StorageManager to use.
   * @param relation The relation to insert tuples into.
   * @param layout The layout to use for any newly-created blocks. If NULL,
   *        defaults to relation's default layout.
   * @param relational_op_index The index of the relational operator in the
   *        QueryPlan DAG that has outputs.
   * @param foreman_client_id The TMB client ID of the Foreman thread.
   * @param bus A pointer to the TMB.
   **/
  InsertDestination(StorageManager *storage_manager,
                    CatalogRelation *relation,
                    StorageBlockLayout *layout,
                    const std::size_t relational_op_index,
                    const tmb::client_id foreman_client_id,
                    tmb::MessageBus *bus);

  /**
   * @brief Virtual destructor.
   **/
  virtual ~InsertDestination() {
  }

  /**
   * @brief A factory method to generate the InsertDestination from the
   *        serialized Protocol Buffer representation.
   *
   * @param proto A serialized Protocol Buffer representation of an
   *        InsertDestination, originally generated by the optimizer.
   * @param relation The relation to insert tuples into.
   * @param storage_manager The StorageManager to use.
   * @param bus A pointer to the TMB.
   *
   * @return The constructed InsertDestination.
   */
  static InsertDestination* ReconstructFromProto(const serialization::InsertDestination &proto,
                                                 CatalogRelation *relation,
                                                 StorageManager *storage_manager,
                                                 tmb::MessageBus *bus);

  /**
   * @brief Check whether a serialized InsertDestination is fully-formed and
   *        all parts are valid.
   *
   * @param proto A serialized Protocol Buffer representation of an
   *        InsertDestination, originally generated by the optimizer.
   * @param relation The relation to insert tuples into.
   *
   * @return Whether proto is fully-formed and valid.
   **/
  static bool ProtoIsValid(const serialization::InsertDestination &proto,
                           const CatalogRelation &relation);

  const CatalogRelationSchema& getRelation() const override {
    return *relation_;
  }

  attribute_id getPartitioningAttribute() const override {
    return -1;
  }

  void insertTuple(const Tuple &tuple) override;

  void insertTupleInBatch(const Tuple &tuple) override;

  void bulkInsertTuples(ValueAccessor *accessor, bool always_mark_full = false) override;

  void bulkInsertTuplesWithRemappedAttributes(
      const std::vector<attribute_id> &attribute_map,
      ValueAccessor *accessor,
      bool always_mark_full = false) override;

  void insertTuplesFromVector(std::vector<Tuple>::const_iterator begin,
                              std::vector<Tuple>::const_iterator end) override;

  /**
   * @brief Get the set of blocks that were used by clients of this
   *        InsertDestination for insertion.
   * @warning Should only be called AFTER this InsertDestination will no longer
   *          be used, and all blocks have been returned to it via
   *          returnBlock().
   *
   * @return A reference to a vector of block_ids of blocks that were used for
   *         insertion.
   **/
  const std::vector<block_id>& getTouchedBlocks() {
    SpinMutexLock lock(mutex_);
    return getTouchedBlocksInternal();
  }

  /**
   * @brief Get the set of blocks that were partially filled by clients of this
   *        InsertDestination for insertion.
   * @warning Should only be called AFTER this InsertDestination will no longer
   *          be used, and all blocks have been returned to it via
   *          returnBlock() and BEFORE getTouchedBlocks() is called, at all.
   *
   * @param partial_blocks A pointer to the vector of block IDs in which the
   *                       partially filled block IDs will be added.
   **/
  virtual void getPartiallyFilledBlocks(std::vector<MutableBlockReference> *partial_blocks) = 0;

 protected:
  /**
   * @brief Get a block to use for insertion.
   *
   * @return A block to use for inserting tuples.
   **/
  virtual MutableBlockReference getBlockForInsertion() = 0;

  /**
   * @brief Release a block after done using it for insertion.
   * @note This should ALWAYS be called when done inserting into a block.
   *
   * @param block A block, originally supplied by getBlockForInsertion(),
   *        which the client is finished using.
   * @param full If true, the client ran out of space when trying to insert
   *        into block. If false, all inserts were successful.
   **/
  virtual void returnBlock(MutableBlockReference &&block, const bool full) = 0;

  // TODO(chasseur): Once StorageManager and CatalogRelation are threadsafe, it
  // will be safe to use this without holding the mutex.
  virtual MutableBlockReference createNewBlock() = 0;

  virtual const std::vector<block_id>& getTouchedBlocksInternal() = 0;

  /**
   * @brief When a StorageBlock becomes full, pipeline the block id to Foreman.
   *
   * @param id The id of the StorageBlock to be pipelined.
   **/
  void sendBlockFilledMessage(const block_id id) const {
    serialization::DataPipelineMessage proto;
    proto.set_operator_index(relational_op_index_);
    proto.set_block_id(id);
    proto.set_relation_id(relation_->getID());

    // NOTE(zuyu): Using the heap memory to serialize proto as a c-like string.
    const std::size_t proto_length = proto.ByteSize();
    char *proto_bytes = static_cast<char*>(std::malloc(proto_length));
    CHECK(proto.SerializeToArray(proto_bytes, proto_length));

    tmb::TaggedMessage tagged_message(static_cast<const void *>(proto_bytes),
                                      proto_length,
                                      kDataPipelineMessage);
    std::free(proto_bytes);

    // The reason we use the ClientIDMap is as follows:
    // InsertDestination needs to send data pipeline messages to Foreman. To
    // send a TMB message, we need to know the sender and receiver's TMB client
    // ID. In this case, the sender thread is the worker thread that executes
    // this function. To figure out the TMB client ID of the executing thread,
    // there are multiple ways :
    // 1. Trickle down the worker's client ID all the way from Worker::run()
    // method until here.
    // 2. Use thread-local storage - Each worker saves its TMB client ID in the
    // local storage.
    // 3. Use a globally accessible map whose key is the caller thread's
    // process level ID and value is the TMB client ID.
    //
    // Option 1 involves modifying the signature of several functions across
    // different modules. Option 2 was difficult to implement given that Apple's
    // Clang doesn't allow C++11's thread_local keyword. Therefore we chose
    // option 3.
    ClientIDMap *thread_id_map = ClientIDMap::Instance();

    DCHECK(bus_ != nullptr);
    QueryExecutionUtil::SendTMBMessage(bus_,
                                       thread_id_map->getValue(),
                                       foreman_client_id_,
                                       std::move(tagged_message));
  }

  StorageManager *storage_manager_;
  CatalogRelation *relation_;

  // NOTE(zuyu): null means to use the default layout in the CatalogRelation.
  std::unique_ptr<StorageBlockLayout> layout_;
  const std::size_t relational_op_index_;

  tmb::client_id foreman_client_id_;
  tmb::MessageBus *bus_;

  // TODO(chasseur): If contention is high, finer-grained locking of internal
  // data members in subclasses is possible.
  SpinMutex mutex_;

 private:
  // TODO(shoban): Workaround to support sort. Sort needs finegrained control of
  // blocks being used to insert, since inserting in an arbitrary block could
  // lead to unsorted results. InsertDestination API changed while sort was
  // being implemented.
  friend class merge_run_operator::RunCreator;

  DISALLOW_COPY_AND_ASSIGN(InsertDestination);
};

/**
 * @brief Implementation of InsertDestination that always creates new blocks,
 *        leaving some blocks potentially very underfull.
 **/
class AlwaysCreateBlockInsertDestination : public InsertDestination {
 public:
  AlwaysCreateBlockInsertDestination(StorageManager *storage_manager,
                                     CatalogRelation *relation,
                                     StorageBlockLayout *layout,
                                     const std::size_t relational_op_index,
                                     const tmb::client_id foreman_client_id,
                                     tmb::MessageBus *bus)
      : InsertDestination(storage_manager, relation, layout, relational_op_index, foreman_client_id, bus) {
  }

  ~AlwaysCreateBlockInsertDestination() override {
  }

 protected:
  MutableBlockReference getBlockForInsertion() override;

  void returnBlock(MutableBlockReference &&block, const bool full) override;

  MutableBlockReference createNewBlock() override;

  const std::vector<block_id>& getTouchedBlocksInternal() override {
    return returned_block_ids_;
  }

  void getPartiallyFilledBlocks(std::vector<MutableBlockReference> *partial_blocks) override {
  }

 private:
  std::vector<block_id> returned_block_ids_;

  DISALLOW_COPY_AND_ASSIGN(AlwaysCreateBlockInsertDestination);
};

/**
 * @brief Implementation of InsertDestination that keeps a pool of
 *        partially-full blocks. Creates new blocks as necessary when
 *        getBlockForInsertion() is called and there are no partially-full
 *        blocks from the pool which are not "checked out" by workers.
 **/
class BlockPoolInsertDestination : public InsertDestination {
 public:
  BlockPoolInsertDestination(StorageManager *storage_manager,
                             CatalogRelation *relation,
                             StorageBlockLayout *layout,
                             const std::size_t relational_op_index,
                             const tmb::client_id foreman_client_id,
                             tmb::MessageBus *bus)
      : InsertDestination(storage_manager, relation, layout, relational_op_index, foreman_client_id, bus) {
  }

  ~BlockPoolInsertDestination() override {
  }

  // TODO(chasseur): Once block fill statistics are available, replace this
  // with something smarter.
  /**
   * @brief Fill block pool with all the blocks belonging to the relation.
   * @warning Call only ONCE, before using getBlockForInsertion().
   **/
  void addAllBlocksFromRelation();

 protected:
  MutableBlockReference getBlockForInsertion() override;

  void returnBlock(MutableBlockReference &&block, const bool full) override;

  void getPartiallyFilledBlocks(std::vector<MutableBlockReference> *partial_blocks) override;

  const std::vector<block_id>& getTouchedBlocksInternal() override;

  MutableBlockReference createNewBlock() override;

 private:
  FRIEND_TEST(ForemanTest, TwoNodesDAGPartiallyFilledBlocksTest);

  // A vector of references to blocks which are loaded in memory.
  std::vector<MutableBlockReference> available_block_refs_;
  // A vector of blocks from the relation that are not loaded in memory yet.
  std::vector<block_id> available_block_ids_;
  // A vector of fully filled blocks.
  std::vector<block_id> done_block_ids_;

  DISALLOW_COPY_AND_ASSIGN(BlockPoolInsertDestination);
};


class PartitionAwareInsertDestination : public InsertDestination {
 public:
  PartitionAwareInsertDestination(StorageManager *storage_manager,
                                  CatalogRelation *relation,
                                  StorageBlockLayout *layout,
                                  const std::size_t relational_op_index,
                                  const tmb::client_id foreman_client_id,
                                  tmb::MessageBus *bus);

  ~PartitionAwareInsertDestination() override {
    delete[] mutexes_for_partition_;
  }

  /**
   * @brief Manually add a block to the pool.
   * @warning Call only ONCE for each block to add to the pool.
   * @warning Do not use in combination with addAllBlocksFromRelation().
   *
   * @param bid The ID of the block to add to the pool.
   * @part_id The partition to add the block to.
   **/
  void addBlockToPool(const block_id bid, const partition_id part_id) {
    SpinMutexLock lock(mutexes_for_partition_[part_id]);
    available_block_ids_[part_id].push_back(bid);
  }

  void addAllBlocksFromRelation();

  void getPartiallyFilledBlocks(std::vector<MutableBlockReference> *partial_blocks) override {
    const PartitionScheme &partition_scheme = relation_->getPartitionScheme();
    const std::size_t num_partitions = partition_scheme.getNumPartitions();
    // Iterate through each partition and return the partially filled blocks
    // in each partition.
    for (partition_id part_id = 0; part_id < num_partitions; ++part_id) {
      getPartiallyFilledBlocksInPartition(partial_blocks, part_id);
    }
  }

  /**
   * @brief Get the set of blocks that were partially filled by clients of this
   *        InsertDestination for insertion.
   * @warning Should only be called AFTER this InsertDestination will no longer
   *          be used, and all blocks have been returned to it via
   *          returnBlock() and BEFORE getTouchedBlocks() is called, at all.
   *
   * @param partial_blocks A pointer to the vector of block IDs in which the
   *                       partially filled block IDs will be added.
   * @param part_id The partition id for which we want the partially filled blocks.
   **/
  void getPartiallyFilledBlocksInPartition(std::vector<MutableBlockReference> *partial_blocks, partition_id part_id) {
    SpinMutexLock lock(mutexes_for_partition_[part_id]);
    for (std::vector<MutableBlockReference>::size_type i = 0; i < available_block_refs_[part_id].size(); ++i) {
      partial_blocks->push_back((std::move(available_block_refs_[part_id][i])));
    }
    available_block_refs_[part_id].clear();
  }

  attribute_id getPartitioningAttribute() const override;

  void insertTuple(const Tuple &tuple) override;

  void insertTupleInBatch(const Tuple &tuple) override;

  void bulkInsertTuples(ValueAccessor *accessor, bool always_mark_full = false) override;

  void bulkInsertTuplesWithRemappedAttributes(
      const std::vector<attribute_id> &attribute_map,
      ValueAccessor *accessor,
      bool always_mark_full = false) override;

  void insertTuplesFromVector(std::vector<Tuple>::const_iterator begin,
                              std::vector<Tuple>::const_iterator end) override;

 protected:
  MutableBlockReference getBlockForInsertion() override;

  /**
   * @brief Get a block to use for insertion from a partition.
   *
   * @param part_id The partition id for which the client requests a block from.
   *
   * @return A block to use for inserting tuples belonging to a particular partition.
   **/
  MutableBlockReference getBlockForInsertionInPartition(const partition_id part_id);

  void returnBlock(MutableBlockReference &&block, const bool full) override;

  /**
   * @brief Release a block after done using it for insertion.
   * @note This should ALWAYS be called when done inserting into a block.
   *
   * @param block A block, originally supplied by getBlockForInsertion(),
   *        which the client is finished using.
   * @param full If true, the client ran out of space when trying to insert
   *        into block. If false, all inserts were successful.
   * @param part_id The partition id into which we should return the block into.
   **/
  void returnBlockInPartition(MutableBlockReference &&block, const bool full, const partition_id part_id);

  MutableBlockReference createNewBlock() override;
  MutableBlockReference createNewBlockInPartition(const partition_id part_id);

  const std::vector<block_id>& getTouchedBlocksInternal() override;
  const std::vector<block_id>& getTouchedBlocksInternalInPartition(partition_id part_id);

 private:
  // A vector of available block references for each partition.
  std::vector< std::vector<MutableBlockReference> > available_block_refs_;
  // A vector of available block ids for each partition.
  std::vector< std::vector<block_id> > available_block_ids_;
  // A vector of done block ids for each partition.
  std::vector< std::vector<block_id> > done_block_ids_;
  // Done block ids across all partitions.
  std::vector<block_id> all_partitions_done_block_ids_;
  // Mutex for locking each partition separately.
  SpinMutex *mutexes_for_partition_;

  DISALLOW_COPY_AND_ASSIGN(PartitionAwareInsertDestination);
};
/** @} */

}  // namespace quickstep

#endif  // QUICKSTEP_STORAGE_INSERT_DESTINATION_HPP_

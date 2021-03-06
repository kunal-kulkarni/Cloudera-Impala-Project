// Modified by Kunal Kulkarni
// Graduate Student in Computer Science at The Ohio State University 2014
// kulkarni.120@osu.edu
// Block Nested Loop Join in Cloudera Impala


#include "exec/nested-loop-join-node.h"

#include <sstream>

#include "codegen/llvm-codegen.h"
#include "exprs/expr.h"
#include "runtime/row-batch.h"
#include "runtime/runtime-state.h"
#include "util/debug-util.h"
#include "util/runtime-profile.h"

#include "gen-cpp/PlanNodes_types.h"

using namespace boost;
using namespace impala;
using namespace llvm;
using namespace std;

/**
 * Constructor
 */
NestedLoopJoinNode::NestedLoopJoinNode(
    ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs)
  : BlockingJoinNode("NestedLoopJoinNode", TJoinOp::INNER_JOIN, pool, tnode, descs) {
}

/**
 * Call parent's Prepare() before doing any other processing.
 * Initialize any row pools or batches for the right child here 
 */
Status NestedLoopJoinNode::Prepare(RuntimeState* state) {
  DCHECK(join_op_ == TJoinOp::INNER_JOIN);
  RETURN_IF_ERROR(BlockingJoinNode::Prepare(state));
  right_child_pool_.reset(new ObjectPool());
  return Status::OK;
}

/**
 * Close any open right_child structures, such as row batches or pool
 */
void NestedLoopJoinNode::Close(RuntimeState* state) {
  if (is_closed()) return;
  right_child_batches_.Reset();
  right_child_pool_.reset();
  BlockingJoinNode::Close(state);
}

/**
 * Process each row from the left child, until eos_ is true (i.e. end of stream)
 */
Status NestedLoopJoinNode::GetNext(RuntimeState* state, RowBatch* output_batch, bool* eos) {
  RETURN_IF_ERROR(ExecDebugAction(TExecNodePhase::GETNEXT, state));
  RETURN_IF_CANCELLED(state);
  RETURN_IF_ERROR(state->CheckQueryState());
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  if (ReachedLimit() || eos_) {
    *eos = true;
    return Status::OK;
  }

  while (!eos_) {
    // Compute max rows that should be added to output_batch
    int64_t row_batch_capacity = GetRowBatchCapacity(output_batch);

    // Continue processing this row batch
    // Call our nested loop join method here
    num_rows_returned_ +=
        DoNestedLoopJoin(output_batch, left_batch_.get(), row_batch_capacity);

    // Sets up the internal counters required by Impala's execution engine    
    COUNTER_SET(rows_returned_counter_, num_rows_returned_);

    // Break if the output batch capacity is hit
    if (ReachedLimit() || output_batch->AtCapacity()) {
      *eos = ReachedLimit();
      break;
    }

    // Check to see if we're done processing the current left child batch
    if (current_right_child_row_.AtEnd() && left_batch_pos_ == left_batch_->num_rows()) {
      left_batch_->TransferResourceOwnership(output_batch);
      left_batch_pos_ = 0;
      if (output_batch->AtCapacity()) break;
      if (left_side_eos_) {
        *eos = eos_ = true;
        break;
      } else {
        // Get the next tuple of the left child batch
        child(0)->GetNext(state, left_batch_.get(), &left_side_eos_);
        COUNTER_UPDATE(left_child_row_counter_, left_batch_->num_rows());
      }
    }
  }

// Return Status as OK here
  return Status::OK;
}

/**
 * 
 * return: Number of rows that qualified the join conditions
 */
int NestedLoopJoinNode::DoNestedLoopJoin(RowBatch* output_batch, RowBatch* batch,
    int row_batch_capacity) {
  //Number of rows returned by this function
  int rows_returned = 0;

//
  int row_idx = output_batch->AddRows(row_batch_capacity);
  DCHECK(row_idx != RowBatch::INVALID_ROW_INDEX);
  uint8_t* output_row_mem = reinterpret_cast<uint8_t*>(output_batch->GetRow(row_idx));
  TupleRow* output_row = reinterpret_cast<TupleRow*>(output_row_mem);

// Conjuncts function evaluates whether the left row tuple and the right row tuple satisfy the Join condition
// It returns true if this evaluation passes

  Expr* const* conjuncts = &conjuncts_[0];

  while (true) {
    while (!current_right_child_row_.AtEnd()) {
      // Stores the current left and right child rows into output_row which is then passed to Conjuncts for Join evaluation
      CreateOutputRow(output_row, current_left_child_row_, current_right_child_row_.GetRow());
      current_right_child_row_.Next();

      // Evaluate the Conjunct
      if (!EvalConjuncts(conjuncts, conjuncts_.size(), output_row)) continue;
      ++rows_returned;
      // output batch is full or limit is hit
      if (UNLIKELY(rows_returned == row_batch_capacity)) goto end;
      // Advance to next output row
      output_row_mem += output_batch->row_byte_size();
      output_row = reinterpret_cast<TupleRow*>(output_row_mem);
    }

    // Check if the right child row reached its end
    DCHECK(current_right_child_row_.AtEnd());

    // Advance to the next row in the left child batch
    if (UNLIKELY(left_batch_pos_ == batch->num_rows())) goto end;
    current_left_child_row_ = batch->GetRow(left_batch_pos_++);

    // Advance the right child batch to point to the next tuple
    current_right_child_row_ = right_child_batches_.Iterator();
  }

// Commit the output rows which are nothing but the Nested Loop Join resultant tuples
end:
  output_batch->CommitRows(rows_returned);

  return rows_returned;
}

/**
 * DO NOT MODIFY THIS.
 */
int64_t NestedLoopJoinNode::GetRowBatchCapacity(RowBatch* output_batch){
  int64_t max_added_rows = output_batch->capacity() - output_batch->num_rows();
  if (limit() != -1)
    max_added_rows = min(max_added_rows, limit() - rows_returned());
  return max_added_rows;
}

/**
 * DO NOT MODIFY THIS.
 *
 * Do a full scan of the right child [child(1)] and store all row batches
 * in right_child_batches_
 */
Status NestedLoopJoinNode::ConstructBuildSide(RuntimeState* state) {
  RETURN_IF_ERROR(child(1)->Open(state));
  while (true) {
    RowBatch* batch = right_child_pool_->Add(
        new RowBatch(child(1)->row_desc(), state->batch_size(), mem_tracker()));
    bool eos;
    child(1)->GetNext(state, batch, &eos);
    DCHECK_EQ(batch->num_io_buffers(), 0) << "Build batch should be compact.";
    SCOPED_TIMER(build_timer_);
    right_child_batches_.AddRowBatch(batch);
    VLOG_ROW << BuildListDebugString();
    COUNTER_SET(build_row_counter_,
        static_cast<int64_t>(right_child_batches_.total_num_rows()));
    if (eos) break;
  }
  return Status::OK;
}

/**
 * DO NOT MODIFY THIS.
 */
void NestedLoopJoinNode::InitGetNext(TupleRow* first_left_row) {
  current_right_child_row_ = right_child_batches_.Iterator();
}

string NestedLoopJoinNode::BuildListDebugString() {
  stringstream out;
  out << "BuildList(";
  out << right_child_batches_.DebugString(child(1)->row_desc());
  out << ")";
  return out.str();
}
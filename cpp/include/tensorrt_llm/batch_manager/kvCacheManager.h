/*
 * Copyright (c) 2022-2023, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "tensorrt_llm/runtime/bufferManager.h"
#include "tensorrt_llm/runtime/cudaStream.h"
#include "tensorrt_llm/runtime/iTensor.h"

#include <NvInferRuntime.h>
#include <cstdint>
#include <list>
#include <memory>
#include <vector>

namespace tensorrt_llm::runtime
{
class GptModelConfig;
}

namespace tensorrt_llm::batch_manager::kv_cache_manager
{

// Basic building block of a paged KV cache - a single
// cache block. This class just holds metadata, no pointers
// since it is reused across all layers.
class KVCacheBlock
{
public:
    using SizeType = tensorrt_llm::runtime::SizeType;

    explicit KVCacheBlock(SizeType blockIdx);

    [[nodiscard]] SizeType getBlockIdx() const;

    void incRefCount();

    void decRefCount();

    [[nodiscard]] bool hasRefs() const;

private:
    // Linear index of block in pool
    SizeType mBlockIdx;
    // Number of references to the block
    SizeType mRefCount;
};

class GenerationRequest
{
public:
    using SizeType = tensorrt_llm::runtime::SizeType;
    using SharedPtr = std::shared_ptr<GenerationRequest>;

    GenerationRequest(SizeType batchSlotIdx, SizeType numTokens, SizeType beamWidth)
        : mBatchSlotIdx(batchSlotIdx)
        , mNumTokens(numTokens)
        , mBeamWidth(beamWidth)
        , mCacheBlockIds(beamWidth)
    {
    }

    void setBatchSlotIdx(SizeType batchSlotIdx)
    {
        mBatchSlotIdx = batchSlotIdx;
    }

    void setNumTokens(SizeType numTokens)
    {
        mNumTokens = numTokens;
    }

    void addToken()
    {
        mNumTokens++;
    }

    [[nodiscard]] SizeType getBatchSlotIdx() const
    {
        return mBatchSlotIdx;
    }

    [[nodiscard]] SizeType getNumTokens() const
    {
        return mNumTokens;
    }

    [[nodiscard]] SizeType getBeamWidth() const
    {
        return mBeamWidth;
    }

    [[nodiscard]] std::vector<std::vector<SizeType>> const& getCacheBlockIds() const
    {
        return mCacheBlockIds;
    }

    void addCacheBlock(SizeType beamIdx, SizeType blockIdx)
    {
        mCacheBlockIds.at(beamIdx).push_back(blockIdx);
    }

    void clearCacheBlocks()
    {
        for (auto& beamBlockIds : mCacheBlockIds)
        {
            beamBlockIds.clear();
        }
    }

private:
    // Index of sequence in the batch
    SizeType mBatchSlotIdx;
    // Current number of generated tokens
    SizeType mNumTokens;
    // Number of beams
    SizeType mBeamWidth;
    // List of blocks allocated for each beam of the sequence
    std::vector<std::vector<SizeType>> mCacheBlockIds;
};

// BlockManager manages overall metadata of KVCacheBlocks in a layer of the
// network. Layers are expected to be symmetric, so the metadata can be
// reused for all layers of the network.
// The array of cache blocks for a layer is called a pool.
// Each pool has shape [max_blocks, 2, num_heads, tokens_per_block, head_size].
// Size per block and number of blocks per pool are pre-determined and set in
// constructor. These should not be changed after.
// Block shape is [2, num_heads, tokens_per_block, head_size].
// BlockManager maintains a list of free blocks at any time.
// Alloc pops off the block at the front, and Free pushes it back to the vector.
// BlockManager maintains a vector of lists of batchSlotIdx to allocated blocks
// per sequence. This can be used to Free all blocks belonging to a sequence.
class BlockManager
{
public:
    explicit BlockManager(std::size_t blocksInPool);

    void allocateBlock(GenerationRequest& sequence, bool shareAmongBeams = false);

    void freeAllBlocks(GenerationRequest& sequence);

    [[nodiscard]] std::size_t getNumFreeBlocks() const
    {
        return mFreeBlocks.size();
    }

private:
    [[nodiscard]] bool hasFreeBlocks(std::size_t numRequired = 1) const
    {
        return getNumFreeBlocks() >= numRequired;
    }

private:
    // List of free blocks
    std::list<KVCacheBlock> mFreeBlocks;
    // List of allocated blocks for each sequences
    std::vector<std::vector<KVCacheBlock>> mAllocatedBlocks;
};

class KVCacheManager
{
public:
    using SizeType = tensorrt_llm::runtime::SizeType;
    using SequencesPtr = GenerationRequest::SharedPtr;
    using CudaStreamPtr = std::shared_ptr<runtime::CudaStream>;

    KVCacheManager(SizeType numLayers, SizeType numHeads, SizeType numKvHeads, SizeType hiddenSize,
        SizeType tokensPerBlock, SizeType maxNumBlocks, SizeType maxBatchSize, nvinfer1::DataType dtype,
        CudaStreamPtr stream);

    [[nodiscard]] SizeType getTokensPerBlock() const
    {
        return mTokensPerBlock;
    }

    [[nodiscard]] SizeType getMaxNumBlocks() const
    {
        return mMaxNumBlocks;
    }

    // Volume of [2, numKvHeads, tokensPerBlock, sizePerHead]
    [[nodiscard]] SizeType getBlockSize() const
    {
        return mBlockSize;
    }

    [[nodiscard]] BlockManager const& getBlockManager() const
    {
        return mBlockManager;
    }

    [[nodiscard]] std::vector<runtime::ITensor::SharedPtr> const& getMemoryPools() const
    {
        return mPools;
    }

    void addToken(SizeType batchSlotIdx);

    void addSequence(SizeType batchSlotIdx, SizeType inputLength, SizeType beamWidth);

    void removeSequence(SizeType batchSlotIdx);

    [[nodiscard]] std::vector<runtime::ITensor::UniquePtr> getBlockPointersOfSlot(
        SizeType batchSlotIdx, SizeType beamWidth, SizeType maxBlocksPerSeq) const;

    [[nodiscard]] std::vector<runtime::ITensor::UniquePtr> getBlockPointersOfBatch(
        SizeType batchSize, SizeType beamWidth, SizeType maxBlocksPerSeq) const;

    // Volume of [2, numKvHeads, tokensPerBlock, sizePerHead]
    [[nodiscard]] static SizeType constexpr calculatePageSize(tensorrt_llm::runtime::GptModelConfig const& modelConfig);

    // numLayers * 2 * numKvHeads * sizePerHead
    [[nodiscard]] static SizeType constexpr calculateCacheSizePerToken(
        tensorrt_llm::runtime::GptModelConfig const& modelConfig);

private:
    // Number of elements per one blocks
    SizeType mBlockSize;
    // Number of tokens per one blocks
    SizeType mTokensPerBlock;
    // Total maximum number of blocks
    SizeType mMaxNumBlocks;
    // Pools
    std::vector<runtime::ITensor::SharedPtr> mPools;
    // Block manager
    BlockManager mBlockManager;
    // List of all sequences
    std::vector<SequencesPtr> mSequences;
    // buffer for block pointers for all batch slots
    std::vector<runtime::ITensor::UniquePtr> mAllBlockPointers;

    runtime::BufferManager mManager;
};
} // namespace tensorrt_llm::batch_manager::kv_cache_manager
#include <Interpreters/SquashingTransform.h>
#include "Common/logger_useful.h"
#include <Common/CurrentThread.h>
#include "IO/WriteHelpers.h"


namespace DB
{
namespace ErrorCodes
{
    extern const int SIZES_OF_COLUMNS_DOESNT_MATCH;
}

SquashingTransform::SquashingTransform(size_t min_block_size_rows_, size_t min_block_size_bytes_)
    : min_block_size_rows(min_block_size_rows_)
    , min_block_size_bytes(min_block_size_bytes_)
{
}

Block SquashingTransform::add(Block && input_block)
{
    return addImpl<Block &&>(std::move(input_block));
}

Block SquashingTransform::add(const Block & input_block)
{
    return addImpl<const Block &>(input_block);
}

/*
 * To minimize copying, accept two types of argument: const reference for output
 * stream, and rvalue reference for input stream, and decide whether to copy
 * inside this function. This allows us not to copy Block unless we absolutely
 * have to.
 */
template <typename ReferenceType>
Block SquashingTransform::addImpl(ReferenceType input_block)
{
    /// End of input stream.
    if (!input_block)
    {
        Block to_return;
        std::swap(to_return, accumulated_block);
        return to_return;
    }

    /// Just read block is already enough.
    if (isEnoughSize(input_block))
    {
        /// If no accumulated data, return just read block.
        if (!accumulated_block)
        {
            return std::move(input_block);
        }

        /// Return accumulated data (maybe it has small size) and place new block to accumulated data.
        Block to_return = std::move(input_block);
        std::swap(to_return, accumulated_block);
        return to_return;
    }

    /// Accumulated block is already enough.
    if (isEnoughSize(accumulated_block))
    {
        /// Return accumulated data and place new block to accumulated data.
        Block to_return = std::move(input_block);
        std::swap(to_return, accumulated_block);
        return to_return;
    }

    append<ReferenceType>(std::move(input_block));
    if (isEnoughSize(accumulated_block))
    {
        Block to_return;
        std::swap(to_return, accumulated_block);
        return to_return;
    }

    /// Squashed block is not ready.
    return {};
}


template <typename ReferenceType>
void SquashingTransform::append(ReferenceType input_block)
{
    if (!accumulated_block)
    {
        accumulated_block = std::move(input_block);
        return;
    }

    assert(blocksHaveEqualStructure(input_block, accumulated_block));

    for (size_t i = 0, size = accumulated_block.columns(); i < size; ++i)
    {
        const auto source_column = input_block.getByPosition(i).column;

        auto mutable_column = IColumn::mutate(std::move(accumulated_block.getByPosition(i).column));
        mutable_column->insertRangeFrom(*source_column, 0, source_column->size());
        accumulated_block.getByPosition(i).column = std::move(mutable_column);
    }
}


bool SquashingTransform::isEnoughSize(const Block & block)
{
    size_t rows = 0;
    size_t bytes = 0;

    for (const auto & [column, type, name] : block)
    {
        if (!rows)
            rows = column->size();
        else if (rows != column->size())
            throw Exception(ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH, "Sizes of columns doesn't match");

        bytes += column->byteSize();
    }

    return isEnoughSize(rows, bytes);
}


bool SquashingTransform::isEnoughSize(size_t rows, size_t bytes) const
{
    return (!min_block_size_rows && !min_block_size_bytes)
        || (min_block_size_rows && rows >= min_block_size_rows)
        || (min_block_size_bytes && bytes >= min_block_size_bytes);
}


NewSquashingTransform::NewSquashingTransform(size_t min_block_size_rows_, size_t min_block_size_bytes_)
    : min_block_size_rows(min_block_size_rows_)
    , min_block_size_bytes(min_block_size_bytes_)
{
}

Block NewSquashingTransform::add(Chunk && input_chunk)
{
    return addImpl<Chunk &&>(std::move(input_chunk));
}

const ChunksToSquash * getInfoFromChunk(const Chunk & chunk)
{
    const auto& info = chunk.getChunkInfo();
    const auto * agg_info = typeid_cast<const ChunksToSquash *>(info.get());

    return agg_info;
}

template <typename ReferenceType>
Block NewSquashingTransform::addImpl(ReferenceType input_chunk)
{
    if (!input_chunk.hasChunkInfo())
    {
        Block to_return;
        std::swap(to_return, accumulated_block);
        return to_return;
    }

    const auto *info = getInfoFromChunk(input_chunk);
    for (size_t i = 0; i < info->chunks.size(); i++)
        append(std::move(info->chunks[i]), info->data_types);
    // for (auto & one : info->chunks)
    //     append(std::move(one), info->data_types);

    {
        Block to_return;
        std::swap(to_return, accumulated_block);
        return to_return;
    }
}

template <typename ReferenceType>
void NewSquashingTransform::append(ReferenceType input_chunk, DataTypes data_types)
{
    // LOG_TRACE(getLogger("Squashing"), "data_type: {}", data_type->getName());
    if (input_chunk.getNumColumns() == 0)
        return;
    if (!accumulated_block)
    {
        // for (const ColumnPtr& column : input_chunk.getColumns())
        for (size_t i = 0; i < input_chunk.getNumColumns(); ++ i)
        {
            String name = data_types[i]->getName() + toString(i);
            LOG_TRACE(getLogger("Squashing"), "data_type: {}", data_types[i]->getName());
            ColumnWithTypeAndName col = ColumnWithTypeAndName(input_chunk.getColumns()[i], data_types[i], name);
            accumulated_block.insert(accumulated_block.columns(), col);
        }
        return;
    }

    for (size_t i = 0, size = accumulated_block.columns(); i < size; ++i)
    {
        const auto source_column = input_chunk.getColumns()[i];

        auto mutable_column = IColumn::mutate(std::move(accumulated_block.getByPosition(i).column));
        mutable_column->insertRangeFrom(*source_column, 0, source_column->size());
        accumulated_block.getByPosition(i).column = std::move(mutable_column);
    }
}

BalanceTransform::BalanceTransform(Block header_, size_t min_block_size_rows_, size_t min_block_size_bytes_)
    : min_block_size_rows(min_block_size_rows_)
    , min_block_size_bytes(min_block_size_bytes_)
    , header(std::move(header_))
{
    // Use query-level memory tracker
    if (auto * memory_tracker_child = CurrentThread::getMemoryTracker())
        memory_tracker = memory_tracker_child->getParent();
}

Chunk BalanceTransform::add(Block && input_block)
{
    return addImpl<Block &&>(std::move(input_block));
}

Chunk BalanceTransform::convertToChunk(std::vector<Chunk> &chunks)
{
    if (chunks.empty())
        return {};

    auto info = std::make_shared<ChunksToSquash>();
    for (auto &chunk : chunks)
        info->chunks.push_back(chunk.clone());
    info->data_types = data_types;

    chunks.clear();

    return Chunk(header.cloneEmptyColumns(), 0, info);
}


template <typename ReferenceType>
Chunk BalanceTransform::addImpl(ReferenceType input_block)
{
    Chunk input_chunk(input_block.getColumns(), input_block.rows());
    if (!input_block.getDataTypes().empty())
        data_types = input_block.getDataTypes();
    if (!input_chunk)
    {
        Chunk res_chunk = convertToChunk(chunks_to_merge_vec);
        return res_chunk;
    }

    if (isEnoughSize(chunks_to_merge_vec))
        chunks_to_merge_vec.clear();

    if (input_chunk)
        chunks_to_merge_vec.push_back(input_chunk.clone());

    if (isEnoughSize(chunks_to_merge_vec))
    {
        Chunk res_chunk = convertToChunk(chunks_to_merge_vec);
        return res_chunk;
    }
    return input_chunk;
}

bool BalanceTransform::isEnoughSize(const std::vector<Chunk> & chunks)
{
    size_t rows = 0;
    size_t bytes = 0;

    for (const Chunk & chunk : chunks)
    {
        rows += chunk.getNumRows();
        bytes += chunk.bytes();
    }
    checkAndWaitMemoryAvailability(bytes);

    return isEnoughSize(rows, bytes);
}

void BalanceTransform::checkAndWaitMemoryAvailability(size_t bytes)
{
    // bytes_used += bytes;
    if (const auto hard_limit = memory_tracker->getHardLimit() != 0)
    {
        auto free_memory = hard_limit - memory_tracker->get();
        while (Int64(bytes) >= free_memory)
            free_memory = hard_limit - memory_tracker->get();
    }
}

bool BalanceTransform::isEnoughSize(const Chunk & chunk)
{
    return isEnoughSize(chunk.getNumRows(), chunk.bytes());
}


bool BalanceTransform::isEnoughSize(size_t rows, size_t bytes) const
{
    return (!min_block_size_rows && !min_block_size_bytes)
        || (min_block_size_rows && rows >= min_block_size_rows)
        || (min_block_size_bytes && bytes >= min_block_size_bytes);
}

NewSquashingBlockTransform::NewSquashingBlockTransform(size_t min_block_size_rows_, size_t min_block_size_bytes_)
    : min_block_size_rows(min_block_size_rows_)
    , min_block_size_bytes(min_block_size_bytes_)
{
}

Block NewSquashingBlockTransform::add(Chunk && input_chunk)
{
    return addImpl(std::move(input_chunk));
}

const BlocksToSquash * getInfoFromChunkBlock(const Chunk & chunk)
{
    const auto& info = chunk.getChunkInfo();
    const auto * agg_info = typeid_cast<const BlocksToSquash *>(info.get());

    return agg_info;
}

Block NewSquashingBlockTransform::addImpl(Chunk && input_chunk)
{
    if (!input_chunk.hasChunkInfo())
    {
        Block to_return;
        std::swap(to_return, accumulated_block);
        return to_return;
    }

    const auto *info = getInfoFromChunkBlock(input_chunk);
    for (auto & block : info->blocks)
        append(std::move(block));

    {
        Block to_return;
        std::swap(to_return, accumulated_block);
        return to_return;
    }
}

void NewSquashingBlockTransform::append(Block && input_block)
{
    if (input_block.columns() == 0)
        return;
    if (!accumulated_block)
    {
        for (size_t i = 0; i < input_block.columns(); ++ i)
        {
            LOG_TRACE(getLogger("Squashing"), "data_type: {}", input_block.getDataTypeNames()[i]);
            ColumnWithTypeAndName col = ColumnWithTypeAndName(input_block.getColumns()[i], input_block.getDataTypes()[i], input_block.getNames()[i]);
            accumulated_block.insert(accumulated_block.columns(), col);
        }
        return;
    }

    for (size_t i = 0, size = accumulated_block.columns(); i < size; ++i)
    {
        const auto source_column = input_block.getColumns()[i];

        auto mutable_column = IColumn::mutate(std::move(accumulated_block.getByPosition(i).column));
        mutable_column->insertRangeFrom(*source_column, 0, source_column->size());
        accumulated_block.getByPosition(i).column = std::move(mutable_column);
    }
}

BalanceBlockTransform::BalanceBlockTransform(Block header_, size_t min_block_size_rows_, size_t min_block_size_bytes_)
    : min_block_size_rows(min_block_size_rows_)
    , min_block_size_bytes(min_block_size_bytes_)
    , header(std::move(header_))
{
    // Use query-level memory tracker
    if (auto * memory_tracker_child = CurrentThread::getMemoryTracker())
        memory_tracker = memory_tracker_child->getParent();
}

Chunk BalanceBlockTransform::add(Block && input_block)
{
    return addImpl(std::move(input_block));
}

Chunk BalanceBlockTransform::addImpl(Block && input_block)
{
    Chunk input_chunk(input_block.getColumns(), input_block.rows());

    if (!input_chunk)
    {
        Chunk res_chunk = convertToChunk(blocks_to_merge_vec);
        return res_chunk;
    }

    if (isEnoughSize(blocks_to_merge_vec))
        blocks_to_merge_vec.clear();

    if (input_chunk)
        blocks_to_merge_vec.push_back(std::move(input_block));

    if (isEnoughSize(blocks_to_merge_vec))
    {
        Chunk res_chunk = convertToChunk(blocks_to_merge_vec);
        return res_chunk;
    }
    return input_chunk;
}

Chunk BalanceBlockTransform::convertToChunk(std::vector<Block> &blocks)
{
    if (blocks.empty())
        return {};

    auto info = std::make_shared<BlocksToSquash>();
    for (auto &block : blocks)
        info->blocks.push_back(std::move(block));

    blocks.clear(); // we can remove this

    return Chunk(header.cloneEmptyColumns(), 0, info);
}

bool BalanceBlockTransform::isEnoughSize(const std::vector<Block> & blocks)
{
    size_t rows = 0;
    size_t bytes = 0;

    for (const Block & block : blocks)
    {
        rows += block.rows();
        bytes += block.bytes();
    }
    checkAndWaitMemoryAvailability(bytes);

    return isEnoughSize(rows, bytes);
}

void BalanceBlockTransform::checkAndWaitMemoryAvailability(size_t bytes)
{
    // bytes_used += bytes;
    if (const auto hard_limit = memory_tracker->getHardLimit() != 0)
    {
        auto free_memory = hard_limit - memory_tracker->get();
        while (Int64(bytes) >= free_memory)
            free_memory = hard_limit - memory_tracker->get();
    }
}

bool BalanceBlockTransform::isEnoughSize(size_t rows, size_t bytes) const
{
    return (!min_block_size_rows && !min_block_size_bytes)
        || (min_block_size_rows && rows >= min_block_size_rows)
        || (min_block_size_bytes && bytes >= min_block_size_bytes);
}
}

// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License

#include <algorithm>
#include <cstring>
#include <memory>
#include <numeric>
#include <queue>
#include <thread>
#include <boost/iterator/counting_iterator.hpp>
#include <type_traits>
#include <variant>

#include "common/Consts.h"
#include "common/EasyAssert.h"
#include "common/FieldData.h"
#include "common/Types.h"
#include "fmt/format.h"
#include "log/Log.h"
#include "nlohmann/json.hpp"
#include "query/PlanNode.h"
#include "query/SearchOnSealed.h"
#include "segcore/SegmentGrowingImpl.h"
#include "segcore/Utils.h"
#include "storage/RemoteChunkManagerSingleton.h"
#include "storage/Util.h"
#include "storage/ThreadPools.h"
#include "storage/options.h"
#include "storage/space.h"

namespace milvus::segcore {

int64_t
SegmentGrowingImpl::PreInsert(int64_t size) {
    auto reserved_begin = insert_record_.reserved.fetch_add(size);
    return reserved_begin;
}

void
SegmentGrowingImpl::mask_with_delete(BitsetType& bitset,
                                     int64_t ins_barrier,
                                     Timestamp timestamp) const {
    auto del_barrier = get_barrier(get_deleted_record(), timestamp);
    if (del_barrier == 0) {
        return;
    }
    auto bitmap_holder = get_deleted_bitmap(
        del_barrier, ins_barrier, deleted_record_, insert_record_, timestamp);
    if (!bitmap_holder || !bitmap_holder->bitmap_ptr) {
        return;
    }
    auto& delete_bitset = *bitmap_holder->bitmap_ptr;
    AssertInfo(
        delete_bitset.size() == bitset.size(),
        fmt::format(
            "Deleted bitmap size:{} not equal to filtered bitmap size:{}",
            delete_bitset.size(),
            bitset.size()));
    bitset |= delete_bitset;
}

void
SegmentGrowingImpl::try_remove_chunks(FieldId fieldId) {
    //remove the chunk data to reduce memory consumption
    if (indexing_record_.SyncDataWithIndex(fieldId)) {
        auto vec_data_base =
            dynamic_cast<segcore::ConcurrentVector<FloatVector>*>(
                insert_record_.get_field_data_base(fieldId));
        if (vec_data_base && vec_data_base->num_chunk() > 0 &&
            chunk_mutex_.try_lock()) {
            vec_data_base->clear();
            chunk_mutex_.unlock();
        }
    }
}

void
SegmentGrowingImpl::Insert(int64_t reserved_offset,
                           int64_t num_rows,
                           const int64_t* row_ids,
                           const Timestamp* timestamps_raw,
                           const InsertData* insert_data) {
    AssertInfo(insert_data->num_rows() == num_rows,
               "Entities_raw count not equal to insert size");
    //    AssertInfo(insert_data->fields_data_size() == schema_->size(),
    //               "num fields of insert data not equal to num of schema fields");
    // step 1: check insert data if valid
    std::unordered_map<FieldId, int64_t> field_id_to_offset;
    int64_t field_offset = 0;
    for (const auto& field : insert_data->fields_data()) {
        auto field_id = FieldId(field.field_id());
        AssertInfo(!field_id_to_offset.count(field_id), "duplicate field data");
        field_id_to_offset.emplace(field_id, field_offset++);
    }

    // step 2: sort timestamp
    // query node already guarantees that the timestamp is ordered, avoid field data copy in c++

    // step 3: fill into Segment.ConcurrentVector
    insert_record_.timestamps_.set_data_raw(
        reserved_offset, timestamps_raw, num_rows);
    insert_record_.row_ids_.set_data_raw(reserved_offset, row_ids, num_rows);

    // update the mem size of timestamps and row IDs
    stats_.mem_size += num_rows * (sizeof(Timestamp) + sizeof(idx_t));
    for (auto [field_id, field_meta] : schema_->get_fields()) {
        if (field_id.get() < START_USER_FIELDID) {
            continue;
        }
        AssertInfo(field_id_to_offset.count(field_id),
                   fmt::format("can't find field {}", field_id.get()));
        auto data_offset = field_id_to_offset[field_id];
        if (!indexing_record_.SyncDataWithIndex(field_id)) {
            insert_record_.get_field_data_base(field_id)->set_data_raw(
                reserved_offset,
                num_rows,
                &insert_data->fields_data(data_offset),
                field_meta);
        }
        //insert vector data into index
        if (segcore_config_.get_enable_interim_segment_index()) {
            indexing_record_.AppendingIndex(
                reserved_offset,
                num_rows,
                field_id,
                &insert_data->fields_data(data_offset),
                insert_record_);
        }

        // update average row data size
        auto field_data_size = GetRawDataSizeOfDataArray(
            &insert_data->fields_data(data_offset), field_meta, num_rows);
        if (datatype_is_variable(field_meta.get_data_type())) {
            SegmentInternalInterface::set_field_avg_size(
                field_id, num_rows, field_data_size);
        }

        stats_.mem_size += field_data_size;

        try_remove_chunks(field_id);
    }

    // step 4: set pks to offset
    auto field_id = schema_->get_primary_field_id().value_or(FieldId(-1));
    AssertInfo(field_id.get() != INVALID_FIELD_ID, "Primary key is -1");
    std::vector<PkType> pks(num_rows);
    ParsePksFromFieldData(
        pks, insert_data->fields_data(field_id_to_offset[field_id]));
    for (int i = 0; i < num_rows; ++i) {
        insert_record_.insert_pk(pks[i], reserved_offset + i);
    }

    // step 5: update small indexes
    insert_record_.ack_responder_.AddSegment(reserved_offset,
                                             reserved_offset + num_rows);
}

void
SegmentGrowingImpl::LoadFieldData(const LoadFieldDataInfo& infos) {
    // schema don't include system field
    AssertInfo(infos.field_infos.size() == schema_->size(),
               "lost some field data when load for growing segment");
    AssertInfo(infos.field_infos.find(TimestampFieldID.get()) !=
                   infos.field_infos.end(),
               "timestamps field data should be included");
    AssertInfo(
        infos.field_infos.find(RowFieldID.get()) != infos.field_infos.end(),
        "rowID field data should be included");
    auto primary_field_id =
        schema_->get_primary_field_id().value_or(FieldId(-1));
    AssertInfo(primary_field_id.get() != INVALID_FIELD_ID, "Primary key is -1");
    AssertInfo(infos.field_infos.find(primary_field_id.get()) !=
                   infos.field_infos.end(),
               "primary field data should be included");

    size_t num_rows = storage::GetNumRowsForLoadInfo(infos);
    auto reserved_offset = PreInsert(num_rows);
    for (auto& [id, info] : infos.field_infos) {
        auto field_id = FieldId(id);
        auto insert_files = info.insert_files;
        std::sort(insert_files.begin(),
                  insert_files.end(),
                  [](const std::string& a, const std::string& b) {
                      return std::stol(a.substr(a.find_last_of('/') + 1)) <
                             std::stol(b.substr(b.find_last_of('/') + 1));
                  });

        auto channel = std::make_shared<FieldDataChannel>();
        auto& pool =
            ThreadPools::GetThreadPool(milvus::ThreadPoolPriority::MIDDLE);

        LOG_INFO("segment {} loads field {} with num_rows {}",
                 this->get_segment_id(),
                 field_id.get(),
                 num_rows);
        auto load_future =
            pool.Submit(LoadFieldDatasFromRemote, insert_files, channel);

        LOG_INFO("segment {} submits load field {} task to thread pool",
                 this->get_segment_id(),
                 field_id.get());
        auto field_data = storage::CollectFieldDataChannel(channel);
        if (field_id == TimestampFieldID) {
            // step 2: sort timestamp
            // query node already guarantees that the timestamp is ordered, avoid field data copy in c++

            // step 3: fill into Segment.ConcurrentVector
            insert_record_.timestamps_.set_data_raw(reserved_offset,
                                                    field_data);
            continue;
        }

        if (field_id == RowFieldID) {
            insert_record_.row_ids_.set_data_raw(reserved_offset, field_data);
            continue;
        }

        if (!indexing_record_.SyncDataWithIndex(field_id)) {
            insert_record_.get_field_data_base(field_id)->set_data_raw(
                reserved_offset, field_data);
        }
        if (segcore_config_.get_enable_interim_segment_index()) {
            auto offset = reserved_offset;
            for (auto& data : field_data) {
                auto row_count = data->get_num_rows();
                indexing_record_.AppendingIndex(
                    offset, row_count, field_id, data, insert_record_);
                offset += row_count;
            }
        }
        try_remove_chunks(field_id);

        if (field_id == primary_field_id) {
            insert_record_.insert_pks(field_data);
        }

        // update average row data size
        auto field_meta = (*schema_)[field_id];
        if (datatype_is_variable(field_meta.get_data_type())) {
            SegmentInternalInterface::set_field_avg_size(
                field_id,
                num_rows,
                storage::GetByteSizeOfFieldDatas(field_data));
        }

        // update the mem size
        stats_.mem_size += storage::GetByteSizeOfFieldDatas(field_data);

        LOG_INFO("segment {} loads field {} done",
                 this->get_segment_id(),
                 field_id.get());
    }

    // step 5: update small indexes
    insert_record_.ack_responder_.AddSegment(reserved_offset,
                                             reserved_offset + num_rows);
}

void
SegmentGrowingImpl::LoadFieldDataV2(const LoadFieldDataInfo& infos) {
    // schema don't include system field
    AssertInfo(infos.field_infos.size() == schema_->size() + 2,
               "lost some field data when load for growing segment");
    AssertInfo(infos.field_infos.find(TimestampFieldID.get()) !=
                   infos.field_infos.end(),
               "timestamps field data should be included");
    AssertInfo(
        infos.field_infos.find(RowFieldID.get()) != infos.field_infos.end(),
        "rowID field data should be included");
    auto primary_field_id =
        schema_->get_primary_field_id().value_or(FieldId(-1));
    AssertInfo(primary_field_id.get() != INVALID_FIELD_ID, "Primary key is -1");
    AssertInfo(infos.field_infos.find(primary_field_id.get()) !=
                   infos.field_infos.end(),
               "primary field data should be included");

    size_t num_rows = storage::GetNumRowsForLoadInfo(infos);
    auto reserved_offset = PreInsert(num_rows);
    for (auto& [id, info] : infos.field_infos) {
        auto field_id = FieldId(id);
        auto field_data_info = FieldDataInfo(field_id.get(), num_rows);
        auto& pool =
            ThreadPools::GetThreadPool(milvus::ThreadPoolPriority::MIDDLE);
        auto res = milvus_storage::Space::Open(
            infos.url, milvus_storage::Options{nullptr, infos.storage_version});
        AssertInfo(res.ok(), "init space failed");
        std::shared_ptr<milvus_storage::Space> space = std::move(res.value());
        auto load_future = pool.Submit(
            LoadFieldDatasFromRemote2, space, schema_, field_data_info);
        auto field_data =
            milvus::storage::CollectFieldDataChannel(field_data_info.channel);
        if (field_id == TimestampFieldID) {
            // step 2: sort timestamp
            // query node already guarantees that the timestamp is ordered, avoid field data copy in c++

            // step 3: fill into Segment.ConcurrentVector
            insert_record_.timestamps_.set_data_raw(reserved_offset,
                                                    field_data);
            continue;
        }

        if (field_id == RowFieldID) {
            insert_record_.row_ids_.set_data_raw(reserved_offset, field_data);
            continue;
        }

        if (!indexing_record_.SyncDataWithIndex(field_id)) {
            insert_record_.get_field_data_base(field_id)->set_data_raw(
                reserved_offset, field_data);
        }
        if (segcore_config_.get_enable_interim_segment_index()) {
            auto offset = reserved_offset;
            for (auto& data : field_data) {
                auto row_count = data->get_num_rows();
                indexing_record_.AppendingIndex(
                    offset, row_count, field_id, data, insert_record_);
                offset += row_count;
            }
        }
        try_remove_chunks(field_id);

        if (field_id == primary_field_id) {
            insert_record_.insert_pks(field_data);
        }

        // update average row data size
        auto field_meta = (*schema_)[field_id];
        if (datatype_is_variable(field_meta.get_data_type())) {
            SegmentInternalInterface::set_field_avg_size(
                field_id,
                num_rows,
                storage::GetByteSizeOfFieldDatas(field_data));
        }

        // update the mem size
        stats_.mem_size += storage::GetByteSizeOfFieldDatas(field_data);
    }

    // step 5: update small indexes
    insert_record_.ack_responder_.AddSegment(reserved_offset,
                                             reserved_offset + num_rows);
}
SegcoreError
SegmentGrowingImpl::Delete(int64_t reserved_begin,
                           int64_t size,
                           const IdArray* ids,
                           const Timestamp* timestamps_raw) {
    auto field_id = schema_->get_primary_field_id().value_or(FieldId(-1));
    AssertInfo(field_id.get() != -1, "Primary key is -1");
    auto& field_meta = schema_->operator[](field_id);
    std::vector<PkType> pks(size);
    ParsePksFromIDs(pks, field_meta.get_data_type(), *ids);

    // filter out the deletions that the primary key not exists
    std::vector<std::tuple<Timestamp, PkType>> ordering(size);
    for (int i = 0; i < size; i++) {
        ordering[i] = std::make_tuple(timestamps_raw[i], pks[i]);
    }
    auto end =
        std::remove_if(ordering.begin(),
                       ordering.end(),
                       [&](const std::tuple<Timestamp, PkType>& record) {
                           return !insert_record_.contain(std::get<1>(record));
                       });
    size = end - ordering.begin();
    ordering.resize(size);
    if (size == 0) {
        return SegcoreError::success();
    }

    // step 1: sort timestamp

    std::sort(ordering.begin(), ordering.end());
    std::vector<PkType> sort_pks(size);
    std::vector<Timestamp> sort_timestamps(size);

    for (int i = 0; i < size; i++) {
        auto [t, pk] = ordering[i];
        sort_timestamps[i] = t;
        sort_pks[i] = pk;
    }

    // step 2: fill delete record
    deleted_record_.push(sort_pks, sort_timestamps.data());
    stats_.mem_size += size * sizeof(Timestamp) + CalcPksSize(sort_pks);
    return SegcoreError::success();
}

void
SegmentGrowingImpl::LoadDeletedRecord(const LoadDeletedRecordInfo& info) {
    AssertInfo(info.row_count > 0, "The row count of deleted record is 0");
    AssertInfo(info.primary_keys, "Deleted primary keys is null");
    AssertInfo(info.timestamps, "Deleted timestamps is null");
    // step 1: get pks and timestamps
    auto field_id =
        schema_->get_primary_field_id().value_or(FieldId(INVALID_FIELD_ID));
    AssertInfo(field_id.get() != INVALID_FIELD_ID,
               "Primary key has invalid field id");
    auto& field_meta = schema_->operator[](field_id);
    int64_t size = info.row_count;
    std::vector<PkType> pks(size);
    ParsePksFromIDs(pks, field_meta.get_data_type(), *info.primary_keys);
    auto timestamps = reinterpret_cast<const Timestamp*>(info.timestamps);

    // step 2: fill pks and timestamps
    deleted_record_.push(pks, timestamps);

    stats_.mem_size += info.row_count * sizeof(Timestamp) + CalcPksSize(pks);
}

SpanBase
SegmentGrowingImpl::chunk_data_impl(FieldId field_id, int64_t chunk_id) const {
    auto vec = get_insert_record().get_field_data_base(field_id);
    return vec->get_span_base(chunk_id);
}

int64_t
SegmentGrowingImpl::num_chunk() const {
    auto size = get_insert_record().ack_responder_.GetAck();
    return upper_div(size, segcore_config_.get_chunk_rows());
}

DataType
SegmentGrowingImpl::GetFieldDataType(milvus::FieldId field_id) const {
    auto& field_meta = schema_->operator[](field_id);
    return field_meta.get_data_type();
}

void
SegmentGrowingImpl::vector_search(SearchInfo& search_info,
                                  const void* query_data,
                                  int64_t query_count,
                                  Timestamp timestamp,
                                  const BitsetView& bitset,
                                  SearchResult& output) const {
    query::SearchOnGrowing(
        *this, search_info, query_data, query_count, timestamp, bitset, output);
}

std::unique_ptr<DataArray>
SegmentGrowingImpl::bulk_subscript(FieldId field_id,
                                   const int64_t* seg_offsets,
                                   int64_t count) const {
    auto vec_ptr = insert_record_.get_field_data_base(field_id);
    auto& field_meta = schema_->operator[](field_id);
    if (field_meta.is_vector()) {
        auto result = CreateVectorDataArray(count, field_meta);
        if (field_meta.get_data_type() == DataType::VECTOR_FLOAT) {
            bulk_subscript_impl<FloatVector>(field_id,
                                             field_meta.get_sizeof(),
                                             vec_ptr,
                                             seg_offsets,
                                             count,
                                             result->mutable_vectors()
                                                 ->mutable_float_vector()
                                                 ->mutable_data()
                                                 ->mutable_data());
        } else if (field_meta.get_data_type() == DataType::VECTOR_BINARY) {
            bulk_subscript_impl<BinaryVector>(
                field_id,
                field_meta.get_sizeof(),
                vec_ptr,
                seg_offsets,
                count,
                result->mutable_vectors()->mutable_binary_vector()->data());
        } else if (field_meta.get_data_type() == DataType::VECTOR_FLOAT16) {
            bulk_subscript_impl<Float16Vector>(
                field_id,
                field_meta.get_sizeof(),
                vec_ptr,
                seg_offsets,
                count,
                result->mutable_vectors()->mutable_float16_vector()->data());
        } else if (field_meta.get_data_type() == DataType::VECTOR_BFLOAT16) {
            bulk_subscript_impl<BFloat16Vector>(
                field_id,
                field_meta.get_sizeof(),
                vec_ptr,
                seg_offsets,
                count,
                result->mutable_vectors()->mutable_bfloat16_vector()->data());
        } else {
            PanicInfo(DataTypeInvalid, "logical error");
        }
        return result;
    }

    AssertInfo(!field_meta.is_vector(),
               "Scalar field meta type is vector type");
    auto result = CreateScalarDataArray(count, field_meta);
    switch (field_meta.get_data_type()) {
        case DataType::BOOL: {
            bulk_subscript_impl<bool>(vec_ptr,
                                      seg_offsets,
                                      count,
                                      result->mutable_scalars()
                                          ->mutable_bool_data()
                                          ->mutable_data()
                                          ->mutable_data());
            break;
        }
        case DataType::INT8: {
            bulk_subscript_impl<int8_t>(vec_ptr,
                                        seg_offsets,
                                        count,
                                        result->mutable_scalars()
                                            ->mutable_int_data()
                                            ->mutable_data()
                                            ->mutable_data());
            break;
        }
        case DataType::INT16: {
            bulk_subscript_impl<int16_t>(vec_ptr,
                                         seg_offsets,
                                         count,
                                         result->mutable_scalars()
                                             ->mutable_int_data()
                                             ->mutable_data()
                                             ->mutable_data());
            break;
        }
        case DataType::INT32: {
            bulk_subscript_impl<int32_t>(vec_ptr,
                                         seg_offsets,
                                         count,
                                         result->mutable_scalars()
                                             ->mutable_int_data()
                                             ->mutable_data()
                                             ->mutable_data());
            break;
        }
        case DataType::INT64: {
            bulk_subscript_impl<int64_t>(vec_ptr,
                                         seg_offsets,
                                         count,
                                         result->mutable_scalars()
                                             ->mutable_long_data()
                                             ->mutable_data()
                                             ->mutable_data());
            break;
        }
        case DataType::FLOAT: {
            bulk_subscript_impl<float>(vec_ptr,
                                       seg_offsets,
                                       count,
                                       result->mutable_scalars()
                                           ->mutable_float_data()
                                           ->mutable_data()
                                           ->mutable_data());
            break;
        }
        case DataType::DOUBLE: {
            bulk_subscript_impl<double>(vec_ptr,
                                        seg_offsets,
                                        count,
                                        result->mutable_scalars()
                                            ->mutable_double_data()
                                            ->mutable_data()
                                            ->mutable_data());
            break;
        }
        case DataType::VARCHAR: {
            bulk_subscript_ptr_impl<std::string>(vec_ptr,
                                                 seg_offsets,
                                                 count,
                                                 result->mutable_scalars()
                                                     ->mutable_string_data()
                                                     ->mutable_data());
            break;
        }
        case DataType::JSON: {
            bulk_subscript_ptr_impl<Json, std::string>(
                vec_ptr,
                seg_offsets,
                count,
                result->mutable_scalars()->mutable_json_data()->mutable_data());
            break;
        }
        case DataType::ARRAY: {
            // element
            bulk_subscript_array_impl(*vec_ptr,
                                      seg_offsets,
                                      count,
                                      result->mutable_scalars()
                                          ->mutable_array_data()
                                          ->mutable_data());
            break;
        }
        default: {
            PanicInfo(
                DataTypeInvalid,
                fmt::format("unsupported type {}", field_meta.get_data_type()));
        }
    }
    return result;
}

template <typename S, typename T>
void
SegmentGrowingImpl::bulk_subscript_ptr_impl(
    const VectorBase* vec_raw,
    const int64_t* seg_offsets,
    int64_t count,
    google::protobuf::RepeatedPtrField<T>* dst) const {
    auto vec = dynamic_cast<const ConcurrentVector<S>*>(vec_raw);
    auto& src = *vec;
    for (int64_t i = 0; i < count; ++i) {
        auto offset = seg_offsets[i];
        dst->at(i) = std::move(T(src[offset]));
    }
}

template <typename T>
void
SegmentGrowingImpl::bulk_subscript_impl(FieldId field_id,
                                        int64_t element_sizeof,
                                        const VectorBase* vec_raw,
                                        const int64_t* seg_offsets,
                                        int64_t count,
                                        void* output_raw) const {
    static_assert(IsVector<T>);
    auto vec_ptr = dynamic_cast<const ConcurrentVector<T>*>(vec_raw);
    AssertInfo(vec_ptr, "Pointer of vec_raw is nullptr");
    auto& vec = *vec_ptr;

    auto copy_from_chunk = [&]() {
        auto output_base = reinterpret_cast<char*>(output_raw);
        for (int i = 0; i < count; ++i) {
            auto dst = output_base + i * element_sizeof;
            auto offset = seg_offsets[i];
            if (offset == INVALID_SEG_OFFSET) {
                memset(dst, 0, element_sizeof);
            } else {
                auto src = (const uint8_t*)vec.get_element(offset);
                memcpy(dst, src, element_sizeof);
            }
        }
    };
    //HasRawData interface guarantees that data can be fetched from growing segment
    if (HasRawData(field_id.get())) {
        //When data sync with index
        if (indexing_record_.SyncDataWithIndex(field_id)) {
            indexing_record_.GetDataFromIndex(
                field_id, seg_offsets, count, element_sizeof, output_raw);
        } else {
            //Else copy from chunk
            std::lock_guard<std::shared_mutex> guard(chunk_mutex_);
            copy_from_chunk();
        }
    }
    AssertInfo(HasRawData(field_id.get()), "Growing segment loss raw data");
}

template <typename S, typename T>
void
SegmentGrowingImpl::bulk_subscript_impl(const VectorBase* vec_raw,
                                        const int64_t* seg_offsets,
                                        int64_t count,
                                        T* output) const {
    static_assert(IsScalar<S>);
    auto vec_ptr = dynamic_cast<const ConcurrentVector<S>*>(vec_raw);
    AssertInfo(vec_ptr, "Pointer of vec_raw is nullptr");
    auto& vec = *vec_ptr;
    for (int64_t i = 0; i < count; ++i) {
        auto offset = seg_offsets[i];
        output[i] = vec[offset];
    }
}

template <typename T>
void
SegmentGrowingImpl::bulk_subscript_array_impl(
    const VectorBase& vec_raw,
    const int64_t* seg_offsets,
    int64_t count,
    google::protobuf::RepeatedPtrField<T>* dst) const {
    auto vec_ptr = dynamic_cast<const ConcurrentVector<Array>*>(&vec_raw);
    AssertInfo(vec_ptr, "Pointer of vec_raw is nullptr");
    auto& vec = *vec_ptr;
    for (int64_t i = 0; i < count; ++i) {
        auto offset = seg_offsets[i];
        if (offset != INVALID_SEG_OFFSET) {
            dst->at(i) = vec[offset].output_data();
        }
    }
}

void
SegmentGrowingImpl::bulk_subscript(SystemFieldType system_type,
                                   const int64_t* seg_offsets,
                                   int64_t count,
                                   void* output) const {
    switch (system_type) {
        case SystemFieldType::Timestamp:
            bulk_subscript_impl<Timestamp>(&this->insert_record_.timestamps_,
                                           seg_offsets,
                                           count,
                                           static_cast<Timestamp*>(output));
            break;
        case SystemFieldType::RowId:
            bulk_subscript_impl<int64_t>(&this->insert_record_.row_ids_,
                                         seg_offsets,
                                         count,
                                         static_cast<int64_t*>(output));
            break;
        default:
            PanicInfo(DataTypeInvalid, "unknown subscript fields");
    }
}

std::pair<std::unique_ptr<IdArray>, std::vector<SegOffset>>
SegmentGrowingImpl::search_ids(const IdArray& id_array,
                               Timestamp timestamp) const {
    auto field_id = schema_->get_primary_field_id().value_or(FieldId(-1));
    AssertInfo(field_id.get() != -1, "Primary key is -1");
    auto& field_meta = schema_->operator[](field_id);
    auto data_type = field_meta.get_data_type();
    auto ids_size = GetSizeOfIdArray(id_array);
    std::vector<PkType> pks(ids_size);
    ParsePksFromIDs(pks, data_type, id_array);

    auto res_id_arr = std::make_unique<IdArray>();
    std::vector<SegOffset> res_offsets;
    res_offsets.reserve(pks.size());
    for (auto& pk : pks) {
        auto segOffsets = insert_record_.search_pk(pk, timestamp);
        for (auto offset : segOffsets) {
            switch (data_type) {
                case DataType::INT64: {
                    res_id_arr->mutable_int_id()->add_data(
                        std::get<int64_t>(pk));
                    break;
                }
                case DataType::VARCHAR: {
                    res_id_arr->mutable_str_id()->add_data(
                        std::get<std::string>(std::move(pk)));
                    break;
                }
                default: {
                    PanicInfo(DataTypeInvalid,
                              fmt::format("unsupported type {}", data_type));
                }
            }
            res_offsets.push_back(offset);
        }
    }
    return {std::move(res_id_arr), std::move(res_offsets)};
}

std::string
SegmentGrowingImpl::debug() const {
    return "Growing\n";
}

int64_t
SegmentGrowingImpl::get_active_count(Timestamp ts) const {
    auto row_count = this->get_row_count();
    auto& ts_vec = this->get_insert_record().timestamps_;
    auto iter = std::upper_bound(
        boost::make_counting_iterator(static_cast<int64_t>(0)),
        boost::make_counting_iterator(row_count),
        ts,
        [&](Timestamp ts, int64_t index) { return ts < ts_vec[index]; });
    return *iter;
}

void
SegmentGrowingImpl::mask_with_timestamps(BitsetType& bitset_chunk,
                                         Timestamp timestamp) const {
    // DO NOTHING
}

}  // namespace milvus::segcore

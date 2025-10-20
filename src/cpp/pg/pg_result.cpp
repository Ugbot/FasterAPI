#include "pg_result.h"
#include <libpq-fe.h>
#include <cstring>

PgResult::PgResult(pg_result* result) noexcept 
    : result_(result), row_count_(0), field_count_(0) {
    if (result_) {
        row_count_ = PQntuples(result_);
        field_count_ = PQnfields(result_);
    }
}

PgResult::~PgResult() {
    if (result_) {
        PQclear(result_);
    }
}

PgResult::PgResult(PgResult&& other) noexcept
    : result_(other.result_),
      row_count_(other.row_count_),
      field_count_(other.field_count_) {
    other.result_ = nullptr;
    other.row_count_ = 0;
    other.field_count_ = 0;
}

PgResult& PgResult::operator=(PgResult&& other) noexcept {
    if (this != &other) {
        if (result_) {
            PQclear(result_);
        }
        
        result_ = other.result_;
        row_count_ = other.row_count_;
        field_count_ = other.field_count_;
        
        other.result_ = nullptr;
        other.row_count_ = 0;
        other.field_count_ = 0;
    }
    return *this;
}

int64_t PgResult::row_count() const noexcept {
    return row_count_;
}

int32_t PgResult::field_count() const noexcept {
    return field_count_;
}

const char* PgResult::field_name(int32_t col_index) const noexcept {
    if (!result_ || col_index < 0 || col_index >= field_count_) {
        return nullptr;
    }
    return PQfname(result_, col_index);
}

uint32_t PgResult::field_type(int32_t col_index) const noexcept {
    if (!result_ || col_index < 0 || col_index >= field_count_) {
        return 0;
    }
    return static_cast<uint32_t>(PQftype(result_, col_index));
}

const char* PgResult::get_value(int64_t row_index, int32_t col_index) const noexcept {
    if (!result_ || row_index < 0 || row_index >= row_count_ || 
        col_index < 0 || col_index >= field_count_) {
        return nullptr;
    }
    return PQgetvalue(result_, static_cast<int>(row_index), col_index);
}

bool PgResult::is_null(int64_t row_index, int32_t col_index) const noexcept {
    if (!result_ || row_index < 0 || row_index >= row_count_ || 
        col_index < 0 || col_index >= field_count_) {
        return true;
    }
    return PQgetisnull(result_, static_cast<int>(row_index), col_index) != 0;
}

int32_t PgResult::get_length(int64_t row_index, int32_t col_index) const noexcept {
    if (!result_ || row_index < 0 || row_index >= row_count_ || 
        col_index < 0 || col_index >= field_count_) {
        return -1;
    }
    return PQgetlength(result_, static_cast<int>(row_index), col_index);
}

const char* PgResult::scalar() const noexcept {
    if (!result_ || row_count_ != 1 || field_count_ != 1) {
        return nullptr;
    }
    return PQgetvalue(result_, 0, 0);
}

const char* PgResult::status() const noexcept {
    if (!result_) {
        return nullptr;
    }
    return PQresStatus(PQresultStatus(result_));
}

const char* PgResult::error_message() const noexcept {
    if (!result_) {
        return nullptr;
    }
    return PQresultErrorMessage(result_);
}

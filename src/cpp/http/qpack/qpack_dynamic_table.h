#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace fasterapi {
namespace qpack {

/**
 * QPACK dynamic table entry (RFC 9204 Section 3.2).
 */
struct DynamicEntry {
    std::string name;
    std::string value;
    size_t size;           // name.length() + value.length() + 32 (RFC overhead)
    uint64_t insert_count; // Absolute insertion index
    uint32_t ref_count;    // Reference count for blocking (RFC 9204 Section 2.1.1)

    DynamicEntry() : size(0), insert_count(0), ref_count(0) {}

    DynamicEntry(std::string_view n, std::string_view v, uint64_t ins_count)
        : name(n), value(v), size(n.length() + v.length() + 32),
          insert_count(ins_count), ref_count(0) {}
};

/**
 * QPACK Dynamic Table (RFC 9204 Section 3.2).
 * 
 * Uses a ring buffer (circular FIFO) to manage entries efficiently.
 * Evicts oldest entries when capacity is exceeded.
 */
class QPACKDynamicTable {
public:
    /**
     * Constructor.
     * 
     * @param capacity Maximum table capacity in bytes
     */
    explicit QPACKDynamicTable(size_t capacity = 4096)
        : capacity_(capacity),
          size_(0),
          insert_count_(0),
          drop_count_(0) {
        entries_.reserve(64);  // Pre-allocate for typical usage
    }
    
    /**
     * Insert new entry (RFC 9204 Section 3.2.2).
     *
     * @param name Header name
     * @param value Header value
     * @return true if inserted, false if entry too large or eviction blocked
     */
    bool insert(std::string_view name, std::string_view value) noexcept {
        DynamicEntry entry(name, value, insert_count_);

        // Check if entry fits in capacity
        if (entry.size > capacity_) {
            return false;  // Entry too large
        }

        // Evict entries until we have space
        while (size_ + entry.size > capacity_ && !entries_.empty()) {
            // Check if oldest entry is referenced (cannot evict)
            if (entries_.front().ref_count > 0) {
                return false;  // Blocked by referenced entry
            }
            evict_oldest();
        }

        // Add entry
        entries_.push_back(std::move(entry));
        size_ += entry.size;
        insert_count_++;

        return true;
    }
    
    /**
     * Get entry by absolute index.
     * 
     * @param index Absolute index (insert_count - index - 1)
     * @return Entry or nullptr if not found
     */
    const DynamicEntry* get(size_t index) const noexcept {
        // Convert absolute index to relative index
        if (index < drop_count_ || index >= insert_count_) {
            return nullptr;  // Out of range
        }
        
        size_t relative = index - drop_count_;
        if (relative >= entries_.size()) {
            return nullptr;
        }
        
        return &entries_[relative];
    }
    
    /**
     * Find entry by name and value.
     * 
     * @param name Header name
     * @param value Header value
     * @return Absolute index if found, -1 otherwise
     */
    int find(std::string_view name, std::string_view value) const noexcept {
        for (size_t i = 0; i < entries_.size(); i++) {
            if (entries_[i].name == name && entries_[i].value == value) {
                return static_cast<int>(drop_count_ + i);
            }
        }
        return -1;
    }
    
    /**
     * Find entry by name only.
     * 
     * @param name Header name
     * @return Absolute index if found, -1 otherwise
     */
    int find_name(std::string_view name) const noexcept {
        for (size_t i = 0; i < entries_.size(); i++) {
            if (entries_[i].name == name) {
                return static_cast<int>(drop_count_ + i);
            }
        }
        return -1;
    }
    
    /**
     * Get table size in bytes.
     */
    size_t size() const noexcept { return size_; }
    
    /**
     * Get table capacity.
     */
    size_t capacity() const noexcept { return capacity_; }
    
    /**
     * Get number of entries.
     */
    size_t count() const noexcept { return entries_.size(); }
    
    /**
     * Get insert count (total insertions).
     */
    size_t insert_count() const noexcept { return insert_count_; }
    
    /**
     * Get drop count (total evictions).
     */
    size_t drop_count() const noexcept { return drop_count_; }
    
    /**
     * Update table capacity.
     * 
     * @param new_capacity New capacity in bytes
     */
    void set_capacity(size_t new_capacity) noexcept {
        capacity_ = new_capacity;
        
        // Evict entries if needed
        while (size_ > capacity_ && !entries_.empty()) {
            evict_oldest();
        }
    }
    
    /**
     * Get entry by relative index (RFC 9204 Section 3.2.3).
     * Relative index 0 = most recently inserted.
     *
     * @param relative_index Relative index (0 = newest)
     * @return Entry or nullptr if not found
     */
    const DynamicEntry* get_relative(size_t relative_index) const noexcept {
        if (entries_.empty() || relative_index >= entries_.size()) {
            return nullptr;
        }
        // Relative 0 = most recent = back of vector
        size_t vec_index = entries_.size() - 1 - relative_index;
        return &entries_[vec_index];
    }

    /**
     * Convert absolute index to relative index.
     *
     * @param absolute_index Absolute index
     * @return Relative index or -1 if invalid
     */
    int absolute_to_relative(uint64_t absolute_index) const noexcept {
        if (absolute_index < drop_count_ || absolute_index >= insert_count_) {
            return -1;
        }
        // Most recent entry has absolute index (insert_count_ - 1)
        // and relative index 0
        return static_cast<int>(insert_count_ - 1 - absolute_index);
    }

    /**
     * Convert relative index to absolute index.
     *
     * @param relative_index Relative index
     * @return Absolute index or -1 if invalid
     */
    int relative_to_absolute(size_t relative_index) const noexcept {
        if (relative_index >= entries_.size()) {
            return -1;
        }
        return static_cast<int>(insert_count_ - 1 - relative_index);
    }

    /**
     * Increment reference count for an entry (RFC 9204 Section 2.1.1).
     *
     * @param absolute_index Absolute index
     * @return true if successful, false if index invalid
     */
    bool increment_reference(uint64_t absolute_index) noexcept {
        if (absolute_index < drop_count_ || absolute_index >= insert_count_) {
            return false;
        }
        size_t vec_index = absolute_index - drop_count_;
        if (vec_index >= entries_.size()) {
            return false;
        }
        entries_[vec_index].ref_count++;
        return true;
    }

    /**
     * Decrement reference count for an entry.
     *
     * @param absolute_index Absolute index
     * @return true if successful, false if index invalid
     */
    bool decrement_reference(uint64_t absolute_index) noexcept {
        if (absolute_index < drop_count_ || absolute_index >= insert_count_) {
            return false;
        }
        size_t vec_index = absolute_index - drop_count_;
        if (vec_index >= entries_.size()) {
            return false;
        }
        if (entries_[vec_index].ref_count > 0) {
            entries_[vec_index].ref_count--;
        }
        return true;
    }

    /**
     * Acknowledge insertions up to insert_count (RFC 9204 Section 4.4.1).
     * This allows eviction of entries that were blocked by references.
     *
     * @param acknowledged_count Acknowledged insert count
     */
    void acknowledge_insert(uint64_t acknowledged_count) noexcept {
        // Decrement reference counts for acknowledged entries
        for (auto& entry : entries_) {
            if (entry.insert_count < acknowledged_count && entry.ref_count > 0) {
                entry.ref_count--;
            }
        }
    }

    /**
     * Clear all entries.
     */
    void clear() noexcept {
        entries_.clear();
        size_ = 0;
        insert_count_ = 0;
        drop_count_ = 0;
    }

private:
    /**
     * Evict oldest entry.
     */
    void evict_oldest() noexcept {
        if (entries_.empty()) return;
        
        size_ -= entries_.front().size;
        entries_.erase(entries_.begin());
        drop_count_++;
    }
    
    std::vector<DynamicEntry> entries_;  // Ring buffer entries
    size_t capacity_;                    // Maximum table size in bytes
    size_t size_;                        // Current table size in bytes
    size_t insert_count_;                // Total insertions
    size_t drop_count_;                  // Total evictions
};

} // namespace qpack
} // namespace fasterapi

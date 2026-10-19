#include "component/transport.hpp"
#include <span>

namespace lig::component {

namespace internal {

Data::Data(std::byte *data, size_t size) : data_{data}, size_{size} {}
Data::~Data() { delete[] data_; }
Data::Data(Data &&other) noexcept : data_{other.data_}, size_{other.size_} {
    other.data_ = nullptr;
    other.size_ = 0;
}
auto Data::operator=(Data &&other) noexcept -> Data & {
    if (&other != this) {
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}
auto Data::data() const noexcept -> const std::byte * { return data_; }
auto Data::size() const noexcept -> size_t { return size_; }
DataBuffer::DataBuffer() { data_ = new std::byte[batch_size_]; }
DataBuffer::~DataBuffer() { delete[] data_; }
DataBuffer::DataBuffer(DataBuffer &&other) noexcept : data_{other.data_}, len_{other.len_} {
    other.data_ = nullptr;
    other.len_ = 0;
}
auto DataBuffer::operator=(DataBuffer &&other) noexcept -> DataBuffer & {
    if (this != &other) {
        delete[] data_;
        data_ = other.data_;
        len_ = other.len_;
        other.data_ = nullptr;
        other.len_ = 0;
    }
    return *this;
}
auto DataBuffer::flush() -> std::optional<Data> {
    if (!checkValid() || len_ < MinBatchSize) {
        return std::nullopt;
    }
    Data data(data_, len_);
    data_ = new std::byte[batch_size_];
    len_ = 0;
    return std::move(data);
}
auto DataBuffer::push(const std::byte *data, size_t len) -> std::vector<Data> {
    checkValid();
    uint64_t cur = 0;
    std::vector<Data> res{};
    const size_t res_count = (len + len_) / batch_size_;
    res.reserve(res_count);
    while (len + len_ >= batch_size_) {
        const uint64_t left = batch_size_ - len_;
        std::copy_n(data + cur, left, data_ + len_);
        res.emplace_back(data_, len_ + left);
        data_ = new std::byte[batch_size_];
        len_ = 0;
        cur += left;
        len -= left;
    }
    if (len != 0) {
        std::copy_n(data + cur, len, data_ + len_);
        len_ += len;
    }
    return res;
}
auto DataBuffer::len() const noexcept -> size_t { return len_; }
auto DataBuffer::batchSize() const noexcept -> size_t { return batch_size_; }
auto DataBuffer::setBatchSize(size_t size) -> bool {
    if (size <= len_ || size > MaxBatchSize || size < MinBatchSize) {
        return false;
    }
    auto data = new std::byte[size];
    std::copy_n(data_, len_, data);
    delete[] data_;
    data_ = data;
    batch_size_ = size;
    return true;
}
auto DataBuffer::checkValid() -> bool {
    if (!data_) {
        data_ = new std::byte[batch_size_];
        len_ = 0;
        return false;
    }
    return true;
}
SinkWrapper::SinkWrapper(std::unique_ptr<::lig::interface::Sink> &sink) : sink_{sink.release()} {}
SinkWrapper::~SinkWrapper() = default;
void SinkWrapper::from(const std::vector<std::byte> &data) {
    if (data.empty()) {
        return;
    }
    if (!fail_data.empty()) {
        auto res = sink_->write(fail_data.data(), fail_data.size());
        fail_data.erase(fail_data.begin(), fail_data.begin() + res.len);
    }
    auto res = sink_->write(data.data(), data.size());
    if (!res) {
        if (!sink_->open()) {
            store_fail_data(std::span{data.begin() + res.len, data.end()});
        } else {
            auto res2 = sink_->write(data.data() + res.len, data.size() - res.len);
            if (!res2) {
                store_fail_data(std::span{data.begin() + res.len + res2.len, data.end()});
            }
        }
    }
}
void SinkWrapper::open() { sink_->open(); }

void SinkWrapper::store_fail_data(const std::span<const std::byte> &data) {
    if (data.empty()) {
        return;
    }
    const size_t len = data.size() + fail_data.size();
    if (len > MaxFailDataSize) {
        const size_t erase_size = std::min(fail_data.size(), len - MaxFailDataSize);
        fail_data.erase(fail_data.begin(), fail_data.begin() + erase_size);
    }
    const size_t offset = data.size() > MaxFailDataSize ? data.size() - MaxFailDataSize : 0;
    fail_data.insert(fail_data.end(), data.begin() + offset, data.end());
}
} // namespace internal




// ITransport

// transport
// ---sync transport

thread_local std::vector<std::shared_ptr<SyncTransport::Entity>> SyncTransport::entities_{};

SyncTransport::SyncTransport(size_t max_cache_size) { set_max_cache_size(max_cache_size); }
auto SyncTransport::set_max_cache_size(size_t size) noexcept -> bool {
    if (size > MaxCacheSize || size < MinCacheSize) {
        return false;
    }
    max_cache_size_ = size;
    return true;
}
void SyncTransport::from(const ::lig::logger::Task &task) {
    std::shared_ptr<Entity> entity = find_entity();
    auto &cache = entity->cache;

    task(formatter_.get(), cache);
    if (cache.size() > max_cache_size_) {
        flush();
    }
}
void SyncTransport::flush() {
    std::shared_ptr<Entity> entity = find_entity();
    auto &cache = entity->cache;
    if (!cache.empty()) {
        for (auto &wrapper : wrappers_) {
            wrapper->from(cache);
        }
        cache.clear();
    }
}
auto SyncTransport::find_entity() -> std::shared_ptr<Entity> {
    for (auto &entity : entities_) {
        if (entity->instance == this) {
            return entity;
        }
    }
    std::vector<std::byte> datas{};
    datas.reserve(max_cache_size_ * 2);
    entities_.push_back(std::make_shared<Entity>(std::move(datas), this));
    return entities_.back();
}
void SyncTransport::set_formatter(std::unique_ptr<const ::lig::interface::Formatter> &formatter) {
    formatter_.swap(formatter);
}
void SyncTransport::add_sink(std::unique_ptr<::lig::interface::Sink> &sink) {
    wrappers_.push_back(std::make_unique<internal::SinkWrapper>(sink));
}

// ---async transport
// -------- BasicBuffer Implement

AsyncTransport::~AsyncTransport() {
    stop();
    if (t_.joinable()) {
        t_.join();
    }
}
void AsyncTransport::start() {
    auto loop = [&]() -> void {
        for (auto &wrapper : wrappers_) {
            wrapper->open();
        }
        std::vector<std::byte> tmp_data{};
        while (!is_stop_.load(std::memory_order_relaxed)) {
            const size_t max_cache_size = max_cache_size_.load(std::memory_order_acquire);
            if (cache_.capacity() < max_cache_size) {
                cache_.reserve(max_cache_size);
            }

            auto res{task_queue_.pop_till_empty()};
            for (auto &task : res) {
                if (!task) {
                    auto res = data_buffer_.flush();
                    while (res) {
                        in_cache(std::move(res.value()));
                        res = data_buffer_.flush();
                    }
                    cache_flush();
                    flush_notifier_.fetch_add(1, std::memory_order_release);
                    flush_notifier_.notify_one();
                } else {
                    tmp_data.clear();
                    task(formatter_.get(), tmp_data);
                    push(tmp_data);
                }
            }
            // std::cout << "pop\n" << std::flush;
            if (is_overflow_.exchange(false, std::memory_order_relaxed)) {
                std::lock_guard guard{overflow_mutex_};
                push(overflow_data_);
                overflow_data_.clear();
            }
        }
        auto res{task_queue_.try_pop_till_empty()};
        for (auto &task : res) {
            if (task) {
                tmp_data.clear();
                task(formatter_.get(), tmp_data);
                push(tmp_data);
            }
        }
        if (is_overflow_.exchange(false, std::memory_order_relaxed)) {
            std::lock_guard guard{overflow_mutex_};
            push(overflow_data_);
            overflow_data_.clear();
        }

        if (auto res = data_buffer_.flush(); res) {
            in_cache(std::move(res.value()));
            cache_flush();
        }
        flush_notifier_.fetch_add(1, std::memory_order_release);
        flush_notifier_.notify_one();
    };
    t_ = std::thread{loop};
}
void AsyncTransport::from(const ::lig::logger::Task &task) {
    const auto now = std::chrono::steady_clock::now();
    while (!task_queue_.try_push(task) && task) {
        if (std::chrono::steady_clock::now() - now > std::chrono::milliseconds(3)) {
            std::vector<std::byte> tmp{};
            tmp.reserve(512);
            task(formatter_.get(), tmp);
            std::lock_guard guard{overflow_mutex_};
            overflow_data_.insert(overflow_data_.end(), tmp.begin(), tmp.end());
            is_overflow_.store(true, std::memory_order_release);
            break;
        }
        std::this_thread::yield();
    }
}
void AsyncTransport::flush() {
    if (is_stop_.load(std::memory_order_acquire)) {
        return;
    }
    auto cur = flush_notifier_.load(std::memory_order_acquire);

    task_queue_.push(::lig::logger::Task{});
    flush_notifier_.wait(cur);
}
void AsyncTransport::stop() {
    if (is_stop_.exchange(true, std::memory_order_release)) {
        return;
    }
    task_queue_.push(::lig::logger::Task{});
}
void AsyncTransport::set_formatter(std::unique_ptr<const ::lig::interface::Formatter> &formatter) {
    formatter_.swap(formatter);
}
void AsyncTransport::add_sink(std::unique_ptr<::lig::interface::Sink> &sink) {
    wrappers_.push_back(std::make_unique<internal::SinkWrapper>(sink));
}
auto AsyncTransport::set_max_cache_size(size_t size) noexcept -> bool {
    if (size > MaxCacheSize || size < MinCacheSize) {
        return false;
    }
    max_cache_size_ = size;
    return true;
}

void AsyncTransport::push(const std::vector<std::byte> &datas) {
    auto res = data_buffer_.push(datas.data(), datas.size());
    for (auto &item : res) {
        in_cache(std::move(item));
    }
}
void AsyncTransport::in_cache(internal::Data &&data) {
    size_t offset = 0, data_len = data.size();
    while (cache_.size() + data_len >= cache_.capacity()) {
        const size_t left = cache_.capacity() - cache_.size();
        cache_.insert(cache_.end(), data.data() + offset, data.data() + offset + left);
        cache_flush();
        offset += left;
        data_len -= left;
    }
    if (data_len != 0) {
        cache_.insert(cache_.end(), data.data() + offset, data.data() + data.size());
    }
}
void AsyncTransport::cache_flush() {
    if (!cache_.empty()) {
        for (auto &wrapper : wrappers_) {
            wrapper->from(cache_);
        }
        cache_.clear();
    }
}

} // namespace lig::component

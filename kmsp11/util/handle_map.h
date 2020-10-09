#ifndef KMSP11_UTIL_HANDLE_MAP_H_
#define KMSP11_UTIL_HANDLE_MAP_H_

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "kmsp11/cryptoki.h"
#include "kmsp11/util/crypto_utils.h"
#include "kmsp11/util/errors.h"

namespace kmsp11 {

// A HandleMap contains a set of items with assigned CK_ULONG handles.
// It is intended for use with the PKCS #11 Session and Object types, both
// of which are identified by a handle.
template <typename T>
class HandleMap {
 public:
  // Create a new map. The provided CK_RV will be used for Get and Remove
  // operations performed against an unknown handle.
  HandleMap(CK_RV not_found_rv) : not_found_rv_(not_found_rv) {}

  // Adds item to the map and returns its handle.
  inline CK_ULONG Add(std::shared_ptr<T> item) {
    absl::WriterMutexLock lock(&mutex_);

    // Generate a new handle by picking a random handle and ensuring that it is
    // not already in use. Repeat this process until we have a useable handle.
    CK_ULONG handle;
    do {
      handle = RandomHandle();
    } while (items_.contains(handle));

    items_.try_emplace(handle, item);
    return handle;
  }

  // Constructs a new T using the provided arguments, adds it to the map, and
  // returns its handle.
  template <typename... Args>
  inline CK_ULONG Add(Args&&... args) {
    return Add(std::make_shared<T>(std::forward<Args>(args)...));
  }

  // Adds an item to the map using the provided handle. Returns
  // absl::InternalError if the provided handle is already in use.
  // TODO(bdhess): Remove this overload when ObjectStore is implemented.
  inline absl::Status AddDirect(CK_ULONG handle, std::shared_ptr<T> item) {
    absl::WriterMutexLock lock(&mutex_);

    if (items_.find(handle) != items_.end()) {
      return NewInternalError(
          absl::StrFormat("handle %#x is already in use", handle),
          SOURCE_LOCATION);
    }
    items_[handle] = item;
    return absl::OkStatus();
  }

  // Finds all keys in the map whose value matches the provided predicate.
  // If sort_compare is provided, the results are sorted before being
  // returned.
  inline std::vector<CK_ULONG> Find(
      std::function<bool(const T&)> predicate,
      std::function<bool(const T&, const T&)> sort_compare = nullptr) const {
    absl::ReaderMutexLock lock(&mutex_);

    std::vector<CK_ULONG> results;
    for (auto it : items_) {
      if (predicate(*it.second)) {
        results.push_back(it.first);
      }
    }

    if (sort_compare != nullptr) {
      std::sort(results.begin(), results.end(),
                [&](const CK_ULONG& h1, const CK_ULONG& h2) -> bool const {
                  // TODO(bdhess): figure out a way to make this not warn about
                  // holding the mutex.
                  return sort_compare(*items_.at(h1), *items_.at(h2));
                });
    }

    return results;
  }

  // Gets the map element with the provided handle, or returns NotFound if there
  // is no element with the provided handle.
  inline absl::StatusOr<std::shared_ptr<T>> Get(CK_ULONG handle) const {
    absl::ReaderMutexLock lock(&mutex_);

    auto it = items_.find(handle);
    if (it == items_.end()) {
      return HandleNotFoundError(handle, not_found_rv_, SOURCE_LOCATION);
    }

    return it->second;
  }

  // Removes the map element with the provided handle, or returns NotFound if
  // there is no element with the provided handle.
  inline absl::Status Remove(CK_ULONG handle) {
    absl::WriterMutexLock lock(&mutex_);

    auto it = items_.find(handle);
    if (it == items_.end()) {
      return HandleNotFoundError(handle, not_found_rv_, SOURCE_LOCATION);
    }

    items_.erase(it);
    return absl::OkStatus();
  }

 private:
  CK_RV not_found_rv_;
  mutable absl::Mutex mutex_;
  absl::flat_hash_map<CK_ULONG, std::shared_ptr<T>> items_
      ABSL_GUARDED_BY(mutex_);
};

}  // namespace kmsp11

#endif  // KMSP11_UTIL_HANDLE_MAP_H_

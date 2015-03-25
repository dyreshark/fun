// Copyright 2015 George Burgess (dyreshark)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Lock-free multi-writer, single-reader queue implementation in C++.

#include <iostream>
#include <memory>
#include <atomic>
#include <experimental/optional>

// For main/"testing"
#include <algorithm>
#include <thread>
#include <vector>

using std::experimental::optional;

namespace lolz {
namespace threading {

// A single-reader multiple-writers lock-free concurrent queue.
// All operations (sans construction and destruction) are threadsafe.
//
// Due to the design of this container, it *does not* support ordered iteration,
// and Size() queries are linear-time.
//
// This performs best when writes are likely to happen in bursts compared to
// reads, as only some reads perform atomic operations, but each write performs
// at least one atomic operation.
template <typename T>
class ConcurrentQueue {
  // I forego using std::unique_ptr because I'm swapping Nodes around often,
  // and dealing with atomics. unique_ptr makes this more difficult/ugly.
  struct Node {
    ConcurrentQueue<T>::Node *next;
    T data;
  };

 public:
  ConcurrentQueue() : writers_(nullptr), reader_(nullptr) {}

  // Destructor. This expects that all writers are done writing, and
  // the reader is no longer reading.
  //
  // Should either only be called from the reader thread, or after some external
  // world-synchronization event that happens after all writers finish writing,
  // and the reader finishes reading.
  ~ConcurrentQueue();

  // Copy ctor is disabled for the same reason that std::atomic's copy ctor is
  // disabled
  ConcurrentQueue(const ConcurrentQueue &) = delete;

  // Move ctor is disabled for the same reason that std::atomic's move ctor is
  // disabled
  ConcurrentQueue(ConcurrentQueue &&other) = delete;

  // Puts the element in the queue
  //
  // Writers/Readers may use this function.
  void Enqueue(T elem);

  // Takes the next element out of the queue. Returns nothing if there's nothing
  // in the queue.
  //
  // Reader-only function.
  optional<T> Dequeue();

  // Returns the size of the queue **in linear time**. Because the queue is
  // lock-free, there's no guarantee the size will be the same when this
  // returns.
  //
  // Reader-only function.
  std::size_t Size() const;

  // Checks to see if the queue is empty.
  //
  // Readers/writers may use this function.
  bool Empty() const;

 private:
  // High level algorithm design:
  // writers_ and reader_ are stacks.
  // Writers write to writers_ forever.
  // Reader reads from reader_ until it's empty, at which point it will
  // take the entire writers_ stack, reverse it, and make it the readers_
  // stack.
  //
  // If writes come in in a bursty fashion, this can save a substantial number
  // of atomic operations, because the reader only needs to do one atomic
  // operation per copy, and zero operations per read. Writers only need to do
  // one atomic operation per write. (Both of said atomic operations are
  // lock-free; there are loops around a compare/exchange, but they're small
  // enough that it would take really high contention to get them to become an
  // issue)

  // Once again, no unique_ptr so code is prettier.
  std::atomic<Node *> writers_;
  Node *reader_;

  // We only ever load with std::memory_order_relaxed. Because that's what we
  // want by default.
  Node *writers_relaxed() { return writers_.load(std::memory_order_relaxed); }

  // Exchanges writers with new_value, guaranteeing the given success_order on
  // success. On failure, defaults to std::memory_order_relaxed, because
  // that's what we want everywhere.
  bool exchange_writers(Node *old_value, Node *new_value,
                        std::memory_order success_order) {
    return writers_.compare_exchange_weak(old_value, new_value, success_order,
                                          std::memory_order_relaxed);
  }

  // Copies everything from writers_ to the end of reader_. Also reverses
  // everything in writers_, so everything is read in the correct order.
  void CopyFromWriters();

  // Reverses nodes. Could be simply templated and removed from the class,
  // but I prefer it this way
  static Node *ReverseNodes(Node *head);
};

template <typename T>
ConcurrentQueue<T>::~ConcurrentQueue() {
  auto delete_list = [](Node *head) {
    while (head != nullptr) {
      auto *e = head;
      head = e->next;
      delete e;
    }
  };

  delete_list(reader_);

  // Acquire is used instead of relaxed due to the following example:
  //
  // <reader thread>
  //   // Finished reading -- wait for writers and do other things.
  //   while (!writers_all_quit.load(std::memory_order_relaxed)) {}
  //   delete the_concurrent_queue;
  //
  // There's no acquire between the writers writing and the destructor trying
  // to read the list. (And the reader *did* wait for some sort of event that
  // guaranteed the writers are, in fact, all done writing.)
  //
  // Slightly contrived, but still definitely possible in Real World Code(TM)
  delete_list(writers_.load(std::memory_order_acquire));
}

template <typename T>
void ConcurrentQueue<T>::Enqueue(T elem) {
  auto node = new Node{nullptr, std::move(elem)};
  while (1) {
    // Relaxed is tolerable because all we care about is the pointer here;
    // the compare_exchange will do an acquire/release for us.
    auto *head = writers_relaxed();
    node->next = head;

    // acquire is necessary here because the reader will only do an acquire on
    // writers_, and none of its later nodes. Without acquire, the following
    // could happen:
    //
    // writers_ = NodeA[released] -> NodeB[released]
    // // Reader acquires writers_
    // reader_ = NodeB[released] -> NodeA[released+acquired]
    //
    // No acquire happened on NodeB, and the reader will never do atomic
    // operations on the reader_ list, so NodeB's contents are undefined.
    if (exchange_writers(head, node, std::memory_order_acq_rel)) break;
  }
}

template <typename T>
optional<T> ConcurrentQueue<T>::Dequeue() {
  if (reader_ == nullptr) CopyFromWriters();

  if (reader_ == nullptr) return {};

  std::unique_ptr<Node> head{reader_};
  reader_ = std::move(head->next);
  return head->data;
}

template <typename T>
typename ConcurrentQueue<T>::Node *ConcurrentQueue<T>::ReverseNodes(
    typename ConcurrentQueue<T>::Node *head) {
  Node *reversed = nullptr;
  while (head != nullptr) {
    auto *next = head->next;
    head->next = reversed;
    reversed = head;
    head = next;
  }
  return reversed;
}

template <typename T>
void ConcurrentQueue<T>::CopyFromWriters() {
  Node *writer_head;
  // We need the compare_exchange loop because we don't want to lose an update
  // between setting writer head and nulling writers_.
  while (1) {
    writer_head = writers_relaxed();
    if (exchange_writers(writer_head, nullptr, std::memory_order_acquire))
      break;
  }

  if (writer_head == nullptr) return;

  auto *reversed = ReverseNodes(writer_head);
  if (reader_ == nullptr) {
    reader_ = reversed;
    return;
  }

  auto *update = reader_;
  while (update->next != nullptr) update = update->next;

  update->next = reversed;
}

template <typename T>
bool ConcurrentQueue<T>::Empty() const {
  return reader_ == nullptr &&
         writers_.load(std::memory_order_relaxed) == nullptr;
}

template <typename T>
std::size_t ConcurrentQueue<T>::Size() const {
  auto count = [](Node *head) {
    std::size_t result = 0;
    while (head != nullptr) {
      ++result;
      head = head->next;
    }
    return result;
  };

  // If we didn't load writers with memory_order_acquire, this thread may not
  // see valid next pointers on the writers.
  return count(reader_) + count(writers_.load(std::memory_order_acquire));
}

// "Test" driver
void Writer(ConcurrentQueue<int> &queue, const std::vector<int> &to_write,
            std::atomic_int &i) {
  auto max_elem = int(to_write.size());
  while (1) {
    auto next = i.fetch_add(1, std::memory_order_relaxed);
    if (next >= max_elem) break;
    queue.Enqueue(next);
  }
}

void Reader(ConcurrentQueue<int> &queue, std::vector<int> &results,
            int num_to_read) {
  int num_read = 0;
  while (num_read < num_to_read) {
    auto next = queue.Dequeue();
    if (!next) continue;

    ++num_read;
    results.push_back(*next);
  }

  if (!queue.Empty())
    std::cerr << "Queue was not empty when we were done reading!";
}
}  // namespace lolz::threading
}  // namespace lolz

// Number of elements to write to the queue
constexpr static int kAmountToWrite = 3000000;

// Numbers of writers to use when putting things in the queue
constexpr static int kNumberOfWriters = 7;

int main() {
  std::vector<int> dequeued;
  dequeued.reserve(kAmountToWrite);

  std::vector<int> to_queue(kAmountToWrite);
  for (int i = 0; i < kAmountToWrite; ++i) to_queue[i] = i;

  lolz::threading::ConcurrentQueue<int> queue;
  std::thread reader{
      [&queue, &dequeued] { Reader(queue, dequeued, kAmountToWrite); }};

  std::vector<std::thread> writers;
  writers.reserve(kNumberOfWriters);

  std::atomic_int writer_int{0};
  for (int i = 0; i < kNumberOfWriters; ++i) {
    writers.emplace_back([&queue, &to_queue, &writer_int] {
      Writer(queue, to_queue, writer_int);
    });
  }

  std::cout << "Waiting for reader/writers\n";
  for (auto &w : writers) w.join();
  std::cout << "Waiting for reader...\n";
  reader.join();

  if (!queue.Empty()) {
    std::cerr << "Reader quit with " << queue.Size()
              << " items still in the queue\n";
    exit(1);
  }

  if (dequeued.size() != to_queue.size()) {
    std::cerr << "Readers or writers got confused: " << dequeued.size()
              << " vs " << to_queue.size() << '\n';
    exit(1);
  }

  uint64_t total_difference = 0;
  std::size_t different_slots = 0;
  for (auto i = 0ul; i < dequeued.size(); ++i) {
    auto reader_val = dequeued[i];
    auto writer_val = to_queue[i];
    auto delta = std::abs(reader_val - writer_val);
    if (delta != 0) {
      ++different_slots;
      total_difference += delta;
    }
  }

  auto avg_difference = double(total_difference) / kAmountToWrite;
  std::cout << "Out of " << kAmountToWrite << " slots, " << different_slots
            << " were not what was expected.\n";
  std::cout << "The average difference across the board was " << avg_difference
            << '\n';

  if (different_slots != 0) {
    std::sort(std::begin(dequeued), std::end(dequeued));
    if (dequeued == to_queue)
      std::cout << "...And everything made it through successfully!\n";
    else
      std::cout << "...And things were missing/wrong\n";
  }
}

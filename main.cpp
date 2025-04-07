#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

using namespace std;

class PriorityMutex {
public:
  virtual void lockLowPriority() = 0;
  virtual void unlockLowPriority() = 0;

  virtual void lockHighPriority() = 0;
  virtual void unlockHighPriority() = 0;
};

class BasicPriorityMutex : public PriorityMutex {
public:
  void lockLowPriority() override {
    mutex_.lock();
  }
  void unlockLowPriority() override {
    mutex_.unlock();
  }

  void lockHighPriority() override {
    mutex_.lock();
  }
  void unlockHighPriority() override {
    mutex_.unlock();
  }
private:
  mutex mutex_;
};

class TwoMutexPriorityMutex : public PriorityMutex {
public:
  void lockLowPriority() override {
    nextToAccessMutex_.lock();
    dataMutex_.lock();
    nextToAccessMutex_.unlock();
  }
  void unlockLowPriority() override {
    dataMutex_.unlock();
  }

  void lockHighPriority() override {
    nextToAccessMutex_.lock();
    dataMutex_.lock();
    nextToAccessMutex_.unlock();
  }
  void unlockHighPriority() override {
    dataMutex_.unlock();
  }

private:
  mutex dataMutex_;
  mutex nextToAccessMutex_;
};

class MutexAndAtomicBoolPriorityMutex : public PriorityMutex {
public:
  void lockLowPriority() override {
    lowPriorityLock_.lock();
    cv_.wait(lowPriorityLock_, [this]() -> bool {
      return !waiting_;
    });
  }
  void unlockLowPriority() override {
    lowPriorityLock_.unlock();
    cv_.notify_all();
  }

  void lockHighPriority() override {
    waiting_ = true;
    highPriorityLock_.lock();
    waiting_ = false;
  }
  void unlockHighPriority() override {
    highPriorityLock_.unlock();
    cv_.notify_all();
  }

private:
  mutex dataMutex_;
  atomic<bool> waiting_{false};
  condition_variable cv_;
  unique_lock<mutex> lowPriorityLock_{dataMutex_, defer_lock};
  unique_lock<mutex> highPriorityLock_{dataMutex_, defer_lock};
};

class MutexAndTwoBoolPriorityMutex : public PriorityMutex {
public:
  void lockLowPriority() override {
    lowPriorityLock_.lock();
    cv_.wait(lowPriorityLock_, [this]() -> bool {
      return !(dataHeld_ || highPriorityWaiting_);
    });
    dataHeld_ = true;
    lowPriorityLock_.unlock();
  }
  void unlockLowPriority() override {
    lowPriorityLock_.lock();
    dataHeld_ = false;
    lowPriorityLock_.unlock();
    cv_.notify_all();
  }

  void lockHighPriority() override {
    highPriorityLock_.lock();
    highPriorityWaiting_ = true;
    cv_.wait(lowPriorityLock_, [this]() -> bool {
      return !(dataHeld_);
    });
    dataHeld_ = true;
    highPriorityLock_.unlock();
  }
  void unlockHighPriority() override {
    highPriorityLock_.lock();
    dataHeld_ = false;
    highPriorityWaiting_ = false;
    highPriorityLock_.unlock();
    cv_.notify_all();
  }

private:
  mutex dataMutex_;
  bool dataHeld_{false}, highPriorityWaiting_{false};
  condition_variable cv_;
  unique_lock<mutex> lowPriorityLock_{dataMutex_, defer_lock};
  unique_lock<mutex> highPriorityLock_{dataMutex_, defer_lock};
};

// Two types of workers:
//  1. "Trainer": Tight loop, needs resource for entire body.
//  2. "Server": Only needs resource for small fraction of body.
class ContentionTest {
public:
  ContentionTest(PriorityMutex *priorityMutex,
                 chrono::microseconds lowPrioWorkTime,
                 chrono::microseconds highPrioWorkTime,
                 chrono::microseconds highPrioSleepTime) :
                    priorityMutex_(priorityMutex),
                    lowPrioWorkTime_(lowPrioWorkTime),
                    highPrioWorkTime_(highPrioWorkTime),
                    highPrioSleepTime_(highPrioSleepTime) {}

  pair<double, double> run() {
    thread thr1(std::bind(&ContentionTest::lowPriorityThreadFunction, this));
    thread thr2(std::bind(&ContentionTest::highPriorityThreadFunction, this));
    this_thread::sleep_for(kTestDurationSeconds);
    shouldRun_ = false;
    thr1.join();
    thr2.join();
    return {lowPriorityThreadWorkTime_, highPriorityThreadLatencyTime_};
  }

private:
  static constexpr chrono::seconds kTestDurationSeconds{120};
  PriorityMutex *priorityMutex_;
  const chrono::microseconds lowPrioWorkTime_;
  const chrono::microseconds highPrioWorkTime_;
  const chrono::microseconds highPrioSleepTime_;
  mutex workMutex_;
  atomic<bool> shouldRun_{true};
  double lowPriorityThreadWorkTime_;
  double highPriorityThreadLatencyTime_;

  void lowPriorityThreadFunction() {
    int64_t workTime = 0;

    while (shouldRun_) {
      priorityMutex_->lockLowPriority();
      
      // Do work...
      auto startTime = chrono::high_resolution_clock::now();
      this_thread::sleep_for(lowPrioWorkTime_);
      workTime += chrono::duration_cast<chrono::nanoseconds>(chrono::high_resolution_clock::now() - startTime).count();
      
      priorityMutex_->unlockLowPriority();
    }
    lowPriorityThreadWorkTime_ = workTime;
  }

  void highPriorityThreadFunction() {
    int64_t latencyTime = 0;
    while (shouldRun_) {
      // Sleep for a bit.
      this_thread::sleep_for(highPrioSleepTime_);

      auto startTime = chrono::high_resolution_clock::now();
      priorityMutex_->lockHighPriority();
      latencyTime += chrono::duration_cast<chrono::nanoseconds>(chrono::high_resolution_clock::now() - startTime).count();
      
      // Do work...
      this_thread::sleep_for(highPrioWorkTime_);

      priorityMutex_->unlockHighPriority();
    }
    highPriorityThreadLatencyTime_ = latencyTime;
  }
};

int main() {
  vector<std::pair<PriorityMutex*, std::string>> priorityMutexes = {
    {new BasicPriorityMutex(), "BasicPriorityMutex"},
    {new TwoMutexPriorityMutex(), "TwoMutexPriorityMutex"},
    {new MutexAndAtomicBoolPriorityMutex(), "MutexAndAtomicBoolPriorityMutex"},
    {new MutexAndTwoBoolPriorityMutex(), "MutexAndTwoBoolPriorityMutex"}
  };
  vector<chrono::microseconds> microseconds = {
    chrono::microseconds{1},
    chrono::microseconds{10},
    chrono::microseconds{100},
    chrono::microseconds{1'000},
    chrono::microseconds{10'000},
    chrono::microseconds{100'000},
    chrono::microseconds{1'000'000}
  };
  printf("  Low Work,  High Work, High Sleep\n");
  map<string, int> winnerCountForLow;
  map<string, int> winnerCountForHigh;
  for (auto lowPrioWorkTime : microseconds) {
    for (auto highPrioWorkTime : microseconds) {
      for (auto highPrioSleepTime : microseconds) {
        string bestLowName;
        string bestHighName;
        string bestBothName;
        double bestLow = 0.0;
        double bestHigh = numeric_limits<double>::max();
        printf("%10d, %10d, %10d\n", lowPrioWorkTime.count(), highPrioWorkTime.count(), highPrioSleepTime.count());
        for (auto &priorityMutexAndName : priorityMutexes) {
          ContentionTest test(priorityMutexAndName.first, lowPrioWorkTime, highPrioWorkTime, highPrioSleepTime);
          const auto [lowPriorityWorkTime, highPriorityLatencyTime] = test.run();
          printf("%31s Low Priority: %12.0f, High Priority: %12.0f\n", priorityMutexAndName.second.data(), lowPriorityWorkTime, highPriorityLatencyTime);
          if (lowPriorityWorkTime > bestLow) {
            bestLow = lowPriorityWorkTime;
            bestLowName = priorityMutexAndName.second;
          }
          if (highPriorityLatencyTime < bestHigh) {
            bestHigh = highPriorityLatencyTime;
            bestHighName = priorityMutexAndName.second;
          }
        }
        winnerCountForLow[bestLowName] += 1;
        winnerCountForHigh[bestHighName] += 1;
      }
    }
  }
  cout << "Algorithm win counts for Low Priority Thread amount of work:" << endl;
  for (const auto &i : winnerCountForLow) {
    cout << "  " << i.first << ": " << i.second << endl;
  }
  cout << "Algorithm win counts for High Priority Thread lowest latency:" << endl;
  for (const auto &i : winnerCountForHigh) {
    cout << "  " << i.first << ": " << i.second << endl;
  }
  return 0;
}
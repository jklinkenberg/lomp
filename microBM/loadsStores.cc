//===-- loadsStores.cc - Measure load/store timing -------*- C++ -*-===//
//
// Part of the LOMP project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains micro-benchmarks to measure the time taken by load and store
/// instructions under different circumstances, such as thread placement, state of
/// the cache line and degree of sharing.

#include <omp.h>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <string.h>

#include "stats-timing.h"
#ifndef USE_YIELD
#define USE_YIELD 1
#endif
#include "target.h"
#include "channel.h"

// We use OpenMP to set up and bind threads, but our measurements here are of hardware properties,
// not those of the OpenMP runtime which is being used.

#if (LOMP_TARGET_LINUX)
// Force thread affinity.
// This is a dubious thing to do; it'd be better to use the functionality
// of the OpenMP runtime, but this is simple, at least.
#include <sched.h>
void forceAffinity() {
  int me = omp_get_thread_num();
  cpu_set_t set;

  CPU_ZERO(&set);
  CPU_SET(me, &set);

  if (sched_setaffinity(0, sizeof(cpu_set_t), &set) != 0)
    fprintf(stderr, "Failed to force affinity for thread %d\n", me);
  //  else
  //    fprintf(stderr,"Thread %d bound to cpuid bit %d\n",me,me);
}
#else
void forceAffinity() {}
#endif

#define MAX_THREADS 512

class alignedUint32 {
  CACHE_ALIGNED std::atomic<uint32_t> value;

public:
  alignedUint32() : value(0) {}
  // This works OK as long as the whole variable is not removed.
  auto load() const {
    return value.load(std::memory_order_relaxed);
  }
  void store(uint32_t v = 1) {
    value.store(v, std::memory_order_relaxed);
  }
  void storeRelease(uint32_t v = 1) {
    value.store(v, std::memory_order_release);
  }
  void atomicInc() {
    value += 1;
  }
  alignedUint32 & operator=(uint32_t v) {
    value.store(v);
    return *this;
  }
  operator uint32_t() const {
    return load();
  }
};

// Both doLoads and doStores use a statically determined
// set of random accesses (generated by a python permutation), so that
// (we hope) pre-fetchers cannot work out what is happening.  In each
// case they generate 256 (measurementArraySize) accesses, this should allow data to fit in
// an L1 data cache, since with 64B cache-lines it represents 16KiB of
// data.

typedef void (*Operation)(alignedUint32 * Array);
#include <rawLoadsStores.h>

// We assume that the L1$, which is what we're mostly interested in, is smaller than 16MiB
static alignedUint32 * arrayForMeasurement;

static void checkCacheAligned(void *p) {
  if ((uintptr_t(p) & (CACHELINE_SIZE - 1)) != 0) {
    fprintf(stderr, "Array is not aligned as required\n");
    exit(1);
  }
}

// Read the largeArray which should displace all the useful data in the L1D$
void flushCacheWithLoads() {
#define largeArrayElements (64 * 1024 * 1024 / CACHELINE_SIZE)
  CACHE_ALIGNED static alignedUint32 arrayForClearing[largeArrayElements];

  for (int i = 0; i < largeArrayElements; i++)
    arrayForClearing[i].load();
}

/* Choose the default, based on the target architecture */
static bool flushWithLoads = !TARGET_HAS_CACHE_FLUSH;

void flushMeasurementArray(alignedUint32 *array) {
  if (flushWithLoads)
    flushCacheWithLoads();
  else
    for (int i = 0; i < measurementArraySize; i++)
      Target::FlushAddress(&array[i]);
}

// Default number of samples is 10,000, but for the multiple measurement cases
// this may be turned down to make them run in reasonable time.
static int NumSamples = 10000;
void measureMemory(lomp::statistic * stat, Operation op) {
  for (int i = 0; i < NumSamples; i++) {
    // Ensure the measurement array is not in our cache
    flushMeasurementArray(&arrayForMeasurement[0]);
    // Time the operation
    {
      lomp::BlockTimer bt(stat);
      op(&arrayForMeasurement[0]);
    }
  }
  stat->scaleDown(measurementArraySize);
}

// Try to measure the time to perform a series of writes to memory which is not in the cache.
// We're trying to work out how deep the write buffer is...
// It's not clear that this is going to work, because we're not discounting costs like the indirect
// function call. We *hope* that that is fixed and small enough not to matter when we're looking
// at the changes in the values.
void measureWrites(lomp::statistic * stats) {
  auto op = writeFns[0];
#if (0)
  for (int i = 0; i < NumSamples; i++) {
    {
      lomp::BlockTimer bt(&stats[0]);
      op(&arrayForMeasurement[0]);
    }
  }
  fprintf(stderr, "Tick interval: %fns\n",
          1.e9 * lomp::tsc_tick_count::getTickTime());
  fprintf(stderr, "Zero write overhead: %s\n", stats[0].format('T').c_str());
#endif

  for (int d = 1; d < 32; d++) {
    lomp::statistic * stat = &stats[d];
    //    stat->setOffset(stats[0].getMin());

    op = writeFns[d];
    for (int i = 0; i < NumSamples; i++) {
      flushMeasurementArray(&arrayForMeasurement[0]);
      {
        lomp::BlockTimer bt(stat);
        op(&arrayForMeasurement[0]);
        // Included in the operation; the last store is memory_order_release
        // std::atomic_thread_fence(std::memory_order_release);
      }
    }
    fprintf(stderr, ".");
  }
  fprintf(stderr, "\n");
}

void measurePlacementFrom(lomp::statistic * stats, Operation op, bool modified,
                          int from, bool allocateInT0 = true) {
  int nThreads = omp_get_max_threads();
  syncOnlyChannel activeToPassive;
  syncOnlyChannel passiveToActive;
  alignedUint32 * arrayToMeasure;

  if (allocateInT0)
    arrayToMeasure = &arrayForMeasurement[0];
  
#pragma omp parallel
  {
    int me = omp_get_thread_num();

    // If we're doing local allocation of the array, here's where we do it!
    if (!allocateInT0) {
#pragma omp barrier
      if (me == from) {
        arrayToMeasure = new alignedUint32[measurementArraySize];
        // The constructor zeros it so it will have been written.
        // Paranoically check that it is apropriately aligned
        checkCacheAligned(&arrayToMeasure[0]);
      }
#pragma omp barrier
    }
    for (int placement = 0; placement < nThreads; placement++) {
      if (placement == from)
        continue;

      if (me == from) {
        for (int i = 0; i < NumSamples; i++) {
          // Ensure the measurement array is not in our cache
          flushMeasurementArray(arrayToMeasure);
          // Tell the thread we're communicating with to get it into the right state there
          activeToPassive.release();
          // And wait for it to do so.
          passiveToActive.wait();
          {
            // Finally, time the operation
            lomp::BlockTimer bt(&stats[placement]);
            op(arrayToMeasure);
          }
        }
        fprintf(stderr, ".");
      }
      else if (me == placement) {
        for (int i = 0; i < NumSamples; i++) {
          activeToPassive.wait();
          // Get the cache lines into the right state here.
          if (modified)
            doStores(arrayToMeasure);
          else
            doLoads(arrayToMeasure);
          // Tell the initial thread we're ready.
          passiveToActive.release();
        }
      }
#pragma omp barrier
    }
  }
  if (!allocateInT0)
    delete[] arrayToMeasure;
  
  // The function operates on measurementArraySize lines, so we scale down the time
  // to get that for a single operation.
  for (int i = 0; i < nThreads; i++)
    stats[i].scaleDown(measurementArraySize);

  fprintf(stderr, "\n");
}

void measureSharingFrom(lomp::statistic * stats, Operation op, bool modified,
                        int from, bool) {
  int nThreads = omp_get_max_threads();

#pragma omp parallel
  {
    int me = omp_get_thread_num();
    int logicalPos = (me + nThreads - from) % nThreads;
    for (int sharing = 1; sharing < nThreads; sharing++) {
      enum {
        active,
        setup,
        setupOwner,
        nothing
      } whatIDo = logicalPos == 0
                      ? active
                      : (logicalPos < sharing
                             ? setup
                             : (logicalPos == sharing ? setupOwner : nothing));

      for (int i = 0; i < NumSamples; i++) {
#pragma omp barrier
        switch (whatIDo) {
        case active:
          // Ensure the measurement array is not in our cache
          flushMeasurementArray(&arrayForMeasurement[0]);
          break;
        default:
          break;
        }
#pragma omp barrier
        // Do setup first phase
        switch (whatIDo) {
        case setupOwner:
          // Get the cache lines into the right state here.
          if (modified)
            doStores(&arrayForMeasurement[0]);
          else
            doLoads(&arrayForMeasurement[0]);
          break;
        default:
          break;
        }
#pragma omp barrier
        // Do setup second phase
        switch (whatIDo) {
        case setup:
          doLoads(&arrayForMeasurement[0]);
          break;
        default:
          break;
        }
#pragma omp barrier
        switch (whatIDo) {
        case active: {
          // Finally, time the operation
          lomp::BlockTimer bt(&stats[sharing]);
          op(&arrayForMeasurement[0]);
        } break;
        default:
          break;
        }
      }
#pragma omp master
      fprintf(stderr, ".");
    }
  }

  // The function operates on measurementArraySize lines, so we should scale down the time
  // to get that for a single operation.
  for (int i = 0; i < nThreads; i++)
    stats[i].scaleDown(measurementArraySize);

  fprintf(stderr, "\n");
}

// Measure the half round-trip time between a specified threads and all others.
template <class ChannelClass>
void measureRoundtripFrom(lomp::statistic * stats, int source) {
  int nThreads = omp_get_max_threads();
  ChannelClass * chanp = nullptr;
  int const innerReps = 20;

#pragma omp parallel
  {
    int me = omp_get_thread_num();
    if (me == source) {
      chanp = new ChannelClass;
    }
    #pragma omp barrier
    ChannelClass * chan = chanp;

    for (int other = 0; other < nThreads; other++) {
      if (other == source)
        continue;

      if (me == source) {
        for (int i = 0; i < NumSamples; i++) {
          lomp::BlockTimer bt(&stats[other]);
          // Do innerReps ping pongs
          for (int i = 0; i < innerReps; i++)
            chan->release();
          chan->waitFor(false); /* Need to see the final consumption */
        }
        fprintf(stderr, ".");
      }
      else if (me == other) {
        for (int i = 0; i < NumSamples; i++) {
          for (int i = 0; i < innerReps; i++)
            chan->wait();
        }
      }
#pragma omp barrier
    }
  }
  delete chanp;
  
  for (int i = 0; i < nThreads; i++)
    stats[i].scaleDown(
        2 * innerReps); /* We want half ping pong, hence we multiply by 2 */

  fprintf(stderr, "\n");
}

static syncOnlyChannel * allocatePageOfChannels() {
  uintptr_t space = uintptr_t(malloc(2 * PAGE_SIZE));
  if (!space) {
    fprintf(stderr, "Cannot allocate a page\n");
    return 0;
  }
  syncOnlyChannel * page =
      (syncOnlyChannel *)((space + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));

  for (int i = 0; i < int(PAGE_SIZE / sizeof(syncOnlyChannel)); i++) {
    page[i].init();
  }

  return page;
}

/*
 * Investigate the difference in half-roundTrip time as we use each different cache-line in a page.
 * We expect that to sample evey possible tag-directory, so we hope to find some lines whose TS
 * is local to either of the two threads communicating, which should be faster.
 * (That description assumes a shared LLC, in a machine with a partioned LLC, we expect no difference
 * here).
 */
void measureLinePlacement(lomp::statistic * stats, int otherThread) {
  // We leak this memory, but it really doesn't matter, this is only a test code and will exit
  // after printing these results anyway!
  static syncOnlyChannel * page = 0;
  if (page == 0) {
    page = allocatePageOfChannels();
    if (page == 0)
      return;
  }

  int const innerReps = 10;
  int numChannels = PAGE_SIZE / sizeof(syncOnlyChannel);

#pragma omp parallel
  {
    int me = omp_get_thread_num();
    if (me == 0) {
      for (int chanIdx = numChannels - 1; chanIdx >= 0; chanIdx--) {
        syncOnlyChannel * chan = &page[chanIdx];
        lomp::statistic * stat = &stats[chanIdx];
        stat->reset();
        for (int i = 0; i < NumSamples; i++) {
          lomp::BlockTimer bt(stat);
          // Do innerReps ping pongs
          for (int i = 0; i < innerReps; i++)
            chan->release();
          chan->waitFor(false); /* Need to see the final consumption */
        }
        fprintf(stderr, ".");
      }
    }
    else if (me == otherThread) {
      for (int chanIdx = numChannels - 1; chanIdx >= 0; chanIdx--) {
        syncOnlyChannel * chan = &page[chanIdx];
        for (int i = 0; i < NumSamples; i++) {
          for (int i = 0; i < innerReps; i++)
            chan->wait();
        }
      }
    }
  }
  for (int i = 0; i < numChannels; i++)
    stats[i].scaleDown(
        2 * innerReps); /* Times two because we want half round-trip time. */
  fprintf(stderr, "\n");
}

static void delay(uint32_t ticks) {
  lomp::tsc_tick_count end =
      lomp::tsc_tick_count(lomp::tsc_tick_count::now().getValue() + ticks);

  while (lomp::tsc_tick_count::now().before(end))
    ;
}

// Find the interval from the first element to the latest
static lomp::tsc_tick_count::tsc_interval_t
longestInterval(lomp::tsc_tick_count * array, int n) {
  lomp::tsc_tick_count maxVal = array[1];

  for (int i = 2; i < n; i++)
    maxVal = maxVal.later(array[i]);

  return maxVal - array[0];
}

//
// Attempt to compute the offset between thread zero's clock and that in each
// of the other threads.
static void computeClockOffset(int64_t * offsets) {
  int nThreads = omp_get_max_threads();
  channel<int64_t> zeroToOther;
  channel<int64_t> otherToZero;
  int const NumTests = 5000;
  offsets[0] = 0;

#pragma omp parallel
  {
    int me = omp_get_thread_num();
    for (int other = 1; other < nThreads; other++) {
      if (me == 0) {
        lomp::statistic stat;
        for (int i = 0; i < NumTests; i++) {
          lomp::tsc_tick_count start;
          zeroToOther.release();
          auto Tother = otherToZero.recv();
          lomp::tsc_tick_count end;
          // Assume that the communication in each direction takes the same amount of time,

          // then the offset is like this
          //
          //     Tstart
          //              Tcomms
          //                         Tother
          //              Tcomms
          //     Tend
          //
          // So in its own time,
          // TOtherStart = Tother-(Tend-Tstart)/2
          // then the offset to add to times in the other thread
          // to map them back to times in the initial thread
          // is Tstart - TOtherStart.
          // (Consider something like this
          // Tstart = 20
          //                  TOther=30
          // TEnd   = 30
          // Then we have
          // TComms = 5
          // TOthersStart = 30-5 = 25
          // Offset = 20-25 = -5, which is correct; placing Tother at 25,
          // half way between Tstart and Tend)
          // Ignore the first iteration
          if (i == 0)
            continue;
          auto Tstart = start.getValue();
          auto Tend = end.getValue();
          double Tcomms = (Tend - Tstart) / 2.0;
          double TOtherStart = Tother - Tcomms;
          double offset = Tstart - TOtherStart;
          stat.addSample(offset);
        }
        offsets[other] = int64_t(stat.getMean());
      }
      else if (me == other) {
        for (int i = 0; i < NumTests; i++) {
          zeroToOther.wait();
          otherToZero.send(lomp::tsc_tick_count::now().getValue());
        }
      }
#pragma omp barrier
    }
  }
}

// Measure the time between the store in one thread and the last other thread seeing
// that store.
//
// We hope that cross thread clocks are synchronised, but that seems sometimes not
// to be the case.
void measureVisibilityFrom(lomp::statistic * stats,int from) {
  int nThreads = omp_get_max_threads();
  lomp::tsc_tick_count threadTimes[MAX_THREADS];
  alignedUint32 * broadcastLineP = nullptr;
  int64_t clockOffset[MAX_THREADS];
  computeClockOffset(&clockOffset[0]);

#pragma omp parallel
  {
    int me = omp_get_thread_num();
    int logicalPos = (me + nThreads - from) % nThreads;
    if (logicalPos == 0) {
      broadcastLineP = new alignedUint32;
      *broadcastLineP = 0;
    }
#pragma omp barrier
    // Ensure that we're accessing this at only one level of indirection, not two.
    auto bl = broadcastLineP;
    
    for (int sharing = 1; sharing < nThreads; sharing++) {
      enum {
        active,
        polling,
        nothing
      } whatIDo = logicalPos == 0
                      ? active
                      : (logicalPos <= sharing
                         ?  polling : nothing);
      int64_t myOffset = clockOffset[me];

      for (int i = 0; i < NumSamples; i++) {
#pragma omp barrier
        switch (whatIDo) {
        case active:
          // Wait for all threads to be ready.
          // Then wait a while so that all of the polling
          // threads have time to start polling after leaving
          // the barrier. (Adding another barrier won't help;
          // we're trying to hide the leave-time for the barrier
          // itself).
          delay(5000);
          threadTimes[0] = lomp::tsc_tick_count::now();
          bl->store(1);
          break;
        case polling:
          while (*bl == 0)
            ;
          threadTimes[logicalPos] = lomp::tsc_tick_count(
            lomp::tsc_tick_count::now().getValue() + myOffset);
          break;
        case nothing:
          break;
        }
#pragma omp barrier
        switch (whatIDo) {
        case active: {
          // Everyone has seen the write.
          // Reset the line for next time.
          *bl = 0;
          // Work out the time we should save.
          int64_t elapsed = longestInterval(threadTimes, sharing).getValue();
          if (elapsed > 0)
            stats[sharing].addSample(elapsed);
          break;
        }
        case polling:
        case nothing:
          break;
        }
      } // for NumSamples
      if (logicalPos == 0) {
        fprintf(stderr, ".");
      }
    } // for sharing
  } // parallel
  // The function operates on measurementArraySize lines, so we should scale down the time
  // to get that for a single operation.
  for (int i = 1; i < nThreads; i++) {
    stats[i].scaleDown(measurementArraySize);
  }
  delete broadcastLineP;
  
  fprintf(stderr, "\n");
}

static void printHelp() {

  printf(
      "If there are two arguments, the first determines the test.\n"
      "It may have up to three letters in it.\n"
      "The first determines the test being performed, the others the op & the "
      "state of the line\n"
      "L            -- Line latency. The half round trip time depending on the "
      "cache-line used\n"
      "M            -- Memory:  read/write latencies\n"
      "N            -- Number of writes timing; time/write if we do N "
      "consecutive writes\n"
      "R[aw] [n]    -- Round trip time: half the round trip time using atomic "
      "                or write from thread n (zero if unspecified)\n"
      "                If n<0 run all cases\n"
      "P[rwa][mu][0] [n]  -- Placement: op is read/write/atomic depending on "
      "second "
      "letter,\n"
      "                line state [modified/unmodified] is determined by the "
      "third\n"
      "                If the fourth letter is '0' then allocate the measurement\n"
      "                array in thread 0 (default is to allocate in the thread\n"
      "                doing the measurement)\n"
      "                If a second argument is present measurements are made\n"
      "                from there; if it is <0 all positions are measured.\n"
      "S[rwa][mu] [n]  -- Sharing: op is read/write/atomic depending on second "
      "letter,\n"
      "                line state [modified/unmodified] is determined by the "
      "third\n"
      "                If a second argument is present measurements are made "
      "from there; if it is <0 all positions are measured.\n"
      "V [n]           -- Visibility\n"
      "                If an argument is present measurements are made from there;\n"
      "                if it is <0; all positions are measured.\n"
      "\n"
      "In memory we're looking at the time to perform read/write to a line not "
      "in the cache\n"
      "In placement we're looking at the performance of the operation when the "
      "line is in one other cache\n"
      "which is moved over every other logicalCPU\n"
      "In sharing we're putting the line into n other caches\n"
      "In visibility we're looking at the time until the last of n polling "
      "threads sees a write\n");
}

static void printStats(lomp::statistic * stats, int count, int offset = 1) {
  // Convert to times
  double tickInterval = lomp::tsc_tick_count::getTickTime();
  for (int i = 0; i < count; i++)
    stats[i].scale(tickInterval);

  for (int i = 0; i < count; i++)
    printf("%6d, %s\n", i + offset, stats[i].format('s').c_str());
}

static std::string getDateTime() {
  auto now = std::time(0);

  return std::ctime(&now);
}

int main(int argc, char ** argv) {
  int nThreads = omp_get_max_threads();
  double tickInterval = lomp::tsc_tick_count::getTickTime();

  if (nThreads > MAX_THREADS) {
    printf("%d threads available, increase MAX_THREADS (%d)\n", nThreads,
           MAX_THREADS);
    return 1;
  }

  if (nThreads < 2) {
    printf("Need more than one thread\n");
    return 1;
  }

  if (argc < 2) {
    printf("Need an argument\n");
    printHelp();
    return 1;
  }

  // Read relevant envirables and remember the info
  if (getenv("FLUSH_WITH_LOADS"))
    flushWithLoads = true;
  std::string targetName = Target::CPUModelName();
  // printf("CPUModelName: %s\n", targetName.c_str());
  if (getenv("TARGET_MACHINE"))
    targetName = getenv("TARGET_MACHINE");

    // Warm up...
#pragma omp parallel
  { forceAffinity(); }

  // Allocate the array to measure, *after* we;ve messed with thread affinity.
  arrayForMeasurement = new alignedUint32[measurementArraySize];
  // Check that alignment is working
  checkCacheAligned(&arrayForMeasurement[0]);
  checkCacheAligned(&arrayForMeasurement[1]);
  
  
#if (0)
  int64_t clockOffset[5][MAX_THREADS];
  computeClockOffset(&clockOffset[0][0]);
  computeClockOffset(&clockOffset[1][0]);
  computeClockOffset(&clockOffset[2][0]);
  computeClockOffset(&clockOffset[3][0]);
  computeClockOffset(&clockOffset[4][0]);
  for (int i = 0; i < nThreads; i++)
    printf("%3d, %5d, %5d, %5d, %5d, %5d\n", i, clockOffset[0][i],
           clockOffset[1][i], clockOffset[2][i], clockOffset[3][i],
           clockOffset[4][i], clockOffset[5][i]);
  return 0;
#endif

  // Ensure that the pages which hold the measurement array have been allocated and touched
  // before we start any measurements.
  // (Not actually necessary since the constructor will touch each element
  // to zero it, but it doesn't hurt to be completely explicit!)
  doStores(&arrayForMeasurement[0]);

  lomp::statistic threadStats[MAX_THREADS];
  lomp::statistic lineStats[PAGE_SIZE / sizeof(syncOnlyChannel)];
  lomp::statistic * stats = &threadStats[0];
  // Most of the tests are per-thread but don't measure zero to zero.
  int numStats = nThreads - 1;
  char units = 's';
  int IdxOffset = 1;

  switch (argv[1][0]) {
  case 'L': {
    int testThread = omp_get_max_threads() - 1; /* Arbitrary choice...*/
    stats = &lineStats[0];
    numStats = PAGE_SIZE / sizeof(syncOnlyChannel);
    IdxOffset = 0;
    // Do it once to warm up, and ignore this data
    measureLinePlacement(stats, testThread);
    // Do it five times and print each so that we can see if it is consistent
    for (int i = 0; i < 5; i++) {
      measureLinePlacement(stats, testThread);
      if (i != 0)
        printf("### NEW EXPERIMENT ###\n");
      printf(
          "Line Placement (half round trip)\n"
          "%s,run %d\n"
          "# %s"
          "# Pinging core %d\n"
          "Line Index,  Samples,       Min,      Mean,       Max,        SD\n",
          targetName.c_str(), i + 1, getDateTime().c_str(), testThread);
      printStats(&stats[IdxOffset], numStats, IdxOffset);
    }
    return 0;
  }

  case 'M': {
    measureMemory(&stats[0], doLoads);
    measureMemory(&stats[1], doStores);
#pragma omp parallel
    {
      if (omp_get_thread_num() == nThreads - 1) {
        measureMemory(&stats[2], doLoads);
        measureMemory(&stats[3], doStores);
      }
    }
    double tickInterval = lomp::tsc_tick_count::getTickTime();
    for (int i = 0; i < 4; i++)
      stats[i].scale(tickInterval);

    printf("Memory Latency\n"
           "%s\n"
           "# %s\n"
           "Operation, Samples,       Min,      Mean,       Max,        SD\n",
           targetName.c_str(), getDateTime().c_str());
    printf("Load,  %s\n", stats[0].format(units).c_str());
    printf("Store, %s\n", stats[1].format(units).c_str());
    printf("Remote Load, %s\n", stats[2].format(units).c_str());
    printf("Remote Store, %s\n", stats[3].format(units).c_str());
    return 0;
  }

  case 'N': {
    fprintf(stderr, "###BEWARE the write test doesn't seem to work###\n");
    printf("Time for N writes\n"
           "%s\n"
           "# %s\n"
           "Number of writes, Samples,       Min,      Mean,       Max,        "
           "SD\n",
           targetName.c_str(), getDateTime().c_str());
    measureWrites(stats);
    numStats = 31;
    break;
  }

  case 'P':
  case 'S': {
    char const * ExperimentName;
    void (*measureFn)(lomp::statistic *, Operation, bool, int, bool);
    if (argv[1][0] == 'P') {
      ExperimentName = "Placement";
      measureFn = measurePlacementFrom;
    }
    else {
      ExperimentName = "Sharing";
      measureFn = measureSharingFrom;
    }

    Operation op;
    switch (argv[1][1]) {
    case 'r':
      op = doLoads;
      break;
    case 'w':
      op = doStores;
      break;
    case 'a':
      op = doAtomicIncs;
      break;
    default:
      printf("*** Unknown second character in %s\n", argv[1]);
      printHelp();
      return 1;
    }
    bool modified = false;
    switch (argv[1][2]) {
    case 'u':
      modified = false;
      break;
    case 'm':
      modified = true;
      break;
    default:
      printf("*** Unkown third character in %s\n", argv[1]);
      printHelp();
      return 1;
    }
    bool allocateInT0 = false;
    
    if (measureFn == measurePlacementFrom && strlen(argv[1]) >= 4) {
      if (argv[1][3] == '0') {
        allocateInT0 = true;
      }
    }
    int from = argc > 2 ? atoi(argv[2]) : 0;

    if (from < 0) {
      // Run all of them...
      NumSamples = NumSamples/4;
      for (from = 0; from < nThreads; from++) {
        measureFn(stats, op, modified, from, allocateInT0);

        // Convert to times
        for (int i = 0; i < nThreads; i++)
          stats[i].scale(tickInterval);

        if (from != 0)
          printf("### NEW EXPERIMENT ###\n");

        printf("%s\n"
               "%s, %s, %s, %s, Active %d\n"
               "# %s\n"
               "%s,  Samples,       Min,      Mean,       Max,        SD\n",
               ExperimentName, targetName.c_str(),
               op == doLoads ? "Load" : (op == doStores ? "Store" : "Atomic Inc"),
               modified ? "modified" : "unmodified",
               allocateInT0 ? "allocate(0)" : "allocate(n)",
               from, getDateTime().c_str(),
               ExperimentName);

        for (int i = 0; i < nThreads; i++) {
          if (measureFn == measurePlacementFrom && i == from)
            continue;
          if (measureFn == measureSharingFrom && i == 0)
            continue;
          printf("%d, %s\n", i, stats[i].format('s').c_str());
          stats[i].reset();
        }
      }
      return 0;
    }
    else {
      measureFn(stats, op, modified, from, allocateInT0);
      printf("%s\n"
             "%s, %s, %s, %s, Active %d\n"
             "# %s\n"
             "%s,  Samples,       Min,      Mean,       Max,        SD\n",
             ExperimentName, targetName.c_str(),
             op == doLoads ? "Load" : (op == doStores ? "Store" : "Atomic Inc"),
             modified ? "modified" : "unmodified",
             allocateInT0 ? "allocate(0)" : "allocate(n)",
             from, getDateTime().c_str(),
             ExperimentName);
      break;
    }
  }

  case 'R': { // Like placement, but we measure half the round-trip time
    char const * storeName;
    int from = (argc > 2) ? atoi(argv[2]) : 0;

    IdxOffset = 0;
    numStats = nThreads;
    void (*measureFn)(lomp::statistic *, int);
    if (argv[1][1] == 'a') {
      measureFn = measureRoundtripFrom<atomicSyncOnlyChannel>;
      storeName = "Atomic";
    }
    else {
      measureFn = measureRoundtripFrom<syncOnlyChannel>;
      storeName = "Write";
    }
    if (from < 0) {
      // Run all of them...
      NumSamples = NumSamples/4;
      for (from = 0; from < nThreads; from++) {
        measureFn(stats, from);

        // Convert to times
        for (int i = 0; i < nThreads; i++)
          stats[i].scale(tickInterval);

        if (from != 0)
          printf("### NEW EXPERIMENT ###\n");

        printf(
            "Half Round Trip\n"
            "From %d, %s, %s, %s\n"
            "# %s\n"
            "Position,  Samples,       Min,      Mean,       Max,        SD\n",
            from, targetName.c_str(), storeName,
            USE_YIELD ? "Yield" : "No Yield", getDateTime().c_str());

        for (int i = 0; i < nThreads; i++) {
          if (i == from)
            continue;
          printf("%d, %s\n", i, stats[i].format('s').c_str());
          stats[i].reset();
        }
      }
      return 0;
    }
    else {
      measureFn(stats, from);
      printf("Half Round Trip\n"
             "From %d, %s, %s, %s\n"
             "# %s\n"
             "Position,  Samples,       Min,      Mean,       Max,        SD\n",
             from, targetName.c_str(), storeName,
             USE_YIELD ? "Yield" : "No Yield", getDateTime().c_str());
    }
  } break;

  case 'V': {
    int from = 0;
    if (argc > 2)
      from = atoi(argv[2]);

    if (from < 0) {
      // Run all of them...
      NumSamples = NumSamples/4;
      for (from = 0; from < nThreads; from++) {
        measureVisibilityFrom(stats, from);

        // Convert to times
        for (int i = 1; i < nThreads; i++)
          stats[i].scale(tickInterval);

        if (from != 0)
          printf("### NEW EXPERIMENT ###\n");

        printf(
            "Visibility\n"
            "From %d, %s\n"
            "# %s\n"
            "Pollers,  Samples,       Min,      Mean,       Max,        SD\n",
            from, targetName.c_str(), getDateTime().c_str());

        for (int i = 1; i < nThreads; i++) {
          printf("%d, %s\n", i, stats[i].format('s').c_str());
          stats[i].reset();
        }
      }
      return 0;
      
    }
    else {
      IdxOffset = 1;
      measureVisibilityFrom(stats, from);
      printf("Visibility\n"
             "From %d, %s\n"
             "# %s\n"
             "Pollers,  Samples,       Min,      Mean,       Max,        SD\n",
             from, targetName.c_str(), getDateTime().c_str());
      break;
    }
  }    
  default:
    printf("Unknown experiment\n");
    printHelp();
    return 1;
  }
  printStats(&stats[IdxOffset], numStats, IdxOffset);
  return 0;
}

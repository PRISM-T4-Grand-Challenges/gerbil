/*********************************************************************************
Copyright (c) 2016 Marius Erbert, Steffen Rechner

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*********************************************************************************/

#ifndef GPUKMCHASHTABLE_CUH_
#define GPUKMCHASHTABLE_CUH_

#include "KMer.h"
#include "Bundle.h"
#include "SyncQueue.h"
#include <chrono>

#ifdef GPU
#include "KmerCountingHashTable.h"
#endif

namespace gerbil {
namespace gpu {

/**
 * A hash table for counting of Kmers based on gpu memory.
 * This is merely a wrapper to a generic hash table.
 */
template<uint32_t K>
class HasherTask {

private:

	// output queue
	SyncSwapQueueMPSC<KmcBundle>* _kmcSyncSwapQueue;

#ifdef GPU
	// thread own hash tables
	KmerCountingHashTable<K>** tables;
#endif

	uint32_t _thresholdMin;		// minimal number of occurrences to be output
	uint8_t _numThreads;		// number of gpu hasher threads

	KmerDistributer* distributor;
	std::thread** _threads;		// array of threads

	TempFile* _tempFiles;	// pointer to addional information about the files
	std::string _tempPath;		// path to temporary files

	// for statistic
	std::atomic<uint64_t> _kMersNumber;		// number of kmers processed
	std::atomic<uint64_t> _uKMersNumber;	// number of unique kmers processed
	std::atomic<uint64_t> _btUKMersNumber;// number of unique kmers below threshold

public:

	/**
	 * Initialize the Hash Table.
	 *
	 * @param numThreads The number of gpu devices to be used.
	 * @param kmcSyncSwapQueue The output queue where kmc bundles are inserted to.
	 * @param thresholdMin The minimal number of kmer counts to be considered.
	 */
	HasherTask(const uint8_t numThreads, KmerDistributer* distributor,
			SyncSwapQueueMPSC<KmcBundle>* kmcSyncSwapQueue, TempFile* tempFiles,
			std::string tempPath, const uint32_t thresholdMin) :
			_kmcSyncSwapQueue(kmcSyncSwapQueue), _thresholdMin(thresholdMin), _numThreads(
					numThreads), distributor(distributor), _tempFiles(
					tempFiles), _tempPath(tempPath), _kMersNumber(0), _uKMersNumber(
					0), _btUKMersNumber(0) {

		_threads = new std::thread*[_numThreads];
		
#ifdef GPU
		tables = new KmerCountingHashTable<K>*[_numThreads];
		// crate thread-own gpu hash tables
		for (uint32_t i = 0; i < numThreads; i++) {
			_threads[i] = nullptr;
			tables[i] = new KmerCountingHashTable<K>(i, _kmcSyncSwapQueue,
					_thresholdMin, _tempPath);
			// report capacities to kmer distributor
			distributor->updateCapacity(true, i, tables[i]->getMaxCapacity());
		}
#endif
	}

	~HasherTask() {

#ifdef GPU
		for (uint32_t i = 0; i < _numThreads; i++) {
			delete tables[i];
		}
		delete[] tables;
#endif
		delete[] _threads;
	}

	/**
	 * Reads from kmer bundle queue and inserts bundles into the hash table
	 * until queue is empty.
	 *
	 * Attention: It is vital that the size of the temp files used to fill
	 * 		the table is not larger than determined by the method
	 *		KmerCountingHashTable::maxNumKmers(). Otherwise it
	 *		could be the case that not all kmers can be inserted.
	 *
	 */
	void hash(SyncSwapQueueMPSC<gpu::KMerBundle<K>>** kMerQueues) {

		// spawn threads
		for (uint32_t devID = 0; devID < _numThreads; devID++) {
#ifdef GPU
			_threads[devID] =
			new std::thread(

					// each thread has its own kmerQueue and its own hash table
					[this](const uint32_t devID, SyncSwapQueueMPSC<gpu::KMerBundle<K>>** kMerQueues) {

						// thread-own input queue
						SyncSwapQueueMPSC<gpu::KMerBundle<K>>* kMerQueue = kMerQueues[devID];

						// thread own hash table
						KmerCountingHashTable<K>* table = tables[devID];

						gpu::KMerBundle<K>* kmb = new gpu::KMerBundle<K>;// Working KMer Bundle
						uint32_t curTempFileId = -1;// current file id
						uint64_t binKMers = 0;// number of kmers inserted by this hasher

						// for time measurement
						typedef std::chrono::microseconds ms;
						std::chrono::steady_clock::time_point start, stop;
						ms duration(0);

						// extract kmer bundles from queue until queue is empty
						while (kMerQueue->swapPop(kmb)) {

							// if the kmer bundle does not belong the the current file
							if(curTempFileId != kmb->getTempFileId()) {

								// Extract kmer counts from table
								start = std::chrono::steady_clock::now();
								table->extractAndClear();
								stop = std::chrono::steady_clock::now();

								// measure time since last extraction and  determine throughput
								duration += std::chrono::duration_cast<ms>(stop-start);
								uint64_t processedKMers = table->getKMersNumber() - binKMers;

								// report throughput to the kmer distributor
								float throughput = duration.count() != 0 ? processedKMers / duration.count() : 0;
								distributor->updateThroughput(true, devID, throughput);

								// update new file id
								curTempFileId = kmb->getTempFileId();

								// request new ratio of kmers and determine expected number of distinct kmers
								float ratio = distributor->getSplitRatio(true, devID, curTempFileId);
								uint64_t newSize = _tempFiles[curTempFileId].approximateUniqueKmers(0.9) * ratio;

								// resize the hash table
								table->init(newSize);

								// reset timer and kmer counter
								binKMers = table->getKMersNumber();
								duration = ms(0);
							}

							// insert bundle into table
							start = std::chrono::steady_clock::now();
							table->addBundle(kmb);
							stop = std::chrono::steady_clock::now();
							duration += std::chrono::duration_cast<ms>(stop-start);
						};

						// After the queue is empty:
						// Extract kmer counts from table (a last time)
						table->extractAndClear();

						_kMersNumber += table->getKMersNumber();
						_uKMersNumber += table->getUKMersNumber();
						_btUKMersNumber += table->getUKMersNumberBelowThreshold();

						// clean up
						delete kmb;

					}, devID, kMerQueues);
#else
			throw std::runtime_error(
					std::string(
							"Gerbil Error! Method not callable without GPU support!"));
#endif
		}
	}

	/**
	 * Wait for Threads to be finished
	 */
	void join() {
		for (int i = 0; i < _numThreads; i++) {
			_threads[i]->join();
			delete _threads[i];
		}
	}

	void test() {

	}

	inline uint64_t getKMersNumber() const {
		return _kMersNumber.load();
	}
	inline uint64_t getUKMersNumber() const {
		return _uKMersNumber.load();
	}
	inline uint64_t getBtUKMersNumber() const {
		return _btUKMersNumber.load();
	}
};

}
}

#endif /* GPUKMCHASHTABLE_CUH_ */

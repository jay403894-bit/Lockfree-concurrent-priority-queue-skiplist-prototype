#pragma once
#include <cstdint>
#include <atomic>
const int thread_id = 0;
class EpochManager {
public:
	static EpochManager& instance() {
		static EpochManager inst;
		return inst;
	}

	// No-op stubs — real impl needed for production
	// see T_Threads taskscheduler project for full implementation detail
	// this is jsut a more portable demo of the list itslef
	// you NEED an epoch or hazard pointer (or other sufficient)
	// method of managing memory and deleting the marked nodes
	// it is IMPORTANT to make a deep copy of the value if you 
	// expect to keep it for a long time otherwise it will get 
	// deleted after you set a 'safe time' at least in this method.

	inline void registerThread(uint32_t) {}
	inline void unregisterThread(uint32_t) {}
	inline void enterEpoch(uint32_t) {}
	inline void leaveEpoch(uint32_t) {}
	inline uint64_t currentEpoch() const { return 0; }
	template<typename T>
	void retirePtr(T* p, size_t epoch) {
	}


	inline void retireSNodeBase(void*, uint64_t) {}
	inline void retireSNMarkable(void*, uint64_t) {}
	inline void retireLNodeBase(void*, uint64_t) {}
	inline void retireLMarkable(void*, uint64_t) {}

};

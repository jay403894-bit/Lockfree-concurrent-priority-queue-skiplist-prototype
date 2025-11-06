// Lock-free concurrent skiplist with priority queue interface.
// Ported from "The Art of Multiprocessor Programming" to C++.
// Uses SNMarkablePointer with epoch-based memory reclamation.
// Supports add, remove, contains, get, and popMin operations.
#pragma once
#define NOMINMAX
#include <vector>
#include <atomic>
#include <iostream>
#include <limits>
#include <cstdint>
#include <thread>
#include <cstdlib>  // for rand()
#include <ctime>    // for seeding
#include "Epochs.h"
struct SNodeBase; // forward declaration

// MarkableReference stores a Node* and a bool mark
struct SNMarkableReference {
	SNodeBase* val_;
	bool marked_;

	SNMarkableReference(SNodeBase* val = nullptr, bool mark = false)
		: val_(val), marked_(mark) {
	}
};

// Hardcoded MarkablePointer for Node*
struct SNMarkablePointer {
	std::atomic<SNMarkableReference*> ref_;

	SNMarkablePointer() {
		ref_.store(new SNMarkableReference(nullptr, false), std::memory_order_release);
	}

	SNMarkablePointer(SNodeBase* val, bool mark) {
		ref_.store(new SNMarkableReference(val, mark), std::memory_order_release);
	}

	~SNMarkablePointer() {
		delete ref_.load(std::memory_order_acquire);
	}

	// get() like Java: returns the pointer and sets the mark
	SNodeBase* get(bool& mark) const {
		SNMarkableReference* temp = ref_.load(std::memory_order_acquire);
		mark = temp->marked_;
		return temp->val_;
	}
	bool attemptMark(SNodeBase* expectedPtr, bool newMark) {
		SNMarkableReference* curr = ref_.load(std::memory_order_acquire);

		// Only attempt if the pointer part matches expectedPtr
		if (curr->val_ != expectedPtr)
			return false;

		// If mark is already what we want, nothing to do
		if (curr->marked_ == newMark)
			return true;

		// Prepare a new reference with the same Node* but new mark
		SNMarkableReference* desired = new SNMarkableReference(curr->val_, newMark);

		// Try to swap the reference atomically
		return ref_.compare_exchange_strong(curr, desired, std::memory_order_acq_rel);
	}
	SNodeBase* getReference() const {
		return ref_.load(std::memory_order_acquire)->val_;
	}

	bool getMark() const {
		return ref_.load(std::memory_order_acquire)->marked_;
	}

	void set(SNodeBase* val, bool mark) {
		SNMarkableReference* oldRef = ref_.load(std::memory_order_acquire);
		SNMarkableReference* newRef = new SNMarkableReference(val, mark);
		ref_.store(newRef, std::memory_order_release);
		EpochManager::instance().retireSNMarkable(oldRef, EpochManager::instance().currentEpoch());
	}

	bool compareAndSet(SNodeBase* expectedPtr, SNodeBase* newPtr, bool expectedMark, bool newMark) {
		SNMarkableReference* curr = ref_.load(std::memory_order_acquire);

		// If the current pointer or mark doesn't match expected, fail early
		if (curr->val_ != expectedPtr || curr->marked_ != expectedMark)
			return false;

		// Prepare the new reference
		SNMarkableReference* desired = new SNMarkableReference(newPtr, newMark);

		// Attempt to atomically swap
		if (ref_.compare_exchange_strong(curr, desired, std::memory_order_acq_rel)) {
			// Success — retire the old reference
			EpochManager::instance().retireSNMarkable(curr, EpochManager::instance().currentEpoch());
			return true;
		}

		// Failed — don't delete curr, someone else changed it
		delete desired; // we have to delete what we allocated to avoid leaking
		return false;
	}
};

const int MAX_LEVEL = 16;

struct SNodeBase {
	SNMarkablePointer* next;
	uint64_t key;           // keep the key here for traversal/comparison
	void* data;
	int topLevel;
};
template <typename T>
struct SNode : SNodeBase {
	SNode(uint64_t key) {
		this->key = key;
		this->topLevel = MAX_LEVEL;
		next = new SNMarkablePointer[MAX_LEVEL + 1];
		for (int i = 0; i <= MAX_LEVEL; i++) {
			next[i].set(nullptr, false); // Initialize everything
		}
		this->data = nullptr;   // store in base::data
	}

	SNode(uint64_t key, int height) {
		this->key = key;
		this->topLevel = height;
		next = new SNMarkablePointer[MAX_LEVEL + 1];
		for (int i = 0; i <= MAX_LEVEL; i++) {
			next[i].set(nullptr, false); // Initialize everything
		}
		this->data = nullptr;   // store in base::data
	}

	SNode(uint64_t key, T* value, int height) {
		this->key = key;
		this->topLevel = height;
		next = new SNMarkablePointer[MAX_LEVEL + 1];
		for (int i = 0; i <= MAX_LEVEL; i++) {
			next[i].set(nullptr, false); // Initialize everything
		}
		this->data = static_cast<void*>(value);  // store pointer as integer
	}

	~SNode() {
		delete[] next;                     // free the array
		delete static_cast<T*>(this->data);  // delete the object
	}
	int height() const { return topLevel; }
};
template <typename T>
class SkipList {
	SNodeBase* head;
	SNodeBase* tail;

public:
	SkipList() {
		head = new SNode<T>(0, nullptr, MAX_LEVEL);
		tail = new SNode<T>(UINT64_MAX, nullptr, MAX_LEVEL);
		for (int i = 0; i <= MAX_LEVEL; ++i)
			head->next[i].set(tail, false);
	}

	~SkipList() {
		delete head;
		delete tail;
	}

	bool find(uint64_t key, SNode<T>* preds[MAX_LEVEL + 1], SNode<T>* succs[MAX_LEVEL + 1]){
		int bottomLevel = 0;
		bool marked = false;
		SNodeBase* pred = nullptr;
		SNodeBase* curr = nullptr;
		SNodeBase* succ = nullptr;

	RETRY:
		while (true) {
			pred = head;
			for (int level = MAX_LEVEL; level >= 0; --level) {
				pred = head;
				curr = pred->next[level].getReference();
				while (true) {
					bool marked = false;
					succ = (level <= curr->topLevel) ? curr->next[level].get(marked) : tail;

					while (marked) {
						// Try to physically remove curr
						if (!pred->next[level].compareAndSet(curr, succ, false, false))
							goto RETRY; // someone changed pred, restart whole search

						curr = pred->next[level].getReference();
						if (!curr) break;
						succ = (level <= curr->topLevel) ? curr->next[level].get(marked) : tail;
					}
				
					if (curr->key < key) {
						pred = curr;
						curr = succ; // advance curr AFTER pred is updated
					}
					else {
						break;
					}
			
				}
				preds[level] = reinterpret_cast<SNode<T>*>(pred);
				succs[level] = reinterpret_cast<SNode<T>*>(curr);
			}
			return (curr->key == key);
		}
	}
	bool add(int key, T x) {
		EpochManager::instance().enterEpoch(thread_id);
		int topLevel = randomLevel();
		const int bottomLevel = 0;
		SNode<T>* newNode = nullptr;

		// Use fixed-size arrays to avoid dynamic allocation
		SNode<T>* preds[MAX_LEVEL + 1] = {};
		SNode<T>* succs[MAX_LEVEL + 1] = {};

		while (true) {
			bool found = find(key, preds, succs);
			if (found) {
				EpochManager::instance().leaveEpoch(thread_id);
				return false; // Key already exists
			}

			// Allocate node once per attempt
			if (!newNode) {
				newNode = new SNode<T>(key, new T(x), topLevel);
			}

			// Step 1: Initialize next pointers of newNode to successors
			for (int level = bottomLevel; level <= topLevel; ++level) {
				newNode->next[level].set(succs[level], false);
			}

			// Step 2: Insert at bottom level first
			SNode<T>* pred = preds[bottomLevel];
			SNode<T>* succ = succs[bottomLevel];

			if (!pred->next[bottomLevel].compareAndSet(succ, newNode, false, false)) {
				// CAS failed, retry from scratch
				continue;
			}

			// Step 3: Insert at higher levels
			for (int level = bottomLevel + 1; level <= topLevel; ++level) {
				while (true) {
					pred = preds[level];
					succ = succs[level];

					if (pred->next[level].compareAndSet(succ, newNode, false, false))
						break; // Success

					// Retry find if CAS fails
					find(key, preds, succs);
				}
			}

			EpochManager::instance().leaveEpoch(thread_id);
			return true; // Node successfully inserted
		}
	}

	bool remove(uint64_t key) {
		EpochManager::instance().enterEpoch(thread_id);
		int bottomLevel = 0;
		SNode<T>* preds[MAX_LEVEL + 1] = {};
		SNode<T>* succs[MAX_LEVEL + 1] = {};
	RETRY_REMOVE:
		while (true) {
			bool found = find(key, preds, succs);
			if (!found) {
				EpochManager::instance().leaveEpoch(thread_id); // <- exit before return
				return false;
			}
			SNode<T>* nodeToRemove = succs[bottomLevel];

			// Step 1: Mark higher-level links
			for (int level = nodeToRemove->topLevel; level > bottomLevel; --level) {
				bool marked = false;
				SNode<T>* succ = nullptr;
				do {
					succ = reinterpret_cast<SNode<T>*>(nodeToRemove->next[level].get(marked));
					if (marked) break; // already marked
				} while (!nodeToRemove->next[level].attemptMark(succ, true));
			}

			// Step 2: Mark bottom-level link using CAS
			bool marked = false;
			SNode<T>* succ = nullptr;
			do {
				succ = reinterpret_cast<SNode<T>*>(nodeToRemove->next[bottomLevel].get(marked));
				if (marked) return false; // already removed by another thread
			} while (!nodeToRemove->next[bottomLevel].compareAndSet(succ, succ, false, true));

			// Step 3: Link predecessors to successor at bottom level
			for (int level = bottomLevel; level <= nodeToRemove->topLevel; ++level) {
				SNode<T>* pred = preds[level];
				SNode<T>* succ = reinterpret_cast<SNode<T>*>(nodeToRemove->next[level].get(marked));

				while (!pred->next[level].compareAndSet(nodeToRemove, succ, false, false)) {
					// Instead of find(key), just restart remove from top
					goto RETRY_REMOVE;
				}
			}

			EpochManager::instance().retireSNodeBase(nodeToRemove, EpochManager::instance().currentEpoch());
			// Node logically and physically removed
			EpochManager::instance().leaveEpoch(thread_id); // <- exit before return
			return true;
		}
	}

	bool contains(uint64_t key) {
		int bottomLevel = 0;
		bool marked = false;
		SNodeBase* pred = head;
		SNodeBase* curr = nullptr;
		SNodeBase* succ = nullptr;

		for (int level = MAX_LEVEL - 1; level >= bottomLevel; --level) {
			curr = pred->next[level].getReference();
			while (true) {
				if (!curr) break; // prevent nullptr dereference
				succ = curr->next[level].get(marked);
				while (marked) {
					curr = curr->next[level].getReference();
					if (!curr) break; // safety
					succ = curr->next[level].get(marked);
				}
				if (!curr) break; // prevent nullptr dereference
				if (curr->key < key) {
					pred = curr;
					curr = succ;
				}
				else {
					break;
				}
			}
		}

		return (curr->key == key);
	}
	int randomLevel(int maxIndex = MAX_LEVEL, double p = 0.5) {
		int level = 0; // use 0-based index
		while ((std::rand() / double(RAND_MAX)) < p && level < maxIndex) ++level;
		return level; // returns 0..maxIndex
	}
	T* get(uint64_t key) {
		const int bottomLevel = 0;
		SNodeBase* pred = head;
		SNodeBase* curr = nullptr;
		SNodeBase* succ = nullptr;

		for (int level = MAX_LEVEL - 1; level >= bottomLevel; --level) {
			curr = pred->next[level].getReference();
			while (curr != nullptr && curr != tail) {
				bool marked = false;
				succ = curr->next[level].get(marked);

				// Skip marked nodes by advancing curr along its next
				while (marked) {
					curr = succ;
					if (!curr || curr == tail) break;
					succ = curr->next[level].get(marked);
				}

				if (!curr || curr == tail) break;

				if (curr->key < key) {
					pred = curr;
					curr = succ;
				}
				else {
					break;
				}
			}
		}

		// Check bottom-level node
		bool nodeMarked = false;
		if (curr != nullptr)
			curr->next[bottomLevel].get(nodeMarked);

		if (curr && curr->key == key && !nodeMarked)
			return static_cast<T*>(curr->data);

		return nullptr;
	}
	SNode<T>* advancePred(SNode<T>* pred, int level) {
		bool marked;
		SNode<T>* curr = reinterpret_cast<SNode<T>*>(pred->next[level].getReference());
		while (curr && curr->next[level].get(marked) && marked) {
			pred = curr;
			curr = reinterpret_cast<SNode<T>*>(curr->next[level].getReference());
		}
		return pred;
	}
	bool empty() {
		return head->next == nullptr;
	}
	T* popMin() {
		constexpr int bottomLevel = 0;
		while (true) {
			SNode<T>* curr = reinterpret_cast<SNode<T>*>(head->next[bottomLevel].getReference());
			if (curr == tail || !curr) return nullptr;

			bool marked = false;
			SNode<T>* succ = reinterpret_cast<SNode<T>*>(curr->next[bottomLevel].get(marked));
			if (marked) {
				head->next[bottomLevel].compareAndSet(curr, succ, false, false);
				continue;
			}

			// Try to mark the node
			if (curr->next[bottomLevel].compareAndSet(succ, succ, false, true)) {
				// Marked successfully, help unlink
				head->next[bottomLevel].compareAndSet(curr, succ, false, false);
				T* val = static_cast<T*>(curr->data);
				EpochManager::instance().retireSNodeBase(curr, EpochManager::instance().currentEpoch());
				return val;
			}

			// Another thread won, retry
		}
	}
};



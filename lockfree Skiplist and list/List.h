#pragma once
#define NOMINMAX
#include <vector>
#include <atomic>
#include <iostream>
#include <intrin.h>
#include <limits>
#include <cstdint>
#include "Epochs.h"

struct LNodeBase; // forward declaration

// MarkableReference stores a Node* and a bool mark
struct LMarkableReference {
	LNodeBase* val_;
	bool marked_;

	LMarkableReference(LNodeBase* val = nullptr, bool mark = false)
		: val_(val), marked_(mark) {
	}
};

// Hardcoded MarkablePointer for Node*
struct LMarkablePointer {
	std::atomic<LMarkableReference*> ref_;

	LMarkablePointer() {
		ref_.store(new LMarkableReference(nullptr, false), std::memory_order_release);
	}

	LMarkablePointer(LNodeBase* val, bool mark) {
		ref_.store(new LMarkableReference(val, mark), std::memory_order_release);
	}

	~LMarkablePointer() {
		delete ref_.load(std::memory_order_acquire);
	}

	// get() like Java: returns the pointer and sets the mark
	LNodeBase* get(bool& mark) const {
		LMarkableReference* temp = ref_.load(std::memory_order_acquire);
		mark = temp->marked_;
		return temp->val_;
	}
	bool attemptMark(LNodeBase* expectedPtr, bool newMark) {
		LMarkableReference* curr = ref_.load(std::memory_order_acquire);

		// Only attempt if the pointer part matches expectedPtr
		if (curr->val_ != expectedPtr)
			return false;

		// If mark is already what we want, nothing to do
		if (curr->marked_ == newMark)
			return true;

		// Prepare a new reference with the same Node* but new mark
		LMarkableReference* desired = new LMarkableReference(curr->val_, newMark);

		// Try to swap the reference atomically
		return ref_.compare_exchange_strong(curr, desired, std::memory_order_acq_rel);
	}
	LNodeBase* getReference() const {
		return ref_.load(std::memory_order_acquire)->val_;
	}

	bool getMark() const {
		return ref_.load(std::memory_order_acquire)->marked_;
	}

	void set(LNodeBase* val, bool mark) {
		LMarkableReference* oldRef = ref_.load(std::memory_order_acquire);
		LMarkableReference* newRef = new LMarkableReference(val, mark);
		ref_.store(newRef, std::memory_order_release);
		EpochManager::instance().retireLMarkable(oldRef, EpochManager::instance().currentEpoch());
	}

	bool compareAndSet(LNodeBase* expectedPtr, LNodeBase* newPtr, bool expectedMark, bool newMark) {
		LMarkableReference* curr = ref_.load(std::memory_order_acquire);

		// If the current pointer or mark doesn't match expected, fail early
		if (curr->val_ != expectedPtr || curr->marked_ != expectedMark)
			return false;

		// Prepare the new reference
		LMarkableReference* desired = new LMarkableReference(newPtr, newMark);

		// Attempt to atomically swap
		if (ref_.compare_exchange_strong(curr, desired, std::memory_order_acq_rel)) {
			// Success — retire the old reference
			EpochManager::instance().retireLMarkable(curr, EpochManager::instance().currentEpoch());
			return true;
		}

		// Failed — don't delete curr, someone else changed it
		delete desired; // we have to delete what we allocated to avoid leaking
		return false;
	}
};
struct LNodeBase {
	LMarkablePointer next;   // always points to NodeBase*
	uint64_t key;           // keep the key here for traversal/comparison
};
template<typename T>
struct LNode : LNodeBase {
	T data;  // actual payload
	LNode(uint64_t k, T d) {  // accept T by value
		key = k;
		data = d;
	}
};

template <typename T>
class List {
	struct Window {
		LNodeBase* pred;
		LNodeBase* curr;
		Window(LNodeBase* myPred, LNodeBase* myCurr) {
			pred = myPred, curr = myCurr;
		}
		static Window find(LNodeBase* head, uint64_t key) {
			LNodeBase* pred = nullptr;
			LNodeBase* curr = nullptr;
			LNodeBase* succ = nullptr;
			bool marked = false;
			bool snip = false;
		RETRY:
			while (true) {
				pred = head;
				curr = pred->next.getReference();
				while (true) {
					succ = curr->next.get(marked);
					while (marked) {
						snip = pred->next.compareAndSet(curr, succ, false, false);
						if (!snip) goto RETRY;
						curr = succ;
						succ = curr->next.get(marked);
					}
					if (curr->key >= key)
						return Window(pred, curr);
					pred = curr;
					curr = succ;
				}
			}
		}
	};

	LNodeBase* head;
	LNodeBase* tail;
public:
	List() {
		head = new LNode<T>(0, T());
		tail = new LNode<T>(UINT64_MAX, T());
		head->next.set(tail, false);
	}
	~List() {
		delete head;
		delete tail;
	}
	bool add(uint64_t key, T item) {
		EpochManager::instance().enterEpoch(thread_id);
		while (true) {
			Window window = Window::find(head, key);
			LNode<T>* pred = static_cast<LNode<T>*>(window.pred);
			LNode<T>* curr = static_cast<LNode<T>*>(window.curr);

			if (curr->key == key)
				return false;

			LNode<T>* node = new LNode<T>(key, item);
			node->next.set(curr, false);

			if (pred->next.compareAndSet(curr, node, false, false)) {
				EpochManager::instance().leaveEpoch(thread_id); // leave epoch
				return true;
			}
		}
	}
	bool remove(uint64_t key) {
		EpochManager::instance().enterEpoch(thread_id); // enter epoch
		bool snip = false;
		while (true) {
			Window window = Window::find(head, key);
			LNode<T>* pred = static_cast<LNode<T>*>(window.pred);
			LNode<T>* curr = static_cast<LNode<T>*>(window.curr);
			if (curr->key != key) {
				EpochManager::instance().leaveEpoch(thread_id); // leave epoch
				return false;
			}
			else {
				LNode<T>* succ = curr->next.getReference();
				snip = curr->next.attemptMark(succ, true);
				if (!snip)
					continue;
				pred->next.compareAndSet(curr, succ, false, false);
				EpochManager::instance().retireLNodeBase(curr, EpochManager::instance().currentEpoch());
				EpochManager::instance().leaveEpoch(thread_id); // leave epoch
				return true;
			}
		}
	}

	bool contains(uint64_t key) {
		LNodeBase* curr = head;

		while (curr != nullptr) {
			LNodeBase* succ = curr->next.getReference();
			bool marked = curr->next.getMark();

			if (curr->key >= key) {
				return (curr->key == key && !marked);
			}

			curr = succ;
		}
		return false;
	}
	T* get(uint64_t key) {
		bool marked = false;
		LNodeBase* curr = head;

		while (curr->key < key) {
			curr = curr->next.get(marked);
		}

		if (curr->key == key && !marked) {
			LNode<T>* typedNode = static_cast<LNode<T>*>(curr);
			return &typedNode->data;  // return pointer to T
		}

		return nullptr;  // not found
	}
};




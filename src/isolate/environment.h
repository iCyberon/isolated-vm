#pragma once
#include <v8.h>
#include <uv.h>

#include "holder.h"
#include "../thread_pool.h"

#include <atomic>
#include <cassert>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

namespace ivm {

class Runnable;

/**
 * Wrapper around Isolate with helpers to make working with multiple isolates easier.
 */
class IsolateEnvironment {
	friend class InspectorAgent;
	friend class InspectorSession;
	friend class IsolateHolder;

	public:
		/**
		 * ExecutorLock class handles v8 locking while C++ code is running. Thread syncronization is
		 * handled by v8::Locker. This also enters the isolate and sets up a handle scope.
		 */
		class ExecutorLock {
			private:
				static thread_local IsolateEnvironment* current;
				static std::thread::id default_thread;
				IsolateEnvironment* last;
				v8::Locker locker;
				v8::Isolate::Scope isolate_scope;
				v8::HandleScope handle_scope;

			public:
				explicit ExecutorLock(IsolateEnvironment& env);
				ExecutorLock(const ExecutorLock&) = delete;
				ExecutorLock operator= (const ExecutorLock&) = delete;
				~ExecutorLock();
				static IsolateEnvironment* GetCurrent() { return current; }
				static void Init(IsolateEnvironment& default_isolate);
				static bool IsDefaultThread();
		};

		/**
		 * Keeps track of tasks an isolate needs to run and manages its run state (running or waiting).
		 * This does all the interaction with libuv async and the thread pool.
		 */
		class Scheduler {
			public:
				enum class Status { Waiting, Running };
				// A Scheduler::Lock is needed to interact with the task queue
				class Lock {
					private:
						Scheduler& scheduler;
						std::unique_lock<std::mutex> lock;
					public:
						explicit Lock(Scheduler& scheduler);
						Lock(const Lock&) = delete;
						Lock operator= (const Lock&) = delete;
						~Lock();
						void DoneRunning();
						// Add work to the task queue
						void PushTask(std::unique_ptr<Runnable> task);
						void PushInterrupt(std::unique_ptr<Runnable> interrupt);
						// Takes control of current tasks. Resets current queue
						std::queue<std::unique_ptr<Runnable>> TakeTasks();
						std::queue<std::unique_ptr<Runnable>> TakeInterrupts();
						// Returns true if a wake was scheduled, true if the isolate is already running.
						bool WakeIsolate(std::shared_ptr<IsolateEnvironment> isolate_ptr);
						// Request an interrupt in this isolate. `status` must == Running to invoke this.
						void InterruptIsolate(IsolateEnvironment& isolate);
				};

			private:
				static uv_async_t root_async;
				static thread_pool_t thread_pool;
				static std::atomic<int> uv_ref_count;
				Status status = Status::Waiting;
				std::mutex mutex;
				std::queue<std::unique_ptr<Runnable>> tasks;
				std::queue<std::unique_ptr<Runnable>> interrupts;
				thread_pool_t::affinity_t thread_affinity;

			public:
				Scheduler();
				Scheduler(const Scheduler&) = delete;
				Scheduler operator= (const Scheduler&) = delete;
				~Scheduler();
				static void Init();

			private:
				static void AsyncCallbackRoot(uv_async_t* async);
				static void AsyncCallbackPool(bool pool_thread, void* param);
				static void AsyncCallbackInterrupt(v8::Isolate* isolate_ptr, void* env_ptr);
		};

		/**
		 * Ensures we don't blow up the v8 heap while transferring arbitrary data
		 */
		class HeapCheck {
			private:
				IsolateEnvironment& env;
				bool did_increase;
			public:
				explicit HeapCheck(IsolateEnvironment& env, size_t expected_size);
				HeapCheck(const HeapCheck&) = delete;
				HeapCheck& operator= (const HeapCheck&) = delete;
				~HeapCheck();
				void Epilogue();
		};

		/**
		 * Like thread_local data, but specific to an Isolate instead.
		 */
		template <typename T>
		class IsolateSpecific {
			private:
				size_t key;
				template <typename L, typename V, V IsolateEnvironment::*S>
				v8::MaybeLocal<L> Deref() const {
					IsolateEnvironment& isolate = *ExecutorLock::GetCurrent();
					if ((isolate.*S).size() > key) {
						if (!(isolate.*S)[key]->IsEmpty()) {
							return v8::MaybeLocal<L>((isolate.*S)[key]->Get(isolate.isolate));
						}
					}
					return v8::MaybeLocal<L>();
				}

				template <typename L, typename V, V IsolateEnvironment::*S>
				void Set(v8::Local<L> handle) {
					IsolateEnvironment& isolate = *ExecutorLock::GetCurrent();
					if ((isolate.*S).size() <= key) {
						(isolate.*S).reserve(key + 1);
						while ((isolate.*S).size() <= key) {
							(isolate.*S).emplace_back(std::make_unique<v8::Eternal<L>>());
						}
					}
					(isolate.*S)[key]->Set(isolate, handle);
				}

			public:
				IsolateSpecific() : key(IsolateEnvironment::specifics_count++) {}

				v8::MaybeLocal<T> Deref() const {
					v8::Local<v8::Value> local;
					if (Deref<v8::Value, decltype(IsolateEnvironment::specifics), &IsolateEnvironment::specifics>().ToLocal(&local)) {
						return v8::MaybeLocal<T>(v8::Local<v8::Object>::Cast(local));
					} else {
						return v8::MaybeLocal<T>();
					}
				}

				void Set(v8::Local<T> handle) {
					Set<v8::Value, decltype(IsolateEnvironment::specifics), &IsolateEnvironment::specifics>(handle);
				}
		};

	private:
		struct BookkeepingStatics {
			/**
			 * These statics are needed in the destructor to update bookkeeping information. The root
			 * IsolateEnvironment will be be destroyed when the module is being destroyed, and static members
			 * may be destroyed before that happens. So we stash them here and wrap the whole in a
			 * shared_ptr so we can ensure access to them even when the module is being torn down.
			 */
			std::map<v8::Isolate*, IsolateEnvironment*> isolate_map;
			std::mutex lookup_mutex;
			bool did_shutdown = false;
		};

		static std::shared_ptr<BookkeepingStatics> bookkeeping_statics_shared;
		static size_t specifics_count;

		v8::Isolate* isolate;
		Scheduler scheduler;
		std::shared_ptr<IsolateHolder> holder;
		std::unique_ptr<class InspectorAgent> inspector_agent;
		v8::Persistent<v8::Context> default_context;
		std::unique_ptr<v8::ArrayBuffer::Allocator> allocator_ptr;
		std::shared_ptr<class ExternalCopyArrayBuffer> snapshot_blob_ptr;
		v8::StartupData startup_data {};
		size_t memory_limit = 0;
		bool hit_memory_limit = false;
		bool root;
		v8::HeapStatistics last_heap {};
		std::shared_ptr<BookkeepingStatics> bookkeeping_statics;
		v8::Persistent<v8::Value> rejected_promise_error;

		std::vector<std::unique_ptr<v8::Eternal<v8::Value>>> specifics;
		std::vector<std::unique_ptr<v8::Eternal<v8::FunctionTemplate>>> specifics_ft;
		std::map<v8::Persistent<v8::Object>*, std::pair<void(*)(void*), void*>> weak_persistents;

	public:
		std::atomic<int> terminate_depth { 0 };
		std::atomic<bool> terminated { false };

	private:
		/**
		 * Catches garbage collections on the isolate and terminates if we use too much.
		 */
		static void GCEpilogueCallback(v8::Isolate* isolate, v8::GCType type, v8::GCCallbackFlags flags);

		/**
		 * If this function is called then I have failed you.
		 */
		static void OOMErrorCallback(const char* location, bool is_heap_oom);

		/**
		 * Called when an isolate has an uncaught error in a promise. This makes no distinction between
		 * contexts so we have to handle that ourselves.
		 */
		static void PromiseRejectCallback(v8::PromiseRejectMessage rejection);

		/**
		 * Called by Scheduler when there is work to be done in this isolate.
		 */
		void AsyncEntry();
		void InterruptEntry();

		/**
		 * Returns reference to the InspectorAgent for this class. It will create the agent if it does
		 * not exist. Agent is valid until this environment is destructed.
		 */

	public:
		/**
		 * Wrap an existing Isolate. This should only be called for the main node Isolate.
		 */
		IsolateEnvironment(v8::Isolate* isolate, v8::Local<v8::Context> context);

		/**
		 * Create a new wrapped Isolate
		 */
		IsolateEnvironment(
			const v8::ResourceConstraints& resource_constraints,
			std::unique_ptr<v8::ArrayBuffer::Allocator> allocator,
			std::shared_ptr<class ExternalCopyArrayBuffer> snapshot_blob,
			size_t memory_limit
		);
		IsolateEnvironment(const IsolateEnvironment&) = delete;
		IsolateEnvironment operator= (const IsolateEnvironment&) = delete;
		~IsolateEnvironment();

		/**
		 * Factory method which generates an IsolateHolder.
		 */
		template <typename ...Args>
		static std::shared_ptr<IsolateHolder> New(Args&&... args) {
			auto isolate = std::make_shared<IsolateEnvironment>(std::forward<Args>(args)...);
			auto holder = std::make_shared<IsolateHolder>(isolate);
			isolate->holder = holder;
			return holder;
		}

		/**
		 * Return pointer the currently running IsolateEnvironment
		 */
		static IsolateEnvironment* GetCurrent() {
			return ExecutorLock::GetCurrent();
		}

		/**
		 * Return shared_ptr to current IsolateHolder
		 */
		static std::shared_ptr<IsolateHolder> GetCurrentHolder() {
			return ExecutorLock::GetCurrent()->holder;
		}

		/**
		 * Convenience operators to work with underlying isolate
		 */
		operator v8::Isolate*() const { // NOLINT
			return isolate;
		}

		v8::Isolate* operator->() const { // Should probably remove this one..
			return isolate;
		}

		v8::Isolate* GetIsolate() const {
			return isolate;
		}

		/**
		 * Default context, useful for generating certain objects when we aren't in a context.
		 */
		v8::Local<v8::Context> DefaultContext() const {
			return v8::Local<v8::Context>::New(isolate, default_context);
		}

		/**
		 * This is called after user code runs. This throws a fatal error if the memory limit was hit.
		 * If an asyncronous exception (promise) was lost, this will throw it for real.
		 */
		void TaskEpilogue();

		/**
		 * Get allocator used by this isolate. Will return nullptr for the default isolate.
		 */
		v8::ArrayBuffer::Allocator* GetAllocator() const {
			return allocator_ptr.get();
		}

		/**
		 * Get the set memory limit for this environment
		 */
		size_t GetMemoryLimit() const {
			return memory_limit;
		}

		/**
		 * Enables the inspector for this isolate.
		 */
		void EnableInspectorAgent();

		/**
		 * Returns the InspectorAgent for this Isolate.
		 */
		InspectorAgent* GetInspectorAgent() const;

		/**
		 * Check memory limit flag
		 */
		bool DidHitMemoryLimit() const {
			return hit_memory_limit;
		}

		/**
		 * Ask this isolate to finish everything it's doing.
		 */
		void Terminate() {
			assert(!root);
			terminated = true;
			isolate->TerminateExecution();
			holder->isolate.reset();
		}

		/**
		 * Since a created Isolate can be disposed of at any time we need to keep track of weak
		 * persistents to call those destructors on isolate disposal.
		 */
		void AddWeakCallback(v8::Persistent<v8::Object>* handle, void(*fn)(void*), void* param);
		void RemoveWeakCallback(v8::Persistent<v8::Object>* handle);

		/**
		 * Given a v8 isolate this will find the IsolateEnvironment instance, if any, that belongs to it.
		 */
		static std::shared_ptr<IsolateHolder> LookupIsolate(v8::Isolate* isolate);
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winstantiation-after-specialization"
// These instantiations make msvc correctly link the template specializations in environment.cc, but clang whines about it
template <>
v8::MaybeLocal<v8::FunctionTemplate> IsolateEnvironment::IsolateSpecific<v8::FunctionTemplate>::Deref() const;
template v8::MaybeLocal<v8::FunctionTemplate> IsolateEnvironment::IsolateSpecific<v8::FunctionTemplate>::Deref() const;

template <>
void IsolateEnvironment::IsolateSpecific<v8::FunctionTemplate>::Set(v8::Local<v8::FunctionTemplate> handle);
template void IsolateEnvironment::IsolateSpecific<v8::FunctionTemplate>::Set(v8::Local<v8::FunctionTemplate> handle);
#pragma clang diagnostic pop

} // namespace ivm

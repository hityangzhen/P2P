#ifndef THPOOL_H
#define THPOOL_H

#include <pthread.h>
#include <assert.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <queue>
#include <vector>
#include <map>

#define DISALLOW_COPY_CONSTRUCTORS(TypeName)	\
	TypeName(const TypeName &);					\
	TypeName & operator=(const TypeName &);

/* ================================== SYNC ================================= */

class CondVar;

class Mutex {
public:
	
	Mutex()  { assert(0==pthread_mutex_init(&mutex_,NULL)); }
	~Mutex() { assert(0==pthread_mutex_destroy(&mutex_)); }
	void Lock()   { assert(0==pthread_mutex_lock(&mutex_)); }
	void Unlock() { assert(0==pthread_mutex_unlock(&mutex_)); }
protected:
	pthread_mutex_t mutex_;
private:
	friend class CondVar;
	DISALLOW_COPY_CONSTRUCTORS(Mutex);
};

class CondVar {
public:
	CondVar()   { assert(0==pthread_cond_init(&condvar_,NULL)); }
	~CondVar()  { assert(0==pthread_cond_destroy(&condvar_)); }
	void Wait(Mutex *mu) { assert(0==pthread_cond_wait(&condvar_,&mu->mutex_)); }
	void Signal() { assert(0==pthread_cond_signal(&condvar_)); }
	void SignalAll() { assert(0==pthread_cond_broadcast(&condvar_)); }
protected:
	pthread_cond_t condvar_;
private:
	DISALLOW_COPY_CONSTRUCTORS(CondVar);
};

class BinarySemaphore {
public:
	BinarySemaphore():condition_(false) {}
	~BinarySemaphore() {}
	void Reset() { condition_=false; }
	void Post() {
		mutex_.Lock();
		condition_=true;
		condvar_.Signal();
		mutex_.Unlock();
	}
	void PostAll() {
		mutex_.Lock();
		condition_=true;
		condvar_.SignalAll();
		mutex_.Unlock();	
	}
	void Wait() {
		mutex_.Lock();
		while(!condition_)
			condvar_.Wait(&mutex_);
		condition_=false;
		mutex_.Unlock();
	}
protected:
	Mutex mutex_;
	CondVar condvar_;
	bool condition_;
};

class ScopedLock {
public:
	ScopedLock(Mutex *mutex):mutex_(mutex) {
		mutex_->Lock();
	}
	~ScopedLock() {
		mutex_->Unlock();
	}
protected:
	Mutex *mutex_;
private:
	DISALLOW_COPY_CONSTRUCTORS(ScopedLock);
};

/* ================================== JOBQUEUE ================================= */

class Job {
public:
  	typedef void *(*func_t)(void*);
  	template <typename T>
  	Job(void *(*func)(T*), T* arg)
  	: func_(reinterpret_cast<func_t>(func)), arg_(arg) {}

  	Job(void *(*func)())
  	: func_(reinterpret_cast<func_t>(func)), arg_(NULL) {}

  	void Execute() {
  		(*func_)(arg_);
  	}
private:
  	func_t func_;
  	void *arg_;
};

class JobQueue {
public:
	typedef std::queue<Job *> Queue; 
	JobQueue():condition_(false) {}
	~JobQueue();
	int Len() { return queue_.size(); }
	bool Empty() { return queue_.empty(); }
	void Push(Job *job);
	Job *Pull();
protected:
	Queue queue_;
	Mutex rwlock_;
	CondVar condvar_;
	bool condition_;
private:
	friend class ThreadPool;
	DISALLOW_COPY_CONSTRUCTORS(JobQueue);
};

/* ================================== THREADPOOL ================================= */
class ThreadPool;

class Thread {
public:
	Thread(){
		assert(0==pthread_create(&tid_,NULL,DoJob,NULL));
		pthread_detach(tid_);
	}
	~Thread() {}
	static void* DoJob(void *);
	static void SetThreadPool(ThreadPool *thp) {
		thpool=thp;
	}
	pthread_t GetTid() {
		return tid_;
	}
protected:
	pthread_t tid_;
	static ThreadPool *thpool;
private:
	DISALLOW_COPY_CONSTRUCTORS(Thread);
};

class ThreadPool {
public:
	ThreadPool():alive_threads_num_(0),working_threads_num_(0),threads_on_hold_(false),
	threads_keepalive_(true),num_(0) {
		thpool=this;
	}
	~ThreadPool();
	void Init(int num);	
	void Destroy();
	void AddWork(Job *job) {
		assert(job);
		job_queue_.Push(job);
	}
	Job *GetWork() {
		return job_queue_.Pull();
	}
	void Wait();
	void Pause() {
		for(int i=0;i<alive_threads_num_;i++)
			pthread_kill(threads_[i]->GetTid(),SIGUSR1);
	}
	void Resume() {
		threads_on_hold_=0;
	}
	void IncreaseAliveThreadsNum();
	void DecreaseAliveThreadsNum();
	void IncreaseWorkingThreadsNum();
	void DecreaseWorkingThreadsNum();
	bool HasThreadsKeepAlive() {
		return threads_keepalive_;
	}
	static void ResumeInner() {
		thpool->Resume();
	}
	
protected:
	volatile int alive_threads_num_;
	volatile int working_threads_num_;
	volatile bool threads_keepalive_;
	volatile bool threads_on_hold_;
	Thread **threads_;
	JobQueue job_queue_;	
	Mutex internal_lock_;
	int num_;
	static ThreadPool *thpool;
private:
	void NotifyAll();
	DISALLOW_COPY_CONSTRUCTORS(ThreadPool);	
};

#endif /* THPOOL_H */
#include "thpool.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>


#define MAX_NANOSEC 999999999
#define CEIL(X) ((X-(int)(X)) > 0 ? (int)(X+1) : (int)(X))

JobQueue::~JobQueue()
{
}

void JobQueue::Push(Job *job)
{
	rwlock_.Lock();
	queue_.push(job);
	condition_=true;
	condvar_.Signal();
	rwlock_.Unlock();	
}

Job *JobQueue::Pull()
{
	rwlock_.Lock();
	while(!condition_)
		condvar_.Wait(&rwlock_);
	condition_=false;
	Job *job=NULL;
	if(!queue_.empty()) {
		job=queue_.front();
		queue_.pop();
	}
	rwlock_.Unlock();
	return job;
}

ThreadPool *Thread::thpool=NULL;

void *Thread::DoJob(void *)
{
	struct sigaction act;
	//void	(*sa_handler)(int);
	act.sa_handler=(void (*)(int))thpool->ResumeInner;
	if(sigaction(SIGUSR1,&act,NULL)==-1)
		fprintf(stderr,"Thread::DoJob() can not handle SIGUSR1");

	//Mark thread as alive (initialized)
	thpool->IncreaseAliveThreadsNum();
	while(thpool->HasThreadsKeepAlive()) {
		Job *job=thpool->GetWork();
		//Double check
		if(thpool->HasThreadsKeepAlive()) {
			thpool->IncreaseWorkingThreadsNum();
			if(job) {
				job->Execute();
				delete job;
			}
			thpool->DecreaseWorkingThreadsNum();
		}
	}
	thpool->DecreaseAliveThreadsNum();
	return NULL;
}

ThreadPool *ThreadPool::thpool=NULL;

ThreadPool::~ThreadPool()
{
	if(num_) {
		for(int i=0;i<num_;i++)
			delete threads_[i];
		delete []threads_;
	}
}

void ThreadPool::Init(int num)
{
	assert(num>0);
	num_=num;
	bool flag=true;
	threads_=new Thread *[num_];
	if(!threads_) {
		fprintf(stderr,"can not allocate memory for thread pool\n");
		exit(1);
	}

	//thread init
	Thread::SetThreadPool(this);
	for(int i=0;i<num_;i++) {
		threads_[i]=new Thread;
		if(!threads_[i]) {
			fprintf(stderr, "can not allocate memory for threads\n");
			flag=false;
			break;
		}
	}
	//allocate failed
	if(!flag) {
		for(int i=0;i<num_;i++)
			delete threads_[i];
		delete []threads_;
	}
	//wait all threads prepared
	while(alive_threads_num_!=num_) {}
}

void ThreadPool::Wait()
{
	//Continuous polling
	double timeout=1.0;
	time_t start,end;
	double tpassed=0.0;
	time(&start);
	while(tpassed<timeout && (job_queue_.Len() || working_threads_num_)) {
		time(&end);
		tpassed=difftime(end,start);
	}
	//Exponential polling
	long init_nano=1;
	long new_nano;
	double multiplier=1.01;
	int max_secs=20;

	struct timespec polling_interval;
	polling_interval.tv_sec=0;
	polling_interval.tv_nsec=init_nano;

	while(job_queue_.Len() || working_threads_num_) {
		nanosleep(&polling_interval,NULL);
		if(polling_interval.tv_sec<max_secs) {
			new_nano=CEIL(polling_interval.tv_nsec * multiplier);
			polling_interval.tv_nsec=new_nano % MAX_NANOSEC;
			if(new_nano > MAX_NANOSEC)
				polling_interval.tv_sec++;
		}
		else
			break;
	}

	//Max polling
	while(job_queue_.Len() || working_threads_num_)
		sleep(max_secs);
}

void ThreadPool::NotifyAll()
{
	job_queue_.rwlock_.Lock();
	job_queue_.condition_=true;
	job_queue_.condvar_.SignalAll();
	job_queue_.rwlock_.Unlock();	
}

void ThreadPool::Destroy()
{
	threads_keepalive_=0;
	//Give one second to kill idle threads
	double timeout = 1.0;
	time_t start, end;
	double tpassed = 0.0;
	time (&start);
	while (tpassed < timeout && alive_threads_num_){
		//Notify all threads
		NotifyAll();
		time (&end);
		tpassed = difftime(end,start);
	}
	//Polling remaining threads
	while(alive_threads_num_) {
		NotifyAll();
		sleep(1);
	}
}

void ThreadPool::IncreaseAliveThreadsNum() {

	ScopedLock scopedLock(&internal_lock_);
	alive_threads_num_++;
}
void ThreadPool::DecreaseAliveThreadsNum() {
	ScopedLock scopedLock(&internal_lock_);
	alive_threads_num_--;
}
void ThreadPool::IncreaseWorkingThreadsNum() {
	ScopedLock scopedLock(&internal_lock_);
	working_threads_num_++;
}
void ThreadPool::DecreaseWorkingThreadsNum() {
	ScopedLock scopedLock(&internal_lock_);
	working_threads_num_--;
}

#ifdef THPOOL_TEST

void *print_num(int *num){
	printf("%d\n", *num);
}

int main() {
	ThreadPool thpool;
	thpool.Init(4);
	int a=10;
	thpool.AddWork(new Job(print_num,&a));
	thpool.Destroy();
	return 0;
}
#endif








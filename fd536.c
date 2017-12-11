#include <linux/syscalls.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/slab.h>

#include <linux/list.h>
#include <linux/semaphore.h>

#define UNSOLVED 0
#define SOLVED 1

struct FdReq {
	pid_t pid;
	pid_t pgid;
	struct semaphore sem_lock;
	int rslt_fd;
	int using;
	struct list_head list; /* kernel's list structure */
};

struct FdReq fdReqLst;
struct semaphore sem_lst;

static int __init myinit(void){
	INIT_LIST_HEAD(&fdReqLst.list);
	sema_init(&sem_lst,1);
	return 0;	
}
core_initcall(myinit);

void dup_by_pid(struct FdReq*, struct file*, int*);

SYSCALL_DEFINE2(sendfd, unsigned int, fd, pid_t, pid){
	struct FdReq *afdReq;
	struct FdReq *hitfdReq;
	int ret = -EBADF;
	int status = UNSOLVED;
	pid_t cgid = pid_vnr(task_pgrp(current));
	int pos_pid;

	struct file *file = fget_raw(fd);
	if(file){
		if(down_interruptible(&sem_lst)){
			return -EINTR;
		}
		list_for_each_entry(afdReq, &fdReqLst.list, list) {
			if(status == UNSOLVED && !afdReq->using){
				if(pid > 0){
					if(pid == afdReq->pid){
						status = SOLVED;
					}
				}else if(pid == 0){
					status = SOLVED;
				}
				else if(pid == -1){
					if(cgid == afdReq->pgid){
						status = SOLVED;
					}
				}else{
					pos_pid = (-1) * pid;
					if(pos_pid == afdReq->pgid){
						status = SOLVED;
					}
				}
				if(status == SOLVED){
					hitfdReq = afdReq;
				}
			}
		}
		if(status == SOLVED){
			dup_by_pid(hitfdReq,file,&ret);
		}else{
			ret = -ESRCH;
		}
		up(&sem_lst);
	}else{
		ret = -EBADF;
	}
	return ret;
}

SYSCALL_DEFINE0(rcvfd){

	// create new FdReq object
	struct FdReq *afdReq;
	int rslt_fd;
	afdReq = vmalloc(sizeof(*afdReq));
	afdReq->pid = current->pid;
	afdReq->using = 0;
	afdReq->pgid = pid_vnr(task_pgrp(current));
	sema_init(&(afdReq->sem_lock),0);
	
	// atomic start -- add itself to queue
	if(down_interruptible(&sem_lst)){
		return -EINTR;
	}
	INIT_LIST_HEAD(&afdReq->list);
	list_add_tail(&(afdReq->list), &(fdReqLst.list));
	up(&sem_lst);
	// atomic end

	// wait for awake signal
	if(down_interruptible(&(afdReq->sem_lock))){
		list_del(&(afdReq->list));
		return -EINTR;
	};

	//get fd and delete node
	if(down_interruptible(&sem_lst)){
		return -EINTR;
	}
	rslt_fd = afdReq->rslt_fd;
	list_del(&(afdReq->list));
	up(&sem_lst);

	return rslt_fd;
}

void dup_by_pid(struct FdReq *hitfdReq, struct file *file, int *ret){
	struct task_struct *task = NULL;
	struct files_struct *files = NULL;

	int new_fd;
	task = find_task_by_vpid(hitfdReq->pid);
	if(task){
		files = get_files_struct(task);
		if(files){
			new_fd = __alloc_fd(files,0,rlimit(RLIMIT_NOFILE),0);
			if(new_fd >= 0){
				__fd_install(files,new_fd,file);
				hitfdReq->rslt_fd = new_fd;
				hitfdReq->using = 1;
				up(&(hitfdReq->sem_lock));
			}else{
				fput(file);
				new_fd = -EMFILE;
			}				
		}else{
			fput(file);
			new_fd = -ENOENT;	
		}
	}else{
		fput(file);
		new_fd = -ENOENT;
	}
	*ret = new_fd;
}
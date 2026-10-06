/*
 * All libntfs-3g work runs on one kernel thread with a large stack.
 *
 * Vops are entered on the caller's kernel stack, which is small, while
 * libntfs-3g has deep call chains (attribute/index/mft code).  Funnelling every
 * call through one thread also serialises libntfs-3g, which is not thread-safe.
 */
#include <psp2kern/kernel/threadmgr.h>

#include "ntfsfs.h"

#define WORKER_STACK 0x20000
#define WORKER_PRIO  0x40

static SceUID g_thread = -1;
static SceUID g_call_lock = -1;
static SceUID g_req = -1;
static SceUID g_done = -1;

static volatile ntfsfs_job_fn g_fn;
static void *volatile g_arg;
static volatile int g_ret;
static volatile int g_quit;
static volatile SceUID g_caller = -1;

static int worker_main(SceSize args, void *argp)
{
	(void)args; (void)argp;
	for (;;) {
		if (ksceKernelWaitSema(g_req, 1, NULL) < 0)
			break;
		if (g_quit)
			break;
		g_ret = g_fn(g_arg);
		ksceKernelSignalSema(g_done, 1);
	}
	ksceKernelSignalSema(g_done, 1);
	return ksceKernelExitDeleteThread(0);
}

int ntfsfs_worker_start(void)
{
	int r;

	g_call_lock = ksceKernelCreateMutex("SceNtfsfsCall", 0, 0, NULL);
	g_req = ksceKernelCreateSema("SceNtfsfsReq", 0, 0, 1, NULL);
	g_done = ksceKernelCreateSema("SceNtfsfsDone", 0, 0, 1, NULL);
	if (g_call_lock < 0 || g_req < 0 || g_done < 0)
		return -1;
	g_thread = ksceKernelCreateThread("SceNtfsfsWorker", worker_main, WORKER_PRIO,
					  WORKER_STACK, 0, 0, NULL);
	if (g_thread < 0)
		return g_thread;
	r = ksceKernelStartThread(g_thread, 0, NULL);
	return r < 0 ? r : 0;
}

void ntfsfs_worker_stop(void)
{
	if (g_thread >= 0) {
		ksceKernelLockMutex(g_call_lock, 1, NULL);
		g_quit = 1;
		ksceKernelSignalSema(g_req, 1);
		ksceKernelWaitSema(g_done, 1, NULL);
		ksceKernelUnlockMutex(g_call_lock, 1);
		g_thread = -1;
	}
	if (g_done >= 0)
		ksceKernelDeleteSema(g_done);
	if (g_req >= 0)
		ksceKernelDeleteSema(g_req);
	if (g_call_lock >= 0)
		ksceKernelDeleteMutex(g_call_lock);
	g_done = g_req = g_call_lock = -1;
}

int ntfsfs_call(ntfsfs_job_fn fn, void *arg)
{
	int r;

	if (ksceKernelGetThreadId() == g_thread)
		return fn(arg);
	r = ksceKernelLockMutex(g_call_lock, 1, NULL);
	if (r < 0)
		return r;
	g_fn = fn;
	g_arg = arg;
	g_caller = ksceKernelGetThreadId();
	ksceKernelSignalSema(g_req, 1);
	ksceKernelWaitSema(g_done, 1, NULL);
	r = g_ret;
	g_caller = -1;
	ksceKernelUnlockMutex(g_call_lock, 1);
	return r;
}

SceUID ntfsfs_caller_thread(void)
{
	return g_caller;
}

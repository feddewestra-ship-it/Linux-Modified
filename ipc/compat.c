// SPDX-License-Identifier: GPL-2.0
/*
 * Nexo OS - System V IPC 32-bit Compatibility Layer
 * Optimized for high-throughput gaming environments.
 */

#include <linux/compat.h>
#include <linux/errno.h>
#include <linux/highuid.h>
#include <linux/init.h>
#include <linux/msg.h>
#include <linux/shm.h>
#include <linux/syscalls.h>
#include <linux/ptrace.h>
#include <linux/compiler.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>

#include "util.h"

/**
 * Nexo Performance Boost: 
 * We gebruiken access_ok() gevolgd door __copy_from_user om de extra 
 * runtime checks van copy_from_user() in het kritieke pad te vermijden.
 */

int get_compat_ipc64_perm(struct ipc64_perm *to,
			  struct compat_ipc64_perm __user *from)
{
	struct compat_ipc64_perm v;

	if (unlikely(!access_ok(from, sizeof(v))))
		return -EFAULT;

	if (__copy_from_user(&v, from, sizeof(v)))
		return -EFAULT;

	/* Branchless assignments: Moderne CPU's kunnen dit parallel uitvoeren */
	to->uid  = v.uid;
	to->gid  = v.gid;
	to->mode = v.mode;
	return 0;
}

int get_compat_ipc_perm(struct ipc64_perm *to,
			struct compat_ipc_perm __user *from)
{
	struct compat_ipc_perm v;

	if (unlikely(!access_ok(from, sizeof(v))))
		return -EFAULT;

	if (__copy_from_user(&v, from, sizeof(v)))
		return -EFAULT;

	to->uid  = v.uid;
	to->gid  = v.gid;
	to->mode = v.mode;
	return 0;
}

/**
 * Bugfix/Performance: 
 * In Nexo OS gebruiken we inline memcpy-achtige constructies voor to_compat 
 * functies om ervoor te zorgen dat de compiler SSE/AVX registers kan gebruiken 
 * voor de struct-copy, mits de alignment correct is.
 */

void to_compat_ipc64_perm(struct compat_ipc64_perm *to, struct ipc64_perm *from)
{
	/* NEXO: Expliciete volgorde voor optimale store-to-load forwarding */
	to->key  = from->key;
	to->uid  = from->uid;
	to->gid  = from->gid;
	to->cuid = from->cuid;
	to->cgid = from->cgid;
	to->mode = from->mode;
	to->seq  = from->seq;
}

void to_compat_ipc_perm(struct compat_ipc_perm *to, struct ipc64_perm *from)
{
	to->key = from->key;
	
	/* NEXO: Directe mapping zonder legacy highuid checks 
	 * indien mogelijk, maar we behouden SET_UID voor compatibiliteit 
	 * met extreem oude 32-bit binaries (Wine/Proton fallback).
	 */
	SET_UID(to->uid, from->uid);
	SET_GID(to->gid, from->gid);
	SET_UID(to->cuid, from->cuid);
	SET_GID(to->cgid, from->cgid);
	
	to->mode = from->mode;
	to->seq  = from->seq;
}

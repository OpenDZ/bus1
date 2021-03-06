/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * bus1 is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uidgid.h>
#include "main.h"
#include "peer.h"
#include "user.h"
#include "util.h"

#define BUS1_INTERNAL_UID_INVALID ((unsigned int) -1)

static DEFINE_MUTEX(bus1_user_lock);
DEFINE_IDR(bus1_user_idr);
DEFINE_IDA(bus1_user_ida);

static struct bus1_user *bus1_user_new(void)
{
	struct bus1_user *u;

	u = kmalloc(sizeof(*u), GFP_KERNEL);
	if (!u)
		return ERR_PTR(-ENOMEM);

	kref_init(&u->ref);
	u->id = BUS1_INTERNAL_UID_INVALID;
	u->uid = INVALID_UID;
	atomic_set(&u->n_messages, BUS1_MESSAGES_MAX);
	atomic_set(&u->n_handles, BUS1_HANDLES_MAX);
	atomic_set(&u->n_fds, BUS1_FDS_MAX);
	atomic_set(&u->max_messages, BUS1_MESSAGES_MAX);
	atomic_set(&u->max_handles, BUS1_HANDLES_MAX);
	atomic_set(&u->max_fds, BUS1_FDS_MAX);

	return u;
}

static void bus1_user_free(struct kref *ref)
{
	struct bus1_user *user = container_of(ref, struct bus1_user, ref);

	WARN_ON(atomic_read(&user->n_fds) !=
					atomic_read(&user->max_fds));
	WARN_ON(atomic_read(&user->n_handles) !=
					atomic_read(&user->max_handles));
	WARN_ON(atomic_read(&user->n_messages) !=
					atomic_read(&user->max_messages));

	/* if already dropped, it's set to invalid */
	if (uid_valid(user->uid)) {
		mutex_lock(&bus1_user_lock);
		if (uid_valid(user->uid)) /* check again underneath lock */
			idr_remove(&bus1_user_idr, __kuid_val(user->uid));
		mutex_unlock(&bus1_user_lock);
	}

	/* drop the id from the ida if it was initialized */
	if (user->id != BUS1_INTERNAL_UID_INVALID)
		ida_simple_remove(&bus1_user_ida, user->id);

	kfree_rcu(user, rcu);
}

/**
 * bus1_user_ref_by_uid() - get a user object for a uid
 * @uid:		uid of the user
 *
 * Find and return the user object for the uid if it exists, otherwise create
 * it first.
 *
 * Return: A user object for the given uid, ERR_PTR on failure.
 */
struct bus1_user *bus1_user_ref_by_uid(kuid_t uid)
{
	struct bus1_user *user, *old_user;
	int r;

	if (WARN_ON(!uid_valid(uid)))
		return ERR_PTR(-ENOTRECOVERABLE);

	/* try to get the user without taking a lock */
	rcu_read_lock();
	user = idr_find(&bus1_user_idr, __kuid_val(uid));
	if (user && !kref_get_unless_zero(&user->ref))
		user = NULL;
	rcu_read_unlock();
	if (user)
		return user;

	/* didn't exist, allocate a new one */
	user = bus1_user_new();
	if (IS_ERR(user))
		return ERR_CAST(user);

	/*
	 * Allocate the smallest possible internal id for this user; used in
	 * arrays for accounting user quota in receiver pools.
	 */
	r = ida_simple_get(&bus1_user_ida, 0, 0, GFP_KERNEL);
	if (r < 0)
		goto error;
	user->id = r;

	/*
	 * Now insert the new user object into the lookup tree. Note that
	 * someone might have raced us, in which case we need to switch over
	 * and drop our object. However, if the racing entry itself is already
	 * about to be destroyed again (ref-count is 0, cleanup handler is
	 * blocking on the user-lock to drop the object), we rather replace
	 * the entry in the IDR with our own and mark the old one as removed.
	 *
	 * Note that we must set user->uid *before* the idr insertion, to make
	 * sure any rcu-lookup can properly read it, even before we drop the
	 * lock.
	 */
	mutex_lock(&bus1_user_lock);
	user->uid = uid;
	old_user = idr_find(&bus1_user_idr, __kuid_val(uid));
	if (likely(!old_user)) {
		r = idr_alloc(&bus1_user_idr, user, __kuid_val(uid),
			      __kuid_val(uid) + 1, GFP_KERNEL);
		if (r < 0) {
			mutex_unlock(&bus1_user_lock);
			user->uid = INVALID_UID; /* couldn't insert */
			goto error;
		}
	} else if (unlikely(!kref_get_unless_zero(&old_user->ref))) {
		idr_replace(&bus1_user_idr, user, __kuid_val(uid));
		old_user->uid = INVALID_UID; /* mark old as removed */
		old_user = NULL;
	} else {
		user->uid = INVALID_UID; /* didn't insert, drop the marker */
	}
	mutex_unlock(&bus1_user_lock);

	if (old_user) {
		bus1_user_unref(user);
		user = old_user;
	}

	return user;

error:
	bus1_user_unref(user);
	return ERR_PTR(r);
}

/**
 * bus1_user_ref() - acquire reference
 * @user:	user to acquire, or NULL
 *
 * Acquire an additional reference to a user-object. The caller must already
 * own a reference.
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: @user is returned.
 */
struct bus1_user *bus1_user_ref(struct bus1_user *user)
{
	if (user)
		kref_get(&user->ref);
	return user;
}

/**
 * bus1_user_unref() - release reference
 * @user:	user to release, or NULL
 *
 * Release a reference to a user-object.
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: NULL is returned.
 */
struct bus1_user *bus1_user_unref(struct bus1_user *user)
{
	if (user)
		kref_put(&user->ref, bus1_user_free);
	return NULL;
}

/**
 * bus1_user_quota_init() - initialize quota object
 * @quota:		quota object to initialize
 *
 * Initialize all fields of a quota object.
 */
void bus1_user_quota_init(struct bus1_user_quota *quota)
{
	quota->n_stats = 0;
	quota->stats = NULL;
}

/**
 * bus1_user_quota_destroy() - destroy quota object
 * @quota:		quota object to destroy, or NULL
 *
 * Destroy and deallocate a quota object. All linked resources are freed, and
 * the object is ready for re-use.
 *
 * If NULL is passed, this is a no-op.
 */
void bus1_user_quota_destroy(struct bus1_user_quota *quota)
{
	if (!quota)
		return;

	kfree(quota->stats);
	bus1_user_quota_init(quota);
}

static struct bus1_user_stats *
bus1_user_quota_query(struct bus1_user_quota *quota,
		      struct bus1_user *user)
{
	struct bus1_user_stats *stats;
	size_t n;

	if (user->id >= quota->n_stats) {
		/* allocate some additional space, but prevent overflow */
		n = max(ALIGN(user->id, 8) + 8, user->id);
		stats = krealloc(quota->stats, n * sizeof(*stats), GFP_KERNEL);
		if (!stats)
			return ERR_PTR(-ENOMEM);

		memset(stats + quota->n_stats, 0,
		       (n - quota->n_stats) * sizeof(*stats));
		quota->stats = stats;
		quota->n_stats = n;
	}

	return quota->stats + user->id;
}

static int bus1_user_quota_charge_one(atomic_t *global,
				      size_t local,
				      size_t share,
				      size_t charge)
{
	/*
	 * Try charging a single resource type. If limits are exceeded, return
	 * an error-code, otherwise apply charges globally but not locally
	 * (because global charges are atomic, local charges are locked and
	 * can be applied by the caller).
	 *
	 * @global: per-user atomic that counts all instances of this resource
	 *          for this single user. It is initially set to the limit for
	 *          this user. For each accounted resource, we decrement it.
	 *          Thus, if must not drop below 0, or you exceeded your quota.
	 * @local: per-peer variable that counts all instances of this resource
	 *         for this single user. Same semantics as @global, i.e., it
	 *         counts number of remaining units, rather than accounted
	 *         units.
	 *         This function does not touch the counter, since the caller
	 *         can do that themself on success.
	 * @share: current amount of resources that the acting task has in
	 *         @local. That is, this share describes how many of the
	 *         resources in @local were added there by the current context.
	 * @charge: number of resources to charge with this operation
	 *
	 * We try charging @charge on both @local and @global. The applied
	 * logic is the same for both: The caller is not allowed to account for
	 * more than the half of the remaining space (including what its
	 * current share). That is, if 'n' free resources are remaining, then
	 * after charging @charge, it must not drop below @share+@charge. That
	 * is, the remaining resources after the charge are still at least as
	 * big as what the caller has charged in total.
	 */

	if (local < charge || local - charge < share + charge)
		return -EDQUOT;

	if (global &&
	    !bus1_atomic_sub_if_ge(global, charge, share + charge * 2))
		return -EDQUOT;

	return 0;
}

/**
 * bus1_user_quota_charge() - try charging a user
 * @peer_info:		peer with quota to operate on
 * @user:		user to charge
 * @size:		size to charge
 * @n_handles:		number of handles to charge
 * @n_fds:		number of FDs to charge
 *
 * This performs a quota charge on the passes quota object for the given user.
 * It first checks whether any quota is exceeded, and if not, it accounts for
 * the specified quantities on the quota and peer.
 *
 * This charges for _one_ message with a size of @size bytes, carrying
 * @n_handles handles and @n_fds file descriptors as payload.
 *
 * Return: 0 on success, negative error code on failure.
 */
int bus1_user_quota_charge(struct bus1_peer_info *peer_info,
			   struct bus1_user *user,
			   size_t size,
			   size_t n_handles,
			   size_t n_fds)
{
	struct bus1_user_stats *stats;
	int r;

	lockdep_assert_held(&peer_info->lock);

	stats = bus1_user_quota_query(&peer_info->quota, user);
	if (IS_ERR(stats))
		return PTR_ERR(stats);

	/*
	 * For each type of quota, we usually follow a very simple rule: A
	 * given user can acquire half of the total in-flight budget that is
	 * not used by any other user. That is, the amount available to a
	 * single user shrinks with the quota of other users rising.
	 */

	BUILD_BUG_ON(BUS1_MESSAGES_MAX > U16_MAX);
	BUILD_BUG_ON(BUS1_HANDLES_MAX > U16_MAX);
	BUILD_BUG_ON(BUS1_FDS_MAX > U16_MAX);

	r = bus1_user_quota_charge_one(NULL,
				       peer_info->n_allocated,
				       stats->n_allocated,
				       size);
	if (r < 0)
		return r;

	r = bus1_user_quota_charge_one(&user->n_messages,
				       peer_info->n_messages,
				       stats->n_messages,
				       1);
	if (r < 0)
		goto error_allocated;

	r = bus1_user_quota_charge_one(&user->n_handles,
				       peer_info->n_handles,
				       stats->n_handles,
				       n_handles);
	if (r < 0)
		goto error_messages;

	r = bus1_user_quota_charge_one(&user->n_fds,
				       peer_info->n_fds,
				       stats->n_fds,
				       n_fds);
	if (r < 0)
		goto error_handles;

	/* charge the local quoats */
	peer_info->n_allocated -= size;
	peer_info->n_messages -= 1;
	peer_info->n_handles -= n_handles;
	peer_info->n_fds -= n_fds;
	stats->n_allocated += size;
	stats->n_messages += 1;
	stats->n_handles += n_handles;
	stats->n_fds += n_fds;

	return 0;

error_handles:
	atomic_add(n_handles, &user->n_handles);
error_messages:
	atomic_inc(&user->n_messages);
error_allocated:
	/* memory has no per-user global limit; use memcgs */
	return r;
}

/**
 * bus1_user_quota_discharge() - discharge a user
 * @peer_info:		peer with quota to operate on
 * @user:		user to discharge
 * @size:		size to discharge
 * @n_handles:		number of handles to discharge
 * @n_fds:		number of FDs to discharge
 *
 * This reverts a single charge done via bus1_user_quota_charge(). It
 * discharges a single message with a slice size of @size, @n_handles handles
 * and @n_fds file-descriptors.
 */
void bus1_user_quota_discharge(struct bus1_peer_info *peer_info,
			       struct bus1_user *user,
			       size_t size,
			       size_t n_handles,
			       size_t n_fds)
{
	struct bus1_user_stats *stats;

	stats = bus1_user_quota_query(&peer_info->quota, user);
	if (WARN_ON(IS_ERR_OR_NULL(stats)))
		return;

	peer_info->n_allocated += size;
	peer_info->n_messages += 1;
	peer_info->n_handles += n_handles;
	peer_info->n_fds += n_fds;
	stats->n_allocated -= size;
	stats->n_messages -= 1;
	stats->n_handles -= n_handles;
	stats->n_fds -= n_fds;
	atomic_inc(&user->n_messages);
	atomic_add(n_handles, &user->n_handles);
	atomic_add(n_fds, &user->n_fds);
}

/**
 * bus1_user_quota_commit() - commit a quota charge
 * @peer_info:		peer with quota to operate on
 * @user:		user to commit for
 * @size:		size to commit
 * @n_handles:		number of handles to commit
 * @n_fds:		number of FDs to commit
 *
 * Commit a quota charge to the receiving user. This de-accounts the in-flight
 * charges, but keeps the actual object charges on the receiver. The caller must
 * make sure the actual objects are de-accounted once they are destructed.
 */
void bus1_user_quota_commit(struct bus1_peer_info *peer_info,
			    struct bus1_user *user,
			    size_t size,
			    size_t n_handles,
			    size_t n_fds)
{
	struct bus1_user_stats *stats;

	stats = bus1_user_quota_query(&peer_info->quota, user);
	if (WARN_ON(IS_ERR_OR_NULL(stats)))
		return;

	stats->n_allocated -= size;
	stats->n_messages -= 1;
	stats->n_handles -= n_handles;
	stats->n_fds -= n_fds;

	/* FDs are externally accounted if non-inflight; we can ignore it */
	atomic_add(n_fds, &user->n_fds);
}

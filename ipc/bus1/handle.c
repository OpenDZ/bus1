/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * bus1 is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

/*
 * Node/Handles - Implementation Details
 *
 * (See header for high-level details, this is just about the implementation)
 *
 * Details about underlying nodes are entirely hidden in this implementation.
 * Any outside caller will only ever deal with handles!
 *
 * Both owning and non-owning handles are represented as `bus1_handle`. They
 * always have `node` pointing to the underlying node as long as they exist.
 * The node object itself `bus1_handle_node` is completely dumb. It just
 * contains a list of all linked handles (which is controlled by the owner) and
 * the transaction-id to synchronize its destruction.
 *
 * Whenever a new node is allocated, the owning handle is embedded in it. This
 * guarantees that the node owner always stays allocated until the node is
 * entirely unused. However, from the caller's perspective, the owning node and
 * non-owning node are indistinguishable. Both should be considered reference
 * counted dynamic objects.
 *
 * In the implementation, the owning handle is always considered to be part of
 * a node. Whenever you have access to a node, you can also access the owning
 * handle. As such, the node and its owning handle provide the link to the
 * owning peer. Every other handle provides the link to the respective handle
 * holder.
 *
 * Both types of links, the owner link and non-owner link, are locked by their
 * respective peer lock. They can only be access or modified by locking the
 * peer. Use RCU to pin a peer in case you don't own a reference, yet.
 * Links can be removed by their owning peer. This way, any peer can remove all
 * backlinks to itself at any time. This guarantees that the peer can be shut
 * down safely, without any dangling references. However, whenever a link is
 * shut down, the remote link needs to be released afterwards as well. This is
 * async as the remote peer (the possible other side of the handle/node
 * relationship) might unlink itself in parallel.
 *
 * For each handle, @ref represents the actual object ref-count. It must be
 * used to pin the actual memory of the handle. @n_inflight describes the
 * number of real references to this handle. Once it drops to 0, the handle
 * will be released (though stay accessible until @ref drops to 0 as well).
 * @n_user is a sub-counter of @n_inflight and is used to count the references
 * that were actually reported to the user. Users can only drop references from
 * @n_user, but not directly from @n_inflight. @n_inflight is kernel-protected
 * and used during message transactions, etc.
 *
 * All handles on a node are linked into the node. This link is protected by
 * the lock of the node-owner (handle->node->owner.holder->info->lock).
 * Additionally, all handles are linked into the rb-tree of holding peer. This
 * is obviously protected by the peer lock of the respective peer.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/seqlock.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <uapi/linux/bus1.h>
#include "handle.h"
#include "peer.h"
#include "queue.h"

struct bus1_handle {
	/* static data */
	struct rb_node rb_id;			/* rb into holder, by id */
	struct rb_node rb_node;			/* rb into holder, by node */
	struct bus1_handle_node *node;		/* node */
	u64 id;					/* assigned ID */

	/* mostly static data (only touched during destruction) */
	struct bus1_peer __rcu *holder;		/* holder of this id */
	union {
		struct list_head link_node;	/* link into node */
		struct bus1_queue_node qnode;	/* link into queue */
	};

	/* non-static data */
	struct kref ref;			/* object ref-count */
	atomic_t n_inflight;			/* # of inflight users */
	atomic_t n_user;			/* # of times held by user */
};

struct bus1_handle_node {
	struct kref ref;			/* object ref-count */
	struct list_head list_handles;		/* list of handles */
	u64 timestamp;				/* destruction timestamp */
	struct bus1_handle owner;		/* handle of node owner */
};

static void bus1_handle_node_free(struct kref *ref)
{
	struct bus1_handle_node *node = container_of(ref,
						     struct bus1_handle_node,
						     ref);

	WARN_ON(rcu_access_pointer(node->owner.holder));
	WARN_ON(!list_empty(&node->list_handles));
	kfree_rcu(node, owner.qnode.rcu);
}

static void bus1_handle_node_no_free(struct kref *ref)
{
	/* no-op kref_put() callback that is used if we hold >1 reference */
	WARN(1, "Node object freed unexpectedly");
}

static bool bus1_handle_is_owner(struct bus1_handle *handle)
{
	return handle && handle == &handle->node->owner;
}

static void bus1_handle_init(struct bus1_handle *handle,
			     struct bus1_handle_node *node)
{
	RB_CLEAR_NODE(&handle->rb_id);
	RB_CLEAR_NODE(&handle->rb_node);
	handle->node = node;
	handle->id = BUS1_HANDLE_INVALID;
	rcu_assign_pointer(handle->holder, NULL);
	INIT_LIST_HEAD(&handle->link_node);
	kref_init(&handle->ref);
	atomic_set(&handle->n_inflight, -1);
	atomic_set(&handle->n_user, 0);

	kref_get(&node->ref);
}

static void bus1_handle_destroy(struct bus1_handle *handle)
{
	if (!handle)
		return;

	/*
	 * rb_id and rb_node might be stray, as we use them for delayed flush
	 * on peer destruction. We would have to explicitly lock the peer a
	 * second time during finalization to reset them. We explicitly avoid
	 * that, hence, we do *not* verify they are unlinked here.
	 */

	WARN_ON(atomic_read(&handle->n_inflight) != -1 &&
		!atomic_read(&handle->n_inflight) !=
		!atomic_read(&handle->n_user));
	WARN_ON(handle->holder);

	/*
	 * CAUTION: The handle might be embedded into the node. Make sure not
	 * to touch @handle after we dropped the reference.
	 */
	kref_put(&handle->node->ref, bus1_handle_node_free);
}

/**
 * bus1_handle_new_copy() - allocate new handle for existing node
 * @existing:		already linked handle
 *
 * This allocates a new, unlinked, detached handle for the same underlying node
 * as @existing.
 *
 * Return: Pointer to new handle, ERR_PTR on failure.
 */
struct bus1_handle *bus1_handle_new_copy(struct bus1_handle *existing)
{
	struct bus1_handle *handle;

	handle = kmalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return ERR_PTR(-ENOMEM);

	bus1_handle_init(handle, existing->node);
	return handle;
}

/**
 * bus1_handle_new() - allocate new handle for new node
 *
 * This allocates a new, unlinked, detached handle, together with a new, unused
 * node. No-one but this handle will have access to the node, until it is
 * installed.
 *
 * Return: Pointer to new handle, ERR_PTR on failure.
 */
struct bus1_handle *bus1_handle_new(void)
{
	struct bus1_handle_node *node;

	node = kmalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return ERR_PTR(-ENOMEM);

	kref_init(&node->ref);
	INIT_LIST_HEAD(&node->list_handles);
	node->timestamp = 0;
	bus1_handle_init(&node->owner, node);

	/* node->owner owns a reference to the node, drop the initial one */
	kref_put(&node->ref, bus1_handle_node_no_free);

	/* return the exclusive reference to @node->owner, and as such @node */
	return &node->owner;
}

static void bus1_handle_free(struct kref *ref)
{
	struct bus1_handle *handle = container_of(ref, struct bus1_handle, ref);
	bool is_owner;

	/*
	 * Owner-handles are embedded into the linked node. They own a
	 * reference to the node, effectively making their ref-count a subset
	 * of the node ref-count. bus1_handle_destroy() drops the
	 * ref-count to the node, as such, the handle itself might already be
	 * gone once it returns. Therefore, check whether the handle is an
	 * owner-handle before destroying it, and then skip releasing the
	 * memory if it is the owner handle.
	 */
	is_owner = bus1_handle_is_owner(handle);
	bus1_handle_destroy(handle);
	if (!is_owner)
		kfree_rcu(handle, qnode.rcu);
}

/**
 * bus1_handle_ref() - acquire reference
 * @handle:		handle to acquire reference to, or NULL
 *
 * Acquire a new reference to the passed handle. The caller must already own a
 * reference.
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: @handle is returned.
 */
struct bus1_handle *bus1_handle_ref(struct bus1_handle *handle)
{
	if (handle)
		kref_get(&handle->ref);
	return handle;
}

/**
 * bus1_handle_unref() - release reference
 * @handle:		handle to release reference of, or NULL
 *
 * Release a reference that was previously acquired via bus1_handle_ref().
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: NULL is returned.
 */
struct bus1_handle *bus1_handle_unref(struct bus1_handle *handle)
{
	if (handle)
		kref_put(&handle->ref, bus1_handle_free);
	return NULL;
}

/**
 * bus1_handle_find_by_id() - find handle by its ID
 * @peer_info:		peer to operate on
 * @id:			ID to search for
 *
 * This searches @peer_info for a handle with the given local ID. If none is
 * found, NULL is returned. Otherwise, a reference is acquired and a pointer to
 * the handle is returned.
 *
 * This only acquires a normal object reference. The caller must acquire an
 * inflight reference themself, if required.
 *
 * Return: Pointer to referenced handle, or NULL if none found.
 */
struct bus1_handle *bus1_handle_find_by_id(struct bus1_peer_info *peer_info,
					   u64 id)
{
	struct bus1_handle *handle, *res = NULL;
	struct rb_node *n;
	unsigned int seq;

	rcu_read_lock();

	/*
	 * We do a raw-reader here, as such we don't block on a racing writer.
	 * The reason for that is successful lookups are always authoritative,
	 * regardless whether they race someone. Therefore, we do the blocking
	 * reader only on the second iteration, if we failed and detected a
	 * race.
	 */
	seq = raw_seqcount_begin(&peer_info->seqcount);
	do {
		n = peer_info->map_handles_by_id.rb_node;
		while (n) {
			handle = container_of(n, struct bus1_handle, rb_id);
			if (id == handle->id) {
				if (kref_get_unless_zero(&handle->ref))
					res = handle;
				break;
			} else if (id < handle->id) {
				n = n->rb_left;
			} else /* if (id > handle->id) */ {
				n = n->rb_right;
			}
		}

		/*
		 * If @n is set, we actually found the handle with the given
		 * ID. Hence, there is no need to retry the lookup, even if we
		 * have a racing writer. Even if @res is NULL, the negative
		 * lookup is authoritative since we know that ids are
		 * sequential and never reused.
		 *
		 * However, if the lookup was negative we must check that there
		 * is no racing writer. If there is, we now do a blocking
		 * read-side acquisition and then retry the lookup.
		 */
	} while (!n &&
		 read_seqcount_retry(&peer_info->seqcount, seq) &&
		 ((seq = read_seqcount_begin(&peer_info->seqcount)), true));

	rcu_read_unlock();

	return res;
}

static struct bus1_handle *
bus1_handle_find_by_node(struct bus1_peer_info *peer_info,
			 struct bus1_handle *existing)
{
	struct bus1_handle *handle, *res = NULL;
	struct rb_node *n;
	unsigned int seq;

	/*
	 * This searches @peer_info for a handle that is linked to the same
	 * node as @existing. If none is found, NULL is returned. Otherwise, a
	 * normal object reference is acquired and returned.
	 */

	rcu_read_lock();

	/*
	 * Similar to bus1_node_handle_find_by_id(), the first iteration can
	 * safely be a raw non-blocking reader, as we expect this to succeed.
	 */
	seq = raw_seqcount_begin(&peer_info->seqcount);
	do {
		n = peer_info->map_handles_by_node.rb_node;
		while (n) {
			handle = container_of(n, struct bus1_handle, rb_node);
			if (existing->node == handle->node) {
				if (kref_get_unless_zero(&handle->ref))
					res = handle;
				break;
			} else if (existing->node < handle->node) {
				n = n->rb_left;
			} else /* if (existing->node > handle->node) */ {
				n = n->rb_right;
			}
		}

		/*
		 * If @res is set, we have a successful lookup, as such it is
		 * always authoritative, regardless of any racing writer.
		 * However, unlike ID-lookups, if the kref-acquisition failed,
		 * we have to retry as technically the backing memory might be
		 * reused for a new handle.
		 */
	} while (!res &&
		 read_seqcount_retry(&peer_info->seqcount, seq) &&
		 ((seq = read_seqcount_begin(&peer_info->seqcount)), true));

	rcu_read_unlock();

	return res;
}

/**
 * bus1_handle_is_public() - check whether a handle is public
 * @handle:		handle to check, or NULL
 *
 * A handle is considered public as soon as it was attached to its node. It
 * will never leave that state again.
 *
 * Return: True if the node is public, false if not (or if NULL is passed).
 */
bool bus1_handle_is_public(struct bus1_handle *handle)
{
	/* private handles have: n_inflight == -1 */
	return handle && atomic_read(&handle->n_inflight) >= 0;
}

static bool bus1_handle_has_id(struct bus1_handle *handle)
{
	/* true _iff_ the handle has been installed before */
	return handle && handle->id != BUS1_HANDLE_INVALID;
}

/**
 * bus1_handle_get_id() - get id of handle
 * @handle:		handle to query
 *
 * This returns the ID of the handle @handle.
 *
 * Return: ID of @handle.
 */
u64 bus1_handle_get_id(struct bus1_handle *handle)
{
	WARN_ON(!bus1_handle_has_id(handle));
	return handle->id;
}

/**
 * bus1_handle_get_owner_id() - get id of node owner
 * @handle:		handle to query
 *
 * This returns the ID of the owner of the underlying node that @handle is
 * linked to. If @handle is the actualy owner handle, the ID is equivalent to
 * the handle id.
 *
 * Return: ID of node owner @handle is linked to.
 */
u64 bus1_handle_get_owner_id(struct bus1_handle *handle)
{
	WARN_ON(!bus1_handle_has_id(&handle->node->owner));
	return handle->node->owner.id;
}

/**
 * bus1_handle_get_inorder_id() - get handle ID based on global order
 * @handle:		handle to query
 * @timestamp:		timestamp to order against
 *
 * This is similar to bus1_handle_get_id(), but only returns the ID if the node
 * was not destructed, yet. If the node was already destroyed, it will return
 * BUS1_HANDLE_INVALID.
 *
 * Since transactions are asynchronous, there is no global sequence to order
 * events. Therefore, the caller must provide their commit timestamp as
 * @timestamp, and it is used to order againts a possible node destruction. In
 * case it is lower than the node-destruction, the node is still valid
 * regarding the caller's transaction, and as such the valid handle ID will be
 * returned.
 *
 * Note that the caller *must* include the clock of the owner of the underlying
 * node in their transaction. Otherwise, the timestamps will be incomparable.
 *
 * Return: Handle ID of @handle, or BUS1_HANDLE_INVALID if already destroyed.
 */
u64 bus1_handle_get_inorder_id(struct bus1_handle *handle, u64 timestamp)
{
	struct bus1_peer_info *peer_info;
	struct bus1_peer *peer;
	unsigned int seq;
	u64 ts;

	WARN_ON(!bus1_handle_has_id(handle));

	rcu_read_lock();
	peer = rcu_dereference(handle->node->owner.holder);
	if (!peer || !(peer_info = rcu_dereference(peer->info))) {
		/*
		 * Owner handles are reset *after* the transaction id has been
		 * stored synchronously, and peer-info even after that. Hence,
		 * we can safely read the transaction ID and all barriers are
		 * provided by rcu.
		 */
		ts = handle->node->timestamp;
	} else {
		/*
		 * Try reading the transaction id. We must synchronize via the
		 * seqcount to guarantee stability across an invalidation
		 * transaction.
		 */
		do {
			seq = read_seqcount_begin(&peer_info->seqcount);
			ts = handle->node->timestamp;
		} while (read_seqcount_retry(&peer_info->seqcount, seq));
	}
	rcu_read_unlock();

	/*
	 * If the node has a commit timestamp set, and it is smaller than
	 * @timestamp, it means the node destruction was committed before
	 * @timestamp, as such the handle is invalid.
	 */
	if (ts > 0 && !(ts & 1) && ts <= timestamp)
		return BUS1_HANDLE_INVALID;

	return handle->id;
}

static struct bus1_peer *
bus1_handle_lock_holder(struct bus1_handle *handle,
			struct bus1_peer_info **infop)
{
	struct bus1_peer_info *peer_info;
	struct bus1_peer *peer;

	rcu_read_lock();
	peer = bus1_peer_acquire(rcu_dereference(handle->holder));
	rcu_read_unlock();

	if (!peer)
		return NULL;

	peer_info = bus1_peer_dereference(peer);
	mutex_lock(&peer_info->lock);
	*infop = peer_info;
	return peer;
}

static struct bus1_peer *
bus1_handle_lock_owner(struct bus1_handle *handle,
		       struct bus1_peer_info **infop)
{
	struct bus1_peer_info *peer_info;
	struct bus1_peer *peer;

	rcu_read_lock();
	peer = bus1_peer_acquire(rcu_dereference(handle->node->owner.holder));
	rcu_read_unlock();

	if (!peer)
		return NULL;

	peer_info = bus1_peer_dereference(peer);
	mutex_lock(&peer_info->lock);
	*infop = peer_info;
	return peer;
}

static struct bus1_peer *
bus1_handle_unlock_peer(struct bus1_peer *peer,
			struct bus1_peer_info *peer_info)
{
	if (peer) {
		mutex_unlock(&peer_info->lock);
		bus1_peer_release(peer);
	}
	return NULL;
}

static void bus1_handle_unlink_rb(struct bus1_handle *handle,
				  struct bus1_peer_info *peer_info)
{
	lockdep_assert_held(&peer_info->lock);

	/*
	 * @rb_node *and* @rb_id are unlinked, in case we were never installed.
	 * In that case, skip deletion entirely.
	 *
	 * @rb_node is unlinked in case we are part of an async RESET. In which
	 * case we're still linked in the rb-tree via @rb_id, but we're not
	 * supposed to touch the tree at all. Furthermore, we're supposed to
	 * leave the additional handle reference around, as the caller relies
	 * on it, just as it relies on the tree to stay around.
	 *
	 * If @rb_node is linked, then @rb_id is as well. In that case, remove
	 * both from their trees and rebalance.
	 */
	if (!RB_EMPTY_NODE(&handle->rb_node)) {
		write_seqcount_begin(&peer_info->seqcount);
		rb_erase(&handle->rb_id, &peer_info->map_handles_by_id);
		rb_erase(&handle->rb_node, &peer_info->map_handles_by_node);
		write_seqcount_end(&peer_info->seqcount);
		bus1_handle_unref(handle);
	}
}

static void
bus1_handle_commit_destruction(struct bus1_handle *handle,
			       struct bus1_peer_info *peer_info,
			       struct list_head *list_handles)
{
	struct bus1_handle *h;

	lockdep_assert_held(&peer_info->lock);
	WARN_ON(!bus1_handle_is_owner(handle));
	WARN_ON(handle->node->timestamp != 0);

	/*
	 * Set the timestamp to 1 to prevent multiple contexts destroying
	 * the handle in parallel. No need to lock seqcount since 1 is treated
	 * as invalid by all async readers.
	 */
	handle->node->timestamp = 1;

	/*
	 * Delete owner handle from list, as we don't want it to be part of the
	 * destruction. Note that it might have already been dropped. However,
	 * the reference is never dropped by the caller, so we do this here
	 * unconditionally.
	 */
	list_del_init(&handle->link_node);
	bus1_handle_unref(handle);

	h = NULL;
	h = list_prepare_entry(h, list_handles, link_node);

	while (!list_empty(&handle->node->list_handles)) {
		list_splice_tail(&handle->node->list_handles, list_handles);
		INIT_LIST_HEAD(&handle->node->list_handles);

		mutex_unlock(&peer_info->lock);
		list_for_each_entry_continue(h, list_handles, link_node) {
			/* XXX: instantiate notification *and* queue it */
		}
		/* remember last entry to continue next round */
		h = list_prev_entry(h, link_node);
		mutex_lock(&peer_info->lock);
	}

	write_seqcount_begin(&peer_info->seqcount);
	/* XXX: allocate and set transaction ID */
	handle->node->timestamp = 2;
	write_seqcount_end(&peer_info->seqcount);

	rcu_assign_pointer(handle->holder, NULL);
	bus1_handle_unlink_rb(handle, peer_info);
}

static void
bus1_handle_finalize_destruction(struct list_head *list_handles)
{
	struct bus1_peer_info *remote_info;
	struct bus1_handle *h;
	struct bus1_peer *remote;

	/* XXX: commit transaction */

	while ((h = list_first_entry_or_null(list_handles, struct bus1_handle,
					     link_node))) {
		list_del_init(&h->link_node);

		remote = bus1_handle_lock_holder(h, &remote_info);
		if (remote && rcu_access_pointer(h->holder)) {
			rcu_assign_pointer(h->holder, NULL);
			bus1_handle_unlink_rb(h, remote_info);
		}
		bus1_handle_unlock_peer(remote, remote_info);

		bus1_handle_unref(h);
	}
}

static void bus1_handle_release_owner(struct bus1_handle *handle,
				      struct bus1_peer_info *peer_info)
{
	LIST_HEAD(list_handles);
	bool destroyed = false;

	WARN_ON(!bus1_handle_is_owner(handle));
	WARN_ON(atomic_read(&handle->n_inflight) < 1);

	mutex_lock(&peer_info->lock);

	if (unlikely(!atomic_dec_and_test(&handle->n_inflight))) {
		mutex_unlock(&peer_info->lock);
		return;
	}

	WARN_ON(atomic_read(&handle->n_user) > 0);

	if (handle->node->timestamp == 0) {
		/* just unlink, don't unref; destruction unrefs the owner */
		list_del_init(&handle->link_node);
		if (list_empty(&handle->node->list_handles)) {
			destroyed = true;
			bus1_handle_commit_destruction(handle, peer_info,
						       &list_handles);
		}
	}

	mutex_unlock(&peer_info->lock);

	if (destroyed)
		bus1_handle_finalize_destruction(&list_handles);
}

static void bus1_handle_release_holder(struct bus1_handle *handle,
				       struct bus1_peer_info *peer_info)
{
	struct bus1_peer_info *remote_info;
	struct bus1_peer *remote;
	LIST_HEAD(list_handles);
	bool dropped = false, destroyed = false;

	WARN_ON(bus1_handle_is_owner(handle));
	WARN_ON(atomic_read(&handle->n_inflight) < 1);

	mutex_lock(&peer_info->lock);
	if (unlikely(!atomic_dec_and_test(&handle->n_inflight))) {
		mutex_unlock(&peer_info->lock);
		return;
	}

	WARN_ON(atomic_read(&handle->n_user) > 0);

	if (rcu_access_pointer(handle->holder)) {
		rcu_assign_pointer(handle->holder, NULL);
		bus1_handle_unlink_rb(handle, peer_info);
		dropped = true;
	}
	mutex_unlock(&peer_info->lock);

	/* bail out, if someone else was faster */
	if (!dropped)
		return;

	remote = bus1_handle_lock_owner(handle, &remote_info);
	if (remote && handle->node->timestamp == 0) {
		list_del_init(&handle->link_node);
		bus1_handle_unref(handle);
		if (list_empty(&handle->node->list_handles)) {
			destroyed = true;
			bus1_handle_commit_destruction(&handle->node->owner,
						       remote_info,
						       &list_handles);
		}
	}
	bus1_handle_unlock_peer(remote, remote_info);

	if (destroyed)
		bus1_handle_finalize_destruction(&list_handles);
}

static void bus1_handle_release_last(struct bus1_handle *handle,
				     struct bus1_peer_info *peer_info)
{
	if (bus1_handle_is_owner(handle))
		bus1_handle_release_owner(handle, peer_info);
	else
		bus1_handle_release_holder(handle, peer_info);
}

static struct bus1_handle *bus1_handle_acquire(struct bus1_handle *handle)
{
	/*
	 * This tries to acquire an inflight reference to a handle. This might
	 * fail if no inflight reference exists, anymore. In this case NULL is
	 * returned and the caller must assume the handle is dead and unlinked.
	 * In fact, it is guaranteed that the destruction is already done or in
	 * progress *with* the holder locked. It can thus be used as barrier.
	 */

	if (!handle || WARN_ON(!bus1_handle_is_public(handle)))
		return NULL;

	/*
	 * References to handles can only be acquired if someone else holds
	 * one. If n_inflight is 0, then we're guaranteed that the handle was
	 * either already unlinked or someone else currently holds the lock and
	 * unlinks it. Hence, the caller should forget about the handle and
	 * create a new one. At the time they link the new handle, the old one
	 * is guaranteed to be removed (since the last inflight ref is dropped
	 * with the peer lock held), except if..
	 *
	 * ..the handle is the node owner. In that case, an inflight reference
	 * can be acquired at any time. The node might be destroyed already,
	 * but that doesn't matter. The authoritative check is done at
	 * commit-time, anyway. The only guarantee we give is that this is the
	 * unique handle of that peer for the given node.
	 */
	if (!atomic_add_unless(&handle->n_inflight, 1, 0)) {
		if (!bus1_handle_is_owner(handle))
			return NULL;

		atomic_inc(&handle->n_inflight);
	}

	return handle;
}

/**
 * bus1_handle_release() - release an acquired handle
 * @handle:		handle to release, or NULL
 *
 * This releases a handle that was previously acquired via
 * bus1_handle_acquire() (or alike). Note that this might lock related peers,
 * in case the handle (or even the node) is destroyed.
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: NULL is returned.
 */
struct bus1_handle *bus1_handle_release(struct bus1_handle *handle)
{
	struct bus1_peer *peer;

	if (!handle || WARN_ON(!bus1_handle_is_public(handle)))
		return NULL;

	/*
	 * Release one inflight reference. If there are other references
	 * remaining, there's nothing to do for us. However, if we *might* be
	 * the last one dropping the reference, we must redirect the caller to
	 * bus1_handle_release_last(), which does a locked release.
	 *
	 * Note that if we cannot pin the holder of the handle, we know that it
	 * was already disabled. In that case, just drop the inflight counter
	 * for debug-reasons (so free() can WARN if references are remaining).
	 */

	if (atomic_add_unless(&handle->n_inflight, -1, 1))
		return NULL; /* there are other references remaining */

	/* we *may* be the last, so try again but pin and lock the holder */
	rcu_read_lock();
	peer = bus1_peer_acquire(rcu_dereference(handle->holder));
	rcu_read_unlock();
	if (peer) {
		bus1_handle_release_last(handle, bus1_peer_dereference(peer));
		bus1_peer_release(peer);
	} else {
		atomic_dec(&handle->n_inflight);
	}

	return NULL;
}

/**
 * bus1_handle_release_pinned() - release an acquired handle
 * @handle:		handle to release, or NULL
 * @peer_info:		pinned holder of @handle
 *
 * This is the same as bus1_handle_release(), but expects the caller to hold an
 * active reference to the holder of @handle, and pass in the dereferenced peer
 * info as @peer_info:
 *
 * If @handle is NULL, this is a no-op.
 *
 * Return: NULL is returned.
 */
struct bus1_handle *bus1_handle_release_pinned(struct bus1_handle *handle,
					struct bus1_peer_info *peer_info)
{
	if (!handle || WARN_ON(!bus1_handle_is_public(handle)))
		return NULL;

	if (!atomic_add_unless(&handle->n_inflight, -1, 1))
		bus1_handle_release_last(handle, peer_info);

	return NULL;
}

/**
 * bus1_handle_release_to_inflight() - turn inflight ref into user ref
 * @handle:		handle to operate on
 * @timestamp:		timestamp to order with, or 0
 *
 * This function behaves like bus1_handle_get_inorder_id(), but
 * additionally turns the caller's inflight reference on @handle into a
 * user reference. That is, the caller must assume this function calls
 * bus1_handle_release() (the object reference is not touched, though).
 *
 * If bus1_handle_get_inorder_id() tells us that the handle is already
 * destroyed, we still release the inflight reference, but skip
 * turning it into a user-reference (as it would not be needed,
 * anyway). The caller should not care, but can still detect it via the
 * returned ID.
 *
 * If you pass 0 as @timestamp, it will always order before the node
 * destruction. It is the callers responsibility to guarantee that the result
 * has valid semantics.
 *
 * Return: Handle ID of @handle, or BUS1_HANDLE_INVALID if already destroyed.
 */
u64 bus1_handle_release_to_inflight(struct bus1_handle *handle, u64 timestamp)
{
	u64 ts;

	ts = bus1_handle_get_inorder_id(handle, timestamp);
	if (ts == BUS1_HANDLE_INVALID ||
	    atomic_inc_return(&handle->n_user) != 1)
		bus1_handle_release(handle);

	return ts;
}

/**
 * bus1_handle_pin() - pin a handle
 * @handle:		handle to pin
 *
 * This tries to acquire a handle plus its owning peer. If either cannot be
 * acquired, NULL is returned.
 *
 * Return: Pointer to acquired peer, NULL on failure.
 */
struct bus1_peer *bus1_handle_pin(struct bus1_handle *handle)
{
	struct bus1_peer *peer;

	rcu_read_lock();
	peer = bus1_peer_acquire(rcu_dereference(handle->node->owner.holder));
	rcu_read_unlock();

	if (peer && !bus1_handle_acquire(handle))
		peer = bus1_peer_release(peer);

	return peer;
}

/**
 * bus1_handle_attach_unlocked() - attach a handle to its node
 * @handle:		handle to attach
 * @holder:		holder of the handle
 *
 * This attaches a non-public handle to its linked node. The caller must
 * provide the peer it wishes to be the holder of the new handle. If the
 * underlying node is already destroyed, this will fail without touching the
 * handle or the holder.
 *
 * If this function succeeds, it will automatically acquire the handle as well.
 * See bus1_handle_acquire() for details.
 *
 * The caller must have pinned *and* locked the owning peer of @handle (this
 * *might* match @holder, if this attaches the owner, but usually does not).
 *
 * Return: True if the handle was attached, false if the node is already gone.
 */
bool bus1_handle_attach_unlocked(struct bus1_handle *handle,
				 struct bus1_peer *holder)
{
	struct bus1_peer_info *owner_info;
	struct bus1_peer *owner;

	if (WARN_ON(handle->holder || bus1_handle_is_public(handle)))
		return true;

	/*
	 * During node destruction, the owner is reset to NULL once the
	 * destruction sequence has been committed. At that point, any
	 * following attach operation must fail and be treated as if the node
	 * never existed.
	 *
	 * BUT if we are the owner, the node is fully disjoint and nobody but
	 * us has access to it. Hence, an attach operation will always succeed.
	 */
	owner = rcu_access_pointer(handle->node->owner.holder);
	if (!owner) {
		if (!bus1_handle_is_owner(handle))
			return false;
		owner = holder;
	}

	owner_info = bus1_peer_dereference(owner);
	lockdep_assert_held(&owner_info->lock);

	atomic_set(&handle->n_inflight, 1);
	rcu_assign_pointer(handle->holder, holder);
	list_add_tail(&handle->link_node, &handle->node->list_handles);
	kref_get(&handle->ref); /* node owns a reference until unlinked */

	return true;
}

static bool bus1_handle_attach(struct bus1_handle *handle,
			       struct bus1_peer *holder)
{
	struct bus1_peer_info *owner_info;
	struct bus1_peer *owner;
	bool res;

	/*
	 * This is a wrapper around bus1_handle_attach(), which acquires and
	 * locks the owner of @handle (or @holder in case this attaches the
	 * owner). This is the slow-path and only needed if 3rd party handles
	 * are transmitted.
	 */

	if (bus1_handle_is_owner(handle)) {
		owner_info = bus1_peer_dereference(holder);
		owner = NULL;
	} else {
		rcu_read_lock();
		owner = rcu_dereference(handle->node->owner.holder);
		owner = bus1_peer_acquire(owner);
		rcu_read_unlock();

		if (!owner)
			return false;

		owner_info = bus1_peer_dereference(owner);
	}

	mutex_lock(&owner_info->lock);
	res = bus1_handle_attach_unlocked(handle, holder);
	mutex_unlock(&owner_info->lock);

	bus1_peer_release(owner);
	return res;
}

/**
 * bus1_handle_install_unlocked() - install a handle
 * @handle:		handle to install
 *
 * This installs the passed handle in its holding peer. The caller must hold
 * the peer lock of @handle->holder.
 *
 * While the attach operation links a handle to its underlying node, the
 * install operation links it into the holding peer. That is, an ID is
 * allocated and the handle is linked into the lookup trees. The caller must
 * attach a handle before it can install it.
 *
 * In case the underlying node was already destroyed, this will return NULL.
 *
 * In case another handle on the same peer raced this install, the pointer to
 * the other *acquired* and *referenced* handle is returned. The original
 * handle is left untouched. The caller should release and drop its original
 * handle and use the replacement instead.
 *
 * If the passed handle was installed successfully, a pointer to it is
 * returned.
 *
 * Return: NULL if the node is already destroyed, @handle on success, pointer
 *         to conflicting handle otherwise.
 */
struct bus1_handle *bus1_handle_install_unlocked(struct bus1_handle *handle)
{
	struct bus1_peer_info *peer_info;
	struct bus1_handle *iter, *old = NULL;
	struct rb_node *n, **slot;

	if (WARN_ON(!bus1_handle_is_public(handle)))
		return NULL;
	if (WARN_ON(handle->id != BUS1_HANDLE_INVALID))
		return handle;

	/*
	 * If the holder is NULL, then the node was shut down in between attach
	 * and install.
	 * Return NULL to signal the caller that the node is gone. There is
	 * also no need to detach the handle. This was already done via node
	 * destruction in this case.
	 */
	if (unlikely(!rcu_access_pointer(handle->holder)))
		return NULL;

	peer_info = bus1_peer_dereference(rcu_access_pointer(handle->holder));
	lockdep_assert_held(&peer_info->lock);

	/*
	 * The holder of the handle is locked. Lock the seqcount and try
	 * inserting the new handle into the lookup trees of the peer. Note
	 * that someone might have raced us, in case the linked node is *not*
	 * exclusively owned by this handle. Hence, first try to find a
	 * conflicting handle entry. If none is found, allocate a new handle ID
	 * and insert the handle into both lookup trees. However, if a conflict
	 * is found, take a reference to it and skip the insertion. Return the
	 * conflict to the caller and let them deal with it (meaning: they
	 * unlock the peer, then destroy their temporary handle and switch over
	 * to the conflict).
	 */
	write_seqcount_begin(&peer_info->seqcount);
	n = NULL;
	slot = &peer_info->map_handles_by_node.rb_node;
	while (*slot) {
		n = *slot;
		iter = container_of(n, struct bus1_handle, rb_node);
		if (unlikely(handle->node == iter->node)) {
			/*
			 * Someone raced us installing a handle for the
			 * well-known node faster than we did. Drop our node
			 * and switch over to the other one.
			 */
			WARN_ON(iter->holder != handle->holder);
			WARN_ON(iter->id == BUS1_HANDLE_INVALID);

			old = handle;
			handle = bus1_handle_ref(iter);
			WARN_ON(!bus1_handle_acquire(handle));
			break;
		} else if (handle->node < iter->node) {
			slot = &n->rb_left;
		} else /* if (handle->node > iter->node) */ {
			slot = &n->rb_right;
		}
	}
	if (likely(!old)) {
		kref_get(&handle->ref); /* peer owns a ref until unlinked */
		handle->id = (++peer_info->handle_ids << 2) |
						     BUS1_NODE_FLAG_MANAGED;

		/* insert into node-map */
		rb_link_node_rcu(&handle->rb_node, n, slot);
		rb_insert_color(&handle->rb_node,
				&peer_info->map_handles_by_node);

		/* insert into id-map */
		n = rb_last(&peer_info->map_handles_by_id);
		if (n)
			rb_link_node_rcu(&handle->rb_id, n, &n->rb_right);
		else
			rb_link_node_rcu(&handle->rb_id, NULL,
					 &peer_info->map_handles_by_id.rb_node);
		rb_insert_color(&handle->rb_id, &peer_info->map_handles_by_id);
	}
	write_seqcount_end(&peer_info->seqcount);

	return handle;
}

/**
 * bus1_handle_release_by_id() - release a user handle
 * @peer_info:		peer to operate on
 * @id:			handle ID
 *
 * This releases a *user* visible reference to the handle with the given ID.
 *
 * Return: 0 on success, negative error code on failure.
 */
int bus1_handle_release_by_id(struct bus1_peer_info *peer_info, u64 id)
{
	struct bus1_handle *handle;
	int r, n_user;

	handle = bus1_handle_find_by_id(peer_info, id);
	if (!handle)
		return -ENXIO;

	/* returns "old_value - 1", regardless whether it succeeded or not */
	n_user = atomic_dec_if_positive(&handle->n_user);
	if (n_user < 0) {
		/* DEC did *NOT* happen, peer does not own a reference */
		r = -ESTALE;
	} else if (n_user > 0) {
		/* DEC happened, but it wasn't the last; bail out */
		r = 0;
	} else {
		/* DEC happened and dropped to 0, release the linked ref */
		bus1_handle_release_pinned(handle, peer_info);
		r = 0;
	}

	bus1_handle_unref(handle);
	return r;
}

/**
 * bus1_handle_destroy_by_id() - destroy a user handle
 * @peer_info:		peer to operate on
 * @id:			handle ID
 *
 * This destroys the underlying node of the handle with the given ID.
 *
 * Return: 0 on success, negative error code on failure.
 */
int bus1_handle_destroy_by_id(struct bus1_peer_info *peer_info, u64 id)
{
	struct bus1_handle *handle;
	LIST_HEAD(list_handles);
	int r;

	handle = bus1_handle_find_by_id(peer_info, id);
	if (!handle)
		return -ENXIO;

	mutex_lock(&peer_info->lock);
	if (!bus1_handle_is_owner(handle)) {
		r = -EPERM;
	} else if (handle->node->timestamp != 0) {
		r = -EINPROGRESS;
	} else {
		bus1_handle_commit_destruction(handle, peer_info,
					       &list_handles);
		r = 0;
	}
	mutex_unlock(&peer_info->lock);

	if (r < 0)
		goto exit;

	bus1_handle_finalize_destruction(&list_handles);
	r = 0;

exit:
	bus1_handle_unref(handle);
	return r;
}

/**
 * bus1_handle_flush_all() - flush all owned handles
 * @peer_info:		peer to operate on
 * @map:		rb-tree to push handles into
 *
 * This removes all owned handles from the given peer and stores them for later
 * removal into @map. See bus1_handle_finish_all() for the tail call.
 *
 * The caller must hold the peer lock of @peer_info.
 */
void bus1_handle_flush_all(struct bus1_peer_info *peer_info,
			   struct rb_root *map)
{
	struct bus1_handle *handle, *t;

	/*
	 * Get a copy of the id-map root and reset it to NULL. This is
	 * basically equivalent to calling rb_erase() on all our handles.
	 * However, we now have the benefit that the tree is still intact and
	 * we can traverse it safely. We just must make sure not to screw with
	 * the rb_id/rb_node pointers, as concurrent lookups might race us. The
	 * rb-removal helpers check for RB_EMPTY_NODE(&h->rb_node), if true
	 * they assume the entry is removed by the caller (which in our case is
	 * us in bus1_handle_finish_all()). Note that RB_CLEAR_NODE only
	 * touches the parent pointer, so racing lookups will not be affected.
	 *
	 * Unlike normal handle destruction/release, we unlink the handle
	 * *before* performing the operation. It might not be obvious why this
	 * is safe, but the only two possible races are:
	 *
	 *   1) A local SEND/RELEASE/DESTROY ioctl that adds or removes
	 *      handles. Those are by definition undefined if run in parallel
	 *      to RESET. As such, it doesn't matter whether they operate on
	 *      the new or old tree.
	 *
	 *   2) A remote peer sends us a handle. If it happens on the old tree,
	 *      it will be cleaned up together with any previous handle. If it
	 *      happens on the new tree, it will create a possible duplicate
	 *      handle on the new tree and be treated as if it was another
	 *      peer. As such, it is fully involved in the transaction logic.
	 *
	 * Hence, a clean disconnect of the whole tree and later finalizing it
	 * async/unlocked will have the same effect as an atomic destruction of
	 * all owned nodes, followed by a non-atomic release of all handles.
	 */

	lockdep_assert_held(&peer_info->lock);

	write_seqcount_begin(&peer_info->seqcount);
	*map = peer_info->map_handles_by_id;
	WRITE_ONCE(peer_info->map_handles_by_id.rb_node, NULL);
	WRITE_ONCE(peer_info->map_handles_by_node.rb_node, NULL);
	write_seqcount_end(&peer_info->seqcount);

	rbtree_postorder_for_each_entry_safe(handle, t, map, rb_id)
		RB_CLEAR_NODE(&handle->rb_node);
}

/**
 * bus1_handle_finish_all() - finish set of handles
 * @peer_info:		peer to operate on
 * @map:		map of handles
 *
 * This is the tail call of bus1_handle_flush_all(). It destroys all owned
 * nodes of a peer, and releases all owned handles.
 *
 * This must be called *without* the peer lock held.
 */
void bus1_handle_finish_all(struct bus1_peer_info *peer_info,
			    struct rb_root *map)
{
	struct bus1_handle *handle, *t;
	LIST_HEAD(list_handles);

	/*
	 * See bus1_handle_flush_all() why it is safe to do this on a
	 * disconnected tree.
	 *
	 * Note that we might have racing RELEASE or DESTROY calls on handles
	 * linked to this tree. This is completely fine. They will work just
	 * like normal, but skip the rb-tree cleanup. However, we must make
	 * sure to only cleanup stuff here that is *not* racing us.
	 */

	rbtree_postorder_for_each_entry_safe(handle, t, map, rb_id) {
		if (bus1_handle_is_owner(handle)) {
			INIT_LIST_HEAD(&list_handles);
			mutex_lock(&peer_info->lock);
			if (handle->node->timestamp == 0)
				bus1_handle_commit_destruction(handle,
							       peer_info,
							       &list_handles);
			mutex_unlock(&peer_info->lock);
		} else {
			if (atomic_xchg(&handle->n_user, 0) > 0)
				bus1_handle_release_pinned(handle, peer_info);
		}

		/* our stolen reference from bus1_handle_unlink_rb() */
		bus1_handle_unref(handle);
	}

	bus1_handle_finalize_destruction(&list_handles);
}

/*
 * Handle Lists
 *
 * We support operations on large handle sets, bigger than we should allocate
 * linearly via kmalloc(). Hence, we rather use single-linked lists of
 * bus1_handle_entry arrays. Each entry in the list contains a maximum of
 * BUS1_HANDLE_BATCH_SIZE real entries. The BUS1_HANDLE_BATCH_SIZE+1'th entry
 * points to the next node in the linked list.
 *
 * bus1_handle_list_new() allocates a new list with space for @n entries. Such
 * lists can be released via bus1_handle_list_free().
 *
 * Entries are initially uninitialized. The caller has to fill them in.
 */

static void bus1_handle_list_free(union bus1_handle_entry *list, size_t n)
{
	union bus1_handle_entry *t;

	while (list && n > BUS1_HANDLE_BATCH_SIZE) {
		t = list;
		list = list[BUS1_HANDLE_BATCH_SIZE].next;
		kfree(t);
		n -= BUS1_HANDLE_BATCH_SIZE;
	}
	kfree(list);
}

static union bus1_handle_entry *bus1_handle_list_new(size_t n)
{
	union bus1_handle_entry list, *e, *slot;
	size_t remaining;

	list.next = NULL;
	slot = &list;
	remaining = n;

	while (remaining >= BUS1_HANDLE_BATCH_SIZE) {
		e = kmalloc(sizeof(*e) * (BUS1_HANDLE_BATCH_SIZE + 1),
			    GFP_KERNEL);
		if (!e)
			goto error;

		slot->next = e;
		slot = &e[BUS1_HANDLE_BATCH_SIZE];
		slot->next = NULL;

		remaining -= BUS1_HANDLE_BATCH_SIZE;
	}

	if (remaining > 0) {
		slot->next = kmalloc(sizeof(*e) * remaining, GFP_KERNEL);
		if (!slot->next)
			goto error;
	}

	return list.next;

error:
	bus1_handle_list_free(list.next, n);
	return NULL;
}

/*
 * Handle Batches
 *
 * A handle batch provides a convenience wrapper around handle lists. It embeds
 * the first node of the handle list into the batch object, but allocates the
 * remaining nodes on-demand.
 *
 * A handle-batch object is usually embedded into a parent object, and provides
 * space for a fixed number of handles (can be queried via batch->n_entries).
 * Initially, none of the entries is initialized. It is up to the user to fill
 * it with data.
 *
 * Batches can store two kinds of handles: Their IDs as entry->id, or a pinned
 * handle as entry->handle. By default it is assumed only IDs are stored, and
 * the caller can modify the batch freely. But once IDs are resolved to handles
 * and pinned in the batch, the caller must increment batch->n_handles for each
 * stored handle. This makes sure that the pinned handles are released on
 * destruction (starting at the front, up to @n_handles entries).
 *
 * Use the iterators BUS1_HANDLE_BATCH_FOREACH_ENTRY() and
 * BUS1_HANDLE_BATCH_FOREACH_HANDLE() to iterate either *all* entries, or only
 * the first entries up to the @n_handles'th entry (that is, iterate all entries
 * that have pinned handles).
 */

#define BUS1_HANDLE_BATCH_FIRST(_batch, _pos)			\
	((_pos) = 0, (_batch)->entries)

#define BUS1_HANDLE_BATCH_NEXT(_iter, _pos)			\
	((!(++(_pos) % BUS1_HANDLE_BATCH_SIZE)) ?		\
			((_iter) + 1)->next :			\
			((_iter) + 1))

#define BUS1_HANDLE_BATCH_FOREACH_ENTRY(_iter, _pos, _batch)		\
	for ((_iter) = BUS1_HANDLE_BATCH_FIRST((_batch), (_pos));	\
	     (_pos) < (_batch)->n_entries;				\
	     (_iter) = BUS1_HANDLE_BATCH_NEXT((_iter), (_pos)))

#define BUS1_HANDLE_BATCH_FOREACH_HANDLE(_iter, _pos, _batch)		\
	for ((_iter) = BUS1_HANDLE_BATCH_FIRST((_batch), (_pos));	\
	     (_pos) < (_batch)->n_handles;				\
	     (_iter) = BUS1_HANDLE_BATCH_NEXT((_iter), (_pos)))

static void bus1_handle_batch_init(struct bus1_handle_batch *batch,
				   size_t n_entries)
{
	batch->n_entries = n_entries;
	batch->n_handles = 0;
	if (n_entries >= BUS1_HANDLE_BATCH_SIZE)
		batch->entries[BUS1_HANDLE_BATCH_SIZE].next = NULL;
}

static int bus1_handle_batch_preload(struct bus1_handle_batch *batch)
{
	union bus1_handle_entry *e;

	/*
	 * If the number of stored entries fits into the static buffer, or if
	 * it was already pre-loaded, there is nothing to do.
	 */
	if (likely(batch->n_entries <= BUS1_HANDLE_BATCH_SIZE))
		return 0;
	if (batch->entries[BUS1_HANDLE_BATCH_SIZE].next)
		return 0;

	/* allocate handle-list for remaining, non-static entries */
	e = bus1_handle_list_new(batch->n_entries - BUS1_HANDLE_BATCH_SIZE);
	if (!e)
		return -ENOMEM;

	batch->entries[BUS1_HANDLE_BATCH_SIZE].next = e;
	return 0;
}

static void bus1_handle_batch_destroy(struct bus1_handle_batch *batch)
{
	union bus1_handle_entry *e;
	size_t pos;

	if (!batch || !batch->n_entries)
		return;

	BUS1_HANDLE_BATCH_FOREACH_HANDLE(e, pos, batch) {
		if (e->handle) {
			if (bus1_handle_is_public(e->handle))
				bus1_handle_release(e->handle);
			bus1_handle_unref(e->handle);
		}
	}

	if (unlikely(batch->n_entries > BUS1_HANDLE_BATCH_SIZE)) {
		e = batch->entries[BUS1_HANDLE_BATCH_SIZE].next;
		bus1_handle_list_free(e, batch->n_entries -
						BUS1_HANDLE_BATCH_SIZE);
	}

	batch->n_entries = 0;
	batch->n_handles = 0;
}

static int bus1_handle_batch_import(struct bus1_handle_batch *batch,
				    const u64 __user *ids,
				    size_t n_ids)
{
	union bus1_handle_entry *block;

	if (WARN_ON(n_ids != batch->n_entries || batch->n_handles > 0))
		return -EINVAL;

	BUILD_BUG_ON(sizeof(*block) != sizeof(*ids));

	block = batch->entries;
	while (n_ids > BUS1_HANDLE_BATCH_SIZE) {
		if (copy_from_user(block, ids,
				   BUS1_HANDLE_BATCH_SIZE * sizeof(*ids)))
			return -EFAULT;

		ids += BUS1_HANDLE_BATCH_SIZE;
		n_ids -= BUS1_HANDLE_BATCH_SIZE;
		block = block[BUS1_HANDLE_BATCH_SIZE].next;
	}

	if (n_ids > 0 && copy_from_user(block, ids, n_ids * sizeof(*ids)))
		return -EFAULT;

	return 0;
}

static size_t bus1_handle_batch_walk(struct bus1_handle_batch *batch,
				     size_t *pos,
				     u64 **idp)
{
	union bus1_handle_entry *block;
	size_t n;

	if (WARN_ON(batch->n_handles > 0))
		return 0;
	if (*pos >= batch->n_entries)
		return 0;

	BUILD_BUG_ON(sizeof(*block) != sizeof(**idp));

	n = batch->n_entries - *pos;
	if (n > BUS1_HANDLE_BATCH_SIZE)
		n = BUS1_HANDLE_BATCH_SIZE;

	if (*idp) {
		block = ((union bus1_handle_entry *)*idp);
		*idp = (void *)block[BUS1_HANDLE_BATCH_SIZE].next;
	} else {
		*idp = (void *)batch->entries;
	}

	*pos += n;
	return n;
}

/**
 * bus1_handle_transfer_init() - initialize handle transfer context
 * @transfer:		transfer context to initialize
 * @n_entries:		number of handles that are transferred
 *
 * This initializes a handle-transfer context. This object is needed to lookup,
 * pin, and optionally create, the handles of the sender during a transaction.
 * That is, for each transaction, you need one handle-transfer object,
 * initialized with the number of handles to transfer.
 *
 * Handles can be imported via bus1_handle_transfer_instantiate(). Once done,
 * the handle-inflight objects can be instantiated from it for each destination
 * of the transaction.
 *
 * The handle-transfer context embeds a handle-batch, as such must be
 * pre-allocated via bus1_handle_batch_inline_size().
 */
void bus1_handle_transfer_init(struct bus1_handle_transfer *transfer,
			       size_t n_entries)
{
	transfer->n_new = 0;
	bus1_handle_batch_init(&transfer->batch, n_entries);
}

/**
 * bus1_handle_transfer_destroy() - destroy handle transfer context
 * @transfer:		transfer context to destroy, or NULL
 *
 * This releases all data allocated, or pinned by a handle-transfer context. If
 * NULL is passed, or if the transfer object was already destroyed, then
 * nothing is done.
 */
void bus1_handle_transfer_destroy(struct bus1_handle_transfer *transfer)
{
	if (!transfer)
		return;

	/* safe to be called multiple times */
	bus1_handle_batch_destroy(&transfer->batch);
}

/**
 * bus1_handle_transfer_instantiate() - instantiate handles for transfer
 * @transfer:		transfer context
 * @peer_info:		peer to import handles of
 * @ids:		user-space array of handle IDs
 * @n_ids:		number of IDs in @ids
 *
 * This imports an array of handle-IDs from user-space (provided as @ids +
 * @n_ids) into the transfer context. It then resolves each of them to their
 * actual bus1_handle objects, optionally creating new ones on demand.
 *
 * This can only be called once per transfer context. Also, @n_ids must match
 * the size used with bus1_handle_transfer_init().
 *
 * Return: 0 on success, negative error code on failure.
 */
int bus1_handle_transfer_instantiate(struct bus1_handle_transfer *transfer,
				     struct bus1_peer_info *peer_info,
				     const u64 __user *ids,
				     size_t n_ids)
{
	union bus1_handle_entry *entry;
	struct bus1_handle *handle;
	size_t pos;
	int r;

	/*
	 * Import the handle IDs from user-space (@ids + @n_ids) into the
	 * handle-batch. Then resolve each of them and pin their underlying
	 * handle. If a new node is demanded, we allocate a fresh node+handle,
	 * but do *not* link it, yet. We just make sure it is allocated, so the
	 * final commit cannot fail due to OOM.
	 *
	 * Note that the batch-import refuses operation if already used, so we
	 * can rely on @n_handles to be 0.
	 */

	r = bus1_handle_batch_preload(&transfer->batch);
	if (r < 0)
		return r;

	r = bus1_handle_batch_import(&transfer->batch, ids, n_ids);
	if (r < 0)
		return r;

	BUS1_HANDLE_BATCH_FOREACH_ENTRY(entry, pos, &transfer->batch) {
		if (entry->id & BUS1_NODE_FLAG_ALLOCATE) {
			/*
			 * Right now we only support allocating managed nodes.
			 * All the upper command flags must be unset, as they
			 * are reserved for the future.
			 */
			if ((entry->id & ~BUS1_NODE_FLAG_ALLOCATE) !=
							BUS1_NODE_FLAG_MANAGED)
				return -EINVAL;

			handle = bus1_handle_new();
			if (IS_ERR(handle))
				return PTR_ERR(handle);

			++transfer->n_new;
		} else {
			/*
			 * If you transfer non-existant, or destructed handles,
			 * we simply store NULL in the batch. We might
			 * optionally allow returning an error instead. But
			 * given the async nature of handle destruction, it
			 * seems rather unlikely that callers want to handle
			 * that.
			 */
			handle = bus1_handle_find_by_id(peer_info, entry->id);
			if (handle && !bus1_handle_acquire(handle))
				handle = bus1_handle_unref(handle);
		}

		entry->handle = handle;
		++transfer->batch.n_handles;
	}

	return 0;
}

/**
 * bus1_handle_inflight_init() - initialize inflight context
 * @inflight:		inflight context to initialize
 * @n_entries:		number of entries to store in this context
 *
 * This initializes an inflight-context to carry @n_entries handles. An
 * inflight-context is used to instantiate and commit the handles a peer
 * *receives* via a transaction. That is, it is created once for each
 * destination of a transaction, and it is instantiated from the
 * transfer-context of the transaction origin/sender.
 *
 * The inflight-context embeds a handle-batch, as such must be pre-allocated
 * via bus1_handle_batch_inline_size().
 */
void bus1_handle_inflight_init(struct bus1_handle_inflight *inflight,
			       size_t n_entries)
{
	inflight->n_new = 0;
	inflight->n_new_local = 0;
	bus1_handle_batch_init(&inflight->batch, n_entries);
}

/**
 * bus1_handle_inflight_destroy() - destroy inflight-context
 * @inflight:		inflight context to destroy, or NULL
 *
 * This releases all data allocated, or pinned by an inflight-context. If NULL
 * is passed, or if the inflight context was already destroyed, then nothing is
 * done.
 */
void bus1_handle_inflight_destroy(struct bus1_handle_inflight *inflight)
{
	if (!inflight)
		return;

	/* safe to be called multiple times */
	bus1_handle_batch_destroy(&inflight->batch);
}

/**
 * bus1_handle_inflight_instantiate() - instantiate inflight context
 * @inflight:		inflight context to instantiate
 * @peer_info:		peer info to instantiate for
 * @transfer:		transfer object to instantiate from
 *
 * Instantiate an inflight-context from an existing transfer-context. Import
 * each pinned handle from the transfer-context into the peer @peer_info,
 * creating new handles if required. All the handles are pinned in the inflight
 * context, but not committed, yet.
 *
 * This must only be called once per inflight object. Furthermore, the number
 * of handles must match the number of handles of the transfer-context.
 *
 * Return: 0 on success, negative error code on failure.
 */
int bus1_handle_inflight_instantiate(struct bus1_handle_inflight *inflight,
				     struct bus1_peer_info *peer_info,
				     struct bus1_handle_transfer *transfer)
{
	union bus1_handle_entry *from, *to;
	struct bus1_handle *handle;
	size_t pos_from, pos_to;
	int r;

	r = bus1_handle_batch_preload(&inflight->batch);
	if (r < 0)
		return r;
	if (WARN_ON(inflight->batch.n_handles > 0))
		return -EINVAL;
	if (WARN_ON(inflight->batch.n_entries != transfer->batch.n_entries))
		return -EINVAL;

	to = BUS1_HANDLE_BATCH_FIRST(&inflight->batch, pos_to);

	BUS1_HANDLE_BATCH_FOREACH_HANDLE(from, pos_from, &transfer->batch) {
		WARN_ON(pos_to >= inflight->batch.n_entries);

		if (!from->handle) {
			handle = NULL;
		} else {
			handle = bus1_handle_find_by_node(peer_info,
							  from->handle);
			if (handle && !bus1_handle_acquire(handle))
				handle = bus1_handle_unref(handle);
			if (!handle) {
				handle = bus1_handle_new_copy(from->handle);
				if (IS_ERR(handle))
					return PTR_ERR(handle);
			}
		}

		to->handle = handle;
		to = BUS1_HANDLE_BATCH_NEXT(to, pos_to);
		++inflight->batch.n_handles;
	}

	return 0;
}

/**
 * bus1_handle_inflight_install() - install inflight handles
 * @inflight:		instantiated inflight context
 * @dst:		peer @inflight is for
 * @transfer:		transfer context
 * @src:		peer @transfer is from
 *
 * After an inflight context was successfully instantiated, this will install
 * the handles into the peer @dst. The caller must provide the used transfer
 * context and the origin peer as @transfer and @src.
 */
void bus1_handle_inflight_install(struct bus1_handle_inflight *inflight,
				  struct bus1_peer *dst,
				  struct bus1_handle_transfer *transfer,
				  struct bus1_peer *src)
{
	struct bus1_peer_info *src_info, *dst_info;
	struct bus1_handle *h, *t;
	union bus1_handle_entry *e;
	size_t pos, n_installs;

	if (inflight->batch.n_handles < 1)
		return;

	src_info = bus1_peer_dereference(src);
	dst_info = bus1_peer_dereference(dst);
	n_installs = inflight->n_new;

	if (transfer->n_new > 0 || inflight->n_new_local > 0) {
		mutex_lock(&src_info->lock);

		BUS1_HANDLE_BATCH_FOREACH_HANDLE(e, pos, &transfer->batch) {
			if (transfer->n_new < 1)
				break;

			h = e->handle;
			if (!h || bus1_handle_is_public(h))
				continue;

			--transfer->n_new;
			WARN_ON(!bus1_handle_attach_unlocked(h, src));
			WARN_ON(bus1_handle_install_unlocked(h) != h);
		}
		WARN_ON(transfer->n_new > 0);

		BUS1_HANDLE_BATCH_FOREACH_HANDLE(e, pos, &inflight->batch) {
			if (inflight->n_new_local < 1)
				break;

			h = e->handle;
			if (!h || bus1_handle_is_public(h))
				continue;

			--inflight->n_new;
			--inflight->n_new_local;

			if (!bus1_handle_attach_unlocked(h, dst))
				e->handle = bus1_handle_unref(h);
		}
		WARN_ON(inflight->n_new_local > 0);

		mutex_unlock(&src_info->lock);
	}

	if (inflight->n_new > 0) {
		BUS1_HANDLE_BATCH_FOREACH_HANDLE(e, pos, &inflight->batch) {
			if (inflight->n_new < 1)
				break;

			h = e->handle;
			if (!h || bus1_handle_is_public(h))
				continue;

			--inflight->n_new;

			if (!bus1_handle_attach(h, dst))
				e->handle = bus1_handle_unref(h);
		}
		WARN_ON(inflight->n_new > 0);
	}

	if (n_installs > 0) {
		mutex_lock(&dst_info->lock);
		BUS1_HANDLE_BATCH_FOREACH_HANDLE(e, pos, &inflight->batch) {
			if (n_installs < 1)
				break;

			h = e->handle;
			if (!h || bus1_handle_has_id(h))
				continue;
			if (WARN_ON(!bus1_handle_is_public(h)))
				continue;

			--n_installs;

			t = bus1_handle_install_unlocked(h);
			if (t != h) {
				mutex_unlock(&dst_info->lock);
				bus1_handle_release(h);
				bus1_handle_unref(h);
				e->handle = t;
				mutex_lock(&dst_info->lock);
			}
		}
		mutex_unlock(&dst_info->lock);
		WARN_ON(n_installs > 0);
	}
}

/**
 * bus1_handle_inflight_commit() - commit inflight context
 * @inflight:		inflight context to commit
 * @seq:		sequence number of transaction
 *
 * This commits a fully installed inflight context, given the sequence number
 * of the transaction. This will make sure to only transfer the actual handle
 * if it is ordered *before* the handle destruction.
 */
void bus1_handle_inflight_commit(struct bus1_handle_inflight *inflight,
				 u64 seq)
{
	union bus1_handle_entry *e;
	struct bus1_handle *h;
	size_t pos;

	WARN_ON(inflight->batch.n_handles != inflight->batch.n_entries);

	BUS1_HANDLE_BATCH_FOREACH_HANDLE(e, pos, &inflight->batch) {
		h = e->handle;
		if (h) {
			e->id = bus1_handle_release_to_inflight(h, seq);
			bus1_handle_unref(h);
		} else {
			e->id = BUS1_HANDLE_INVALID;
		}
	}

	inflight->batch.n_handles = 0;
}

/**
 * bus1_handle_inflight_walk() - walk all handle IDs
 * @inflight:		inflight context to walk
 * @pos:		current iterator position
 * @idp:		current block
 *
 * This walks over all stored handle IDs of @inflight, returning them in blocks
 * to the caller. The caller must initialize @pos to 0, @idp to NULL and
 * provide both as iterators to this function.
 *
 * This function updates @idp to point to the next block of IDs, forwards @pos
 * to point to the end of the block, and returns the number of IDs in this
 * block. Once done it returns 0.
 *
 * Return: Number of IDs in the next block, 0 if done.
 */
size_t bus1_handle_inflight_walk(struct bus1_handle_inflight *inflight,
				 size_t *pos,
				 u64 **idp)
{
	return bus1_handle_batch_walk(&inflight->batch, pos, idp);
}

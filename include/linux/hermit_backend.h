/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HERMIT_BACKEND_H
#define _LINUX_HERMIT_BACKEND_H

#include <linux/swap.h>

struct folio;
struct dentry;

struct hermit_io {
	swp_entry_t entry;
	struct folio *folio;
	unsigned int folio_order;
	unsigned int transfer_order;
	int cpu;
	bool fallback;
	void *private;
};

struct hermit_backend_ops {
	unsigned long supported_order_mask;
	int (*load)(struct hermit_io *io, bool async);
	/*
	 * store() must make every page of io->folio durably remote before
	 * returning 0.  io->fallback only reports transfer granularity (the
	 * folio was sent as base-page WRs instead of one large WR), never
	 * partial success; a store that cannot transfer the whole folio must
	 * return an error.  The kernel aborts the whole remote extent on any
	 * error and falls back to the native swap device.
	 */
	int (*store)(struct hermit_io *io);
	int (*poll)(struct hermit_io *io, bool wait);
};

int hermit_register_backend(const struct hermit_backend_ops *ops);
void hermit_unregister_backend(const struct hermit_backend_ops *ops);
bool hermit_backend_ready(void);
int hermit_backend_load(struct hermit_io *io, bool async);
int hermit_backend_store(struct hermit_io *io);
int hermit_backend_poll(struct hermit_io *io, bool wait);
unsigned long hermit_backend_effective_order_mask(void);
unsigned int hermit_backend_transfer_order(unsigned int folio_order);
int hermit_backend_prepare_remote(swp_entry_t entry, unsigned int order);
int hermit_backend_commit_remote(swp_entry_t entry, unsigned int order);
void hermit_backend_abort_remote(swp_entry_t entry, unsigned int order);
bool hermit_backend_entry_remote(swp_entry_t entry);
bool hermit_backend_range_remote(swp_entry_t entry, unsigned int order);
int hermit_backend_entry_order(swp_entry_t entry);
void hermit_backend_invalidate(swp_entry_t entry);
void hermit_backend_account(unsigned int order, bool store, bool fallback,
			    int error);
int hermit_backend_debugfs_init(struct dentry *root);

#endif

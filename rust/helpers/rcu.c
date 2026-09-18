// SPDX-License-Identifier: GPL-2.0

#include <linux/rcupdate.h>
#include <linux/rcupdate_trace.h>

__rust_helper void rust_helper_rcu_read_lock(void)
{
	rcu_read_lock();
}

__rust_helper void rust_helper_rcu_read_unlock(void)
{
	rcu_read_unlock();
}

__rust_helper void rust_helper_rcu_read_lock_trace(void)
{
	rcu_read_lock_trace();
}

__rust_helper void rust_helper_rcu_read_unlock_trace(void)
{
	rcu_read_unlock_trace();
}

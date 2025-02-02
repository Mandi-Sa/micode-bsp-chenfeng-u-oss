// SPDX-License-Identifier: GPL-2.0
/*
* Copyright (C) 2023 Xiaomi Inc.
* Authors:
*	Tianyang cao <caotianyang@xiaomi.com>
*/


#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/miscdevice.h>
#include "mpbe.h"

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

#define EARA_IOCTL_MAX_SIZE 27
struct _EARA_IOCTL_PACKAGE {
	union {
		__s32 cmd;
		__s32 data[EARA_IOCTL_MAX_SIZE];
	};
};

#define EARA_GETINDEX                _IOW('g', 1, struct _EARA_IOCTL_PACKAGE)
#define EARA_COLLECT                 _IOW('g', 2, struct _EARA_IOCTL_PACKAGE)

struct eara_iostat {
	int io_wl;
	int io_top;
	int io_reqc_r;
	int io_reqc_w;
	int io_q_dept;
	int io_reqsz_r;
	int io_reqsz_w;
};

static DEFINE_MUTEX(eara_ioctl_lock);

/* mini context for major embedded storage only */
#define MICTX_PROC_CMD_BUF_SIZE (1)
#define PWD_WIDTH_NS 100000000 /* 100ms */
static struct mpbe_earaio_control earaio_ctrl;
static struct miscdevice earaio_obj;

#if IS_ENABLED(CONFIG_CGROUP_SCHED)
static void mpbe_eara_get_data(struct eara_iostat *data)
{
	struct mpbe_mictx_iostat_struct iostat = {0};

	WARN_ON(!mutex_is_locked(&eara_ioctl_lock));

	if (mpbe_mictx_get_data(earaio_ctrl.mictx_id, &iostat))
		mpbe_mictx_enable(&earaio_ctrl.mictx_id, 1);

	data->io_wl = iostat.wl;
	data->io_top = iostat.top;
	data->io_reqc_r = iostat.reqcnt_r;
	data->io_reqc_w = iostat.reqcnt_w;
	data->io_q_dept = iostat.q_depth;
	data->io_reqsz_r = iostat.reqsize_r;
	data->io_reqsz_w = iostat.reqsize_w;
}

static void mpbe_eara_start_collect(void)
{
	struct eara_iostat data = {0};

	WARN_ON(!mutex_is_locked(&eara_ioctl_lock));

	mpbe_eara_get_data(&data);
}

static void mpbe_eara_stop_collect(void)
{
	mpbe_earaio_boost(false);

	WARN_ON(!mutex_is_locked(&eara_ioctl_lock));
}

static void mpbe_eara_switch_collect(int cmd)
{
	mutex_lock(&eara_ioctl_lock);

	if (cmd)
		mpbe_eara_start_collect();
	else
		mpbe_eara_stop_collect();

	mutex_unlock(&eara_ioctl_lock);
}

static void mpbe_eara_transfer_data(__s32 *data, __s32 input_size)
{
	struct eara_iostat eara_io_data = {0};
	int limit_size;

	mutex_lock(&eara_ioctl_lock);
	mpbe_eara_get_data(&eara_io_data);
	mutex_unlock(&eara_ioctl_lock);

	limit_size = MIN(input_size, sizeof(struct eara_iostat));
	memcpy(data, &eara_io_data, limit_size);
}

static unsigned long eara_ioctl_copy_from_user(void *pvTo,
		const void __user *pvFrom, unsigned long ulBytes)
{
	if (access_ok(pvFrom, ulBytes))
		return __copy_from_user(pvTo, pvFrom, ulBytes);

	return ulBytes;
}

static unsigned long eara_ioctl_copy_to_user(void __user *pvTo,
		const void *pvFrom, unsigned long ulBytes)
{
	if (access_ok(pvTo, ulBytes))
		return __copy_to_user(pvTo, pvFrom, ulBytes);

	return ulBytes;
}

static long mpbe_eara_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	ssize_t ret = 0;
	struct _EARA_IOCTL_PACKAGE *msgKM = NULL;
	struct _EARA_IOCTL_PACKAGE *msgUM = (struct _EARA_IOCTL_PACKAGE *)arg;
	struct _EARA_IOCTL_PACKAGE smsgKM = {0};

	msgKM = &smsgKM;

	switch (cmd) {
	case EARA_GETINDEX:
		mpbe_eara_transfer_data(smsgKM.data,
				sizeof(struct _EARA_IOCTL_PACKAGE));
		eara_ioctl_copy_to_user(msgUM, msgKM,
				sizeof(struct _EARA_IOCTL_PACKAGE));
		break;
	case EARA_COLLECT:
		if (eara_ioctl_copy_from_user(msgKM, msgUM,
				sizeof(struct _EARA_IOCTL_PACKAGE))) {
			ret = -EFAULT;
			goto ret_ioctl;
		}
		mpbe_eara_switch_collect(msgKM->cmd);
		break;
	default:
		pr_debug("[BLOCK TAG] %s %d: unknown cmd %x\n",
				__FILE__, __LINE__, cmd);
		ret = -EINVAL;
		goto ret_ioctl;
	}

ret_ioctl:
	return ret;
}

static int mpbe_eara_ioctl_show(struct seq_file *m, void *v)
{
	return 0;
}

static int mpbe_eara_ioctl_open(struct inode *inode, struct file *file)
{
	return single_open(file, mpbe_eara_ioctl_show, inode->i_private);
}

static const struct proc_ops mpbe_eara_ioctl_fops = {
	.proc_ioctl = mpbe_eara_ioctl,
	.proc_compat_ioctl = mpbe_eara_ioctl,
	.proc_open = mpbe_eara_ioctl_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int mpbe_eara_ioctl_init(struct proc_dir_entry *parent)
{
	int ret = 0;
	struct proc_dir_entry *proc_entry;

	proc_entry = proc_create("eara_io",
		0664, parent, &mpbe_eara_ioctl_fops);

	if (IS_ERR(proc_entry)) {
		pr_debug("[BLOCK TAG] Creating file node failed with %d\n Creating file node ",
				ret);
		ret = -ENOMEM;
	}

	return ret;
}

static ssize_t mpbe_earaio_ctrl_sub_write(struct file *file,
	const char __user *ubuf, size_t count, loff_t *ppos)
{
	int ret;
	char cmd[MICTX_PROC_CMD_BUF_SIZE] = {0};

	if (count == 0)
		goto err;
	else if (count > MICTX_PROC_CMD_BUF_SIZE)
		count = MICTX_PROC_CMD_BUF_SIZE;

	ret = copy_from_user(cmd, ubuf, count);

	if (ret < 0)
		goto err;

	if (cmd[0] == '1') {
		earaio_ctrl.enabled = true;
		pr_info("[MPBE] EARA-IO QoS: allowed\n");
	} else if (cmd[0] == '0') {
		mpbe_earaio_boost(false);
		earaio_ctrl.enabled = false;
		pr_info("[MPBE] EARA-IO QoS: disallowed\n");
	} else {
		pr_info("[MPBE] invalid arg: 0x%x\n", cmd[0]);
		goto err;
	}

	return count;

err:
	return -1;
}

static int mpbe_earaio_ctrl_sub_show(struct seq_file *s, void *data)
{
	struct mpbe *btag;
	char name[MPBE_NAME_LEN] = {' '};

	btag = mpbe_find_by_type(earaio_ctrl.mictx_id.storage);
	if (btag)
		strncpy(name, btag->name, MPBE_NAME_LEN-1);

	seq_puts(s, "<MPBE EARA-IO Control Unit>\n");
	seq_printf(s, "Monitor Storage Type: %s\n", name);
	seq_puts(s, "Status:\n");
	seq_printf(s, "  EARA-IO Control Enable: %d\n", earaio_ctrl.enabled);
	seq_puts(s, "Commands: echo n > blockio_mictx, n presents\n");
	seq_puts(s, "  Enable EARA-IO QoS  : 1\n");
	seq_puts(s, "  Disable EARA-IO QoS : 0\n");
	return 0;
}

static const struct seq_operations mpbe_seq_earaio_ctrl_ops = {
	.start  = mpbe_seq_debug_start,
	.next   = mpbe_seq_debug_next,
	.stop   = mpbe_seq_debug_stop,
	.show   = mpbe_earaio_ctrl_sub_show,
};

static int mpbe_earaio_ctrl_sub_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &mpbe_seq_earaio_ctrl_ops);
}

static const struct proc_ops mpbe_earaio_ctrl_sub_fops = {
	.proc_open		= mpbe_earaio_ctrl_sub_open,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release		= seq_release,
	.proc_write		= mpbe_earaio_ctrl_sub_write,
};

static void mpbe_earaio_uevt_worker(struct work_struct *work)
{
	#define EVT_STR_SIZE 10
	unsigned long flags;
	char event_string[EVT_STR_SIZE];
	char *envp[2];
	bool boost, restart, quit;
	int ret;

	envp[0] = event_string;
	envp[1] = NULL;

start:
	boost = false;
	quit = false;
	restart = false;
	spin_lock_irqsave(&earaio_ctrl.lock, flags);
	if (earaio_ctrl.uevt_state != earaio_ctrl.uevt_req)
		boost = earaio_ctrl.uevt_req;
	else
		quit = true;
	spin_unlock_irqrestore(&earaio_ctrl.lock, flags);

	MPBE_DBG("enter,earaio_ctrl.uevt_state=%d,earaio_ctrl.uevt_req=%d\n",  earaio_ctrl.uevt_state, earaio_ctrl.uevt_req);

	if (quit)
		return;

	MPBE_DBG("send uevent,boost=%d\n",boost);
	ret = snprintf(event_string, EVT_STR_SIZE, "boost=%d", boost ? 1 : 0);
	if (!ret)
		return;

	ret = kobject_uevent_env(
			&earaio_obj.this_device->kobj,
			KOBJ_CHANGE, envp);
	if (ret)
		pr_info("[MPBE] uevt: %s sent fail:%d", event_string, ret);
	else
		earaio_ctrl.uevt_state = boost;

	spin_lock_irqsave(&earaio_ctrl.lock, flags);
	if (earaio_ctrl.uevt_state != earaio_ctrl.uevt_req)
		restart = true;
	spin_unlock_irqrestore(&earaio_ctrl.lock, flags);

	if (restart)
		goto start;
}

static bool mpbe_earaio_send_uevt(bool boost)
{
	earaio_ctrl.uevt_req = boost;
	MPBE_DBG("enter,boost=%d\n",boost);
	queue_work(earaio_ctrl.uevt_workq, &earaio_ctrl.uevt_work);

	return true;
}

#define EARAIO_UEVT_THRESHOLD_PAGES ((32 * 1024 * 1024) >> 12)
//#define EARAIO_UEVT_THRESHOLD_PAGES ((1 * 1024 * 1024) >> 12)
static int __mpbe_earaio_boost(bool boost)
{
	int changed = 0;

	if (!(boost ^ earaio_ctrl.boosted))
		return changed;

	if (boost) {
		MPBE_DBG("enter,pwd_top_r_pages=0x%x,earaio_ctrl.pwd_top_w_pages=%d\n",earaio_ctrl.pwd_top_r_pages,earaio_ctrl.pwd_top_w_pages);
		/* Establish threshold to avoid lousy uevents */
		if ((earaio_ctrl.pwd_top_r_pages >= EARAIO_UEVT_THRESHOLD_PAGES) ||
			(earaio_ctrl.pwd_top_w_pages >= EARAIO_UEVT_THRESHOLD_PAGES))
			changed = mpbe_earaio_send_uevt(true);
	} else {
		changed = mpbe_earaio_send_uevt(false);
		changed = 1;
	}

	if (changed)
		earaio_ctrl.boosted = boost;
	else
		changed = 2;

	return changed;
}

void mpbe_earaio_boost(bool boost)
{
	unsigned long flags;
	int changed = 0; // 0: not try, 1: try and success, 2: try but fail

	MPBE_DBG("enter,boost=%d\n",boost);
	/* Use earaio_obj.minor to indicate if obj is existed */
	if (!earaio_ctrl.enabled || unlikely(!earaio_obj.minor))
		return;

	spin_lock_irqsave(&earaio_ctrl.lock, flags);
	changed = __mpbe_earaio_boost(boost);
	if (boost || changed == 1) {
		MPBE_DBG("earaio_ctrl pwd_begin&pwd_top_r_pages&pwd_top_w_pages reset!\n");
		earaio_ctrl.pwd_begin = sched_clock();
		earaio_ctrl.pwd_top_r_pages = 0;
		earaio_ctrl.pwd_top_w_pages = 0;
	}
	spin_unlock_irqrestore(&earaio_ctrl.lock, flags);

	if ((boost && changed == 2) || (!boost && changed == 1))
		mpbe_mictx_check_window(earaio_ctrl.mictx_id);
}

void mpbe_earaio_check_pwd(void)
{
	if ((sched_clock() - earaio_ctrl.pwd_begin) >= PWD_WIDTH_NS)
		mpbe_earaio_boost(true);
}

bool mpbe_earaio_enabled(void)
{
	return earaio_ctrl.enabled;
}

void mpbe_earaio_update_pwd(bool write, __u32 size)
{
	unsigned long flags;

	spin_lock_irqsave(&earaio_ctrl.lock, flags);
	if (write)
		earaio_ctrl.pwd_top_w_pages += (size >> 12);
	else
		earaio_ctrl.pwd_top_r_pages += (size >> 12);
	spin_unlock_irqrestore(&earaio_ctrl.lock, flags);
}

void mpbe_earaio_init_mictx(
	struct mpbe_vops *vops,
	enum mpbe_storage_type storage_type,
	struct proc_dir_entry *btag_proc_root)
{
	struct proc_dir_entry *proc_entry;

	if (!vops->earaio_enabled)
		return;

	if (!earaio_ctrl.enabled) {
		earaio_ctrl.uevt_workq =
			alloc_ordered_workqueue("mpbe_uevt",
			WQ_MEM_RECLAIM);
		INIT_WORK(&earaio_ctrl.uevt_work, mpbe_earaio_uevt_worker);
		spin_lock_init(&earaio_ctrl.lock);
		earaio_ctrl.enabled = true;
		earaio_ctrl.pwd_begin = sched_clock();
		earaio_ctrl.pwd_top_r_pages = 0;
		earaio_ctrl.pwd_top_w_pages = 0;
		earaio_ctrl.mictx_id.storage = storage_type;
	}

	/* Enable mictx by default if EARA-IO is enabled*/
	mpbe_mictx_enable(&earaio_ctrl.mictx_id, 1);

	mpbe_eara_ioctl_init(btag_proc_root);
	proc_entry = proc_create("earaio_ctrl", S_IFREG | 0444,
		btag_proc_root, &mpbe_earaio_ctrl_sub_fops);
}

int mpbe_earaio_init(void)
{
	int ret = 0;

	earaio_obj.name = "eara-io";
	earaio_obj.minor = MISC_DYNAMIC_MINOR;
	ret = misc_register(&earaio_obj);
	if (ret) {
		pr_info("[MPBE] register earaio obj error:%d\n", ret);
		earaio_obj.minor = 0;
		return ret;
	}

	ret = kobject_uevent(&earaio_obj.this_device->kobj, KOBJ_ADD);
	if (ret) {
		misc_deregister(&earaio_obj);
		pr_info("[MPBE] add uevent fail:%d\n", ret);
		earaio_obj.minor = 0;
		return ret;
	}

	return ret;
}

#else
static inline int mpbe_earaio_init(void)
{
	return -1;
}

#define mpbe_earaio_init_mictx(...)
#endif

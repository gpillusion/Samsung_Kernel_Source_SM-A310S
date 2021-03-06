/*
 *
 * (C) COPYRIGHT 2010-2017 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained
 * from Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */





/**
 * @file mali_kbase_pm.c
 * Base kernel power management APIs
 */

#include <mali_kbase.h>
#include <mali_midg_regmap.h>
#include <mali_kbase_vinstr.h>

#include <mali_kbase_pm.h>
#ifdef CONFIG_MALI_SEC_UTILIZATION
#include <backend/gpu/mali_kbase_pm_internal.h>
#endif

int kbase_pm_powerup(struct kbase_device *kbdev, unsigned int flags)
{
	return kbase_hwaccess_pm_powerup(kbdev, flags);
}

void kbase_pm_halt(struct kbase_device *kbdev)
{
	kbase_hwaccess_pm_halt(kbdev);
}

void kbase_pm_context_active(struct kbase_device *kbdev)
{
	(void)kbase_pm_context_active_handle_suspend(kbdev, KBASE_PM_SUSPEND_HANDLER_NOT_POSSIBLE);
}
int kbase_pm_context_hold_noactivate_handle_suspend(struct kbase_device *kbdev,
				enum kbase_pm_suspend_handler suspend_handler)
{
	struct kbasep_js_device_data *js_devdata = &kbdev->js_data;
	unsigned long flags1, flags2;
	spinlock_t *gpu_powered_lock = &kbdev->pm.backend.gpu_powered_lock;

	KBASE_DEBUG_ASSERT(kbdev != NULL);

	mutex_lock(&js_devdata->runpool_mutex);
	mutex_lock(&kbdev->pm.lock);

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags1);

	/* check the actual PM state, active_count isn't enough */
	spin_lock_irqsave(gpu_powered_lock, flags2);
	if (!kbdev->pm.backend.gpu_powered) {
		/* GPU is turned off - don't turn it on for this. */
		spin_unlock_irqrestore(gpu_powered_lock, flags2);
		spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags1);
		mutex_unlock(&kbdev->pm.lock);
		mutex_unlock(&js_devdata->runpool_mutex);

		return -ENOENT;
	}

	if (kbase_pm_is_suspending(kbdev)) {
		switch (suspend_handler) {
		case KBASE_PM_SUSPEND_HANDLER_DONT_REACTIVATE:
			break;
		case KBASE_PM_SUSPEND_HANDLER_DONT_INCREASE:
			spin_unlock_irqrestore(gpu_powered_lock, flags2);
			spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags1);
			mutex_unlock(&kbdev->pm.lock);
			mutex_unlock(&js_devdata->runpool_mutex);
			return -EAGAIN;
		case KBASE_PM_SUSPEND_HANDLER_NOT_POSSIBLE:
			/* FALLTHROUGH */
		default:
			KBASE_DEBUG_ASSERT_MSG(false, "unreachable");
			break;
		}
	}

	if (1 == ++kbdev->pm.active_count)
		kbase_hwaccess_pm_gpu_keep_active_nolock(kbdev);

	spin_unlock_irqrestore(gpu_powered_lock, flags2);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags1);
	mutex_unlock(&kbdev->pm.lock);
	mutex_unlock(&js_devdata->runpool_mutex);

	return 0;
}

int kbase_pm_context_active_handle_suspend(struct kbase_device *kbdev, enum kbase_pm_suspend_handler suspend_handler)
{
	struct kbasep_js_device_data *js_devdata = &kbdev->js_data;
	int c;
	int old_count;
#ifdef CONFIG_MALI_SEC_UTILIZATION
	unsigned long flags;
	ktime_t now;
#endif

	KBASE_DEBUG_ASSERT(kbdev != NULL);

	/* Trace timeline information about how long it took to handle the decision
	 * to powerup. Sometimes the event might be missed due to reading the count
	 * outside of mutex, but this is necessary to get the trace timing
	 * correct. */
	old_count = kbdev->pm.active_count;
	if (old_count == 0)
		kbase_timeline_pm_send_event(kbdev, KBASE_TIMELINE_PM_EVENT_GPU_ACTIVE);

	mutex_lock(&js_devdata->runpool_mutex);
	mutex_lock(&kbdev->pm.lock);
	if (kbase_pm_is_suspending(kbdev)) {
		switch (suspend_handler) {
		case KBASE_PM_SUSPEND_HANDLER_DONT_REACTIVATE:
			if (kbdev->pm.active_count != 0)
				break;
			/* FALLTHROUGH */
		case KBASE_PM_SUSPEND_HANDLER_DONT_INCREASE:
			mutex_unlock(&kbdev->pm.lock);
			mutex_unlock(&js_devdata->runpool_mutex);
			if (old_count == 0)
				kbase_timeline_pm_handle_event(kbdev, KBASE_TIMELINE_PM_EVENT_GPU_ACTIVE);
			return 1;

		case KBASE_PM_SUSPEND_HANDLER_NOT_POSSIBLE:
			/* FALLTHROUGH */
		default:
			KBASE_DEBUG_ASSERT_MSG(false, "unreachable");
			break;
		}
	}
	c = ++kbdev->pm.active_count;
	KBASE_TIMELINE_CONTEXT_ACTIVE(kbdev, c);
	KBASE_TRACE_ADD_REFCOUNT(kbdev, PM_CONTEXT_ACTIVE, NULL, NULL, 0u, c);

	/* Trace the event being handled */
	if (old_count == 0)
		kbase_timeline_pm_handle_event(kbdev, KBASE_TIMELINE_PM_EVENT_GPU_ACTIVE);

	if (c == 1) {
		/* First context active: Power on the GPU and any cores requested by
		 * the policy */
		kbase_hwaccess_pm_gpu_active(kbdev);
#ifdef CONFIG_MALI_SEC_UTILIZATION
        now = ktime_get();
        spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
        kbase_pm_metrics_update(kbdev, &now);
        spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
        spin_lock_irqsave(&kbdev->pm.backend.metrics.lock, flags);
        kbdev->pm.backend.metrics.gpu_active = true;
        spin_unlock_irqrestore(&kbdev->pm.backend.metrics.lock, flags);
#endif
	}
	mutex_unlock(&kbdev->pm.lock);
	mutex_unlock(&js_devdata->runpool_mutex);

	return 0;
}

KBASE_EXPORT_TEST_API(kbase_pm_context_active);

void kbase_pm_context_idle(struct kbase_device *kbdev)
{
	struct kbasep_js_device_data *js_devdata = &kbdev->js_data;
	int c;
	int old_count;
#ifdef CONFIG_MALI_SEC_UTILIZATION
	unsigned long flags;
	ktime_t now;
#endif

	KBASE_DEBUG_ASSERT(kbdev != NULL);

	/* Trace timeline information about how long it took to handle the decision
	 * to powerdown. Sometimes the event might be missed due to reading the
	 * count outside of mutex, but this is necessary to get the trace timing
	 * correct. */
	old_count = kbdev->pm.active_count;
	if (old_count == 0)
		kbase_timeline_pm_send_event(kbdev, KBASE_TIMELINE_PM_EVENT_GPU_IDLE);

	mutex_lock(&js_devdata->runpool_mutex);
	mutex_lock(&kbdev->pm.lock);

	c = --kbdev->pm.active_count;
	KBASE_TIMELINE_CONTEXT_ACTIVE(kbdev, c);
	KBASE_TRACE_ADD_REFCOUNT(kbdev, PM_CONTEXT_IDLE, NULL, NULL, 0u, c);

	KBASE_DEBUG_ASSERT(c >= 0);

	/* Trace the event being handled */
	if (old_count == 0)
		kbase_timeline_pm_handle_event(kbdev, KBASE_TIMELINE_PM_EVENT_GPU_IDLE);

	if (c == 0) {
		/* Last context has gone idle */
		kbase_hwaccess_pm_gpu_idle(kbdev);
#ifdef CONFIG_MALI_SEC_UTILIZATION
        now = ktime_get();
        spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
        kbase_pm_metrics_update(kbdev, &now);
        spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
        spin_lock_irqsave(&kbdev->pm.backend.metrics.lock, flags);
        kbdev->pm.backend.metrics.gpu_active = false;
        spin_unlock_irqrestore(&kbdev->pm.backend.metrics.lock, flags);
#endif
		/* Wake up anyone waiting for this to become 0 (e.g. suspend). The
		 * waiters must synchronize with us by locking the pm.lock after
		 * waiting */
		wake_up(&kbdev->pm.zero_active_count_wait);
	}

	mutex_unlock(&kbdev->pm.lock);
	mutex_unlock(&js_devdata->runpool_mutex);
}

KBASE_EXPORT_TEST_API(kbase_pm_context_idle);

void kbase_pm_suspend(struct kbase_device *kbdev)
{
	KBASE_DEBUG_ASSERT(kbdev);

	mutex_lock(&kbdev->pm.lock);
	KBASE_DEBUG_ASSERT(!kbase_pm_is_suspending(kbdev));
	kbdev->pm.suspending = true;
	mutex_unlock(&kbdev->pm.lock);

	/* From now on, the active count will drop towards zero. Sometimes, it'll
	 * go up briefly before going down again. However, once it reaches zero it
	 * will stay there - guaranteeing that we've idled all pm references */

	/* Suspend job scheduler and associated components, so that it releases all
	 * the PM active count references */
	kbasep_js_suspend(kbdev);

	/* Suspend any counter collection that might be happening */
#ifdef CONFIG_MALI_SEC_HWCNT
	mutex_lock(&kbdev->hwcnt.mlock);
	if (kbdev->vendor_callbacks->hwcnt_disable)
		kbdev->vendor_callbacks->hwcnt_disable(kbdev);
	mutex_unlock(&kbdev->hwcnt.mlock);
#else
	/* Suspend vinstr.
	 * This call will block until vinstr is suspended. */
	kbase_vinstr_suspend(kbdev->vinstr_ctx);
#endif

	/* Wait for the active count to reach zero. This is not the same as
	 * waiting for a power down, since not all policies power down when this
	 * reaches zero. */
	wait_event(kbdev->pm.zero_active_count_wait, kbdev->pm.active_count == 0);

	/* NOTE: We synchronize with anything that was just finishing a
	 * kbase_pm_context_idle() call by locking the pm.lock below */

	kbase_hwaccess_pm_suspend(kbdev);
}

void kbase_pm_resume(struct kbase_device *kbdev)
{
	/* MUST happen before any pm_context_active calls occur */
	kbase_hwaccess_pm_resume(kbdev);

	/* Initial active call, to power on the GPU/cores if needed */
	kbase_pm_context_active(kbdev);

	/* Re-enable instrumentation, if it was previously disabled */
#ifdef CONFIG_MALI_SEC_HWCNT
	mutex_lock(&kbdev->hwcnt.mlock);
	if (kbdev->vendor_callbacks->hwcnt_enable)
		kbdev->vendor_callbacks->hwcnt_enable(kbdev);
	mutex_unlock(&kbdev->hwcnt.mlock);
#else

	/* Resume vinstr operation */
	kbase_vinstr_resume(kbdev->vinstr_ctx);
#endif

	/* Resume any blocked atoms (which may cause contexts to be scheduled in
	 * and dependent atoms to run) */
	kbase_resume_suspended_soft_jobs(kbdev);

	/* Resume the Job Scheduler and associated components, and start running
	 * atoms */
	kbasep_js_resume(kbdev);

	/* Matching idle call, to power off the GPU/cores if we didn't actually
	 * need it and the policy doesn't want it on */
	kbase_pm_context_idle(kbdev);
}


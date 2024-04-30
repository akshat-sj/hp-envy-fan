// SPDX-License-Identifier: GPL-2.0-only

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/thermal.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include "fan_core.h"

#define HP_GUID "5FB7F034-2C63-45E9-BE91-3D44E2C707E4"
#define EC_PATH "\\_SB.PC00.LPCB.EC0."

static DEFINE_MUTEX(fan_lock);
static struct hp_fan_state fan;
static struct delayed_work safety_work;
static bool ready, pinned;
static int last_error;
static unsigned long long owner;
static unsigned int pulses;
static const char *reason = "none";

static int read_byte(const char *path, u8 *result)
{
	unsigned long long value;
	acpi_status status;

	status = acpi_evaluate_integer(NULL, (acpi_string)path, NULL, &value);
	if (ACPI_FAILURE(status) || value > 255)
		return -EIO;
	*result = value;
	return 0;
}

static int read_hardware(void *context, struct hp_hw_state *hw)
{
	if (read_byte(EC_PATH "ECOK", &hw->ready) ||
	    read_byte(EC_PATH "OMCC", &hw->omcc) ||
	    read_byte(EC_PATH "FFFS", &hw->max_flag) ||
	    read_byte(EC_PATH "HPCM", &hw->profile) ||
	    read_byte(EC_PATH "SRP1", &hw->cpu) ||
	    read_byte(EC_PATH "SRP2", &hw->gpu))
		return -EIO;
	return 0;
}

static int firmware_query(bool keepalive, u8 cpu, u8 gpu)
{
	struct {
		u32 signature, command, commandtype, datasize;
		u8 data[128];
	} args = { .signature = 0x55434553, .command = 0x20008,
		.commandtype = keepalive ? 0x10 : 0x2e,
		.datasize = keepalive ? 1 : 2 };
	struct acpi_buffer input = { sizeof(args), &args };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	int ret = -EIO;

	if (!keepalive && !hp_preset_valid(cpu, gpu) && !(cpu == 0xff && gpu == 0xff))
		return -EINVAL;
	args.data[0] = cpu;
	args.data[1] = gpu;
	status = wmi_evaluate_method(HP_GUID, 0, keepalive ? 2 : 1, &input, &output);
	obj = output.pointer;
	if (ACPI_SUCCESS(status) && obj && obj->type == ACPI_TYPE_BUFFER &&
	    obj->buffer.pointer && obj->buffer.length >= 8 &&
	    get_unaligned_le32(obj->buffer.pointer) == 0x53534150 &&
	    !get_unaligned_le32(obj->buffer.pointer + 4))
		ret = 0;
	kfree(output.pointer);
	return ret;
}

static int write_targets(void *context, u8 cpu, u8 gpu)
{
	return firmware_query(false, cpu, gpu);
}

static int firmware_keepalive(void *context)
{
	int ret = firmware_query(true, 0, 0);

	if (!ret)
		pulses++;
	return ret;
}

static const struct hp_io io = {
	.read = read_hardware,
	.write = write_targets,
	.keepalive = firmware_keepalive,
};

static int cpu_temperature(void)
{
	struct thermal_zone_device *zone;
	int temperature, ret;

	zone = thermal_zone_get_zone_by_name("x86_pkg_temp");
	if (IS_ERR(zone))
		return -1;
	ret = thermal_zone_get_temp(zone, &temperature);
	if (ret || temperature < 0 || temperature > 125000)
		return -1;
	return temperature;
}

static void finish_locked(void)
{
	lockdep_assert_held(&fan_lock);
	if (fan.active) {

		queue_delayed_work(system_wq, &safety_work, msecs_to_jiffies(1000));
	} else if (pinned) {
		pinned = false;
		module_put(THIS_MODULE);
	}
}

static int stop_locked(void)
{
	int ret;

	lockdep_assert_held(&fan_lock);
	ret = hp_restore(&fan, &io);
	last_error = ret;
	finish_locked();
	return ret;
}

static void safety_poll(struct work_struct *work)
{
	int ret, temperature;
	u64 now;

	mutex_lock(&fan_lock);
	if (ready && fan.active) {
		now = ktime_to_ms(ktime_get_boottime());
		temperature = cpu_temperature();
		if (temperature < 0)
			reason = "sensor";
		else if (temperature >= HP_TEMP_LIMIT_MC)
			reason = "temperature";
		else if (now >= fan.deadline)
			reason = "watchdog";
		ret = hp_tick(&fan, &io, now, temperature);
		if (ret) {
			last_error = ret;
			reason = "firmware";
		}
		finish_locked();
	}
	mutex_unlock(&fan_lock);
}

static int control_set(const char *value, const struct kernel_param *kp)
{
	int ret = 0, temperature;
	char command[16], extra;
	unsigned long long token;
	u64 now;
	bool emergency = sysfs_streq(value, "auto");

	if (!emergency && (sscanf(value, "%15s %llx %c", command, &token, &extra) != 2 ||
			   !token))
		return -EINVAL;

	mutex_lock(&fan_lock);
	if (!ready) {
		ret = -EAGAIN;
		goto out;
	}
	now = ktime_to_ms(ktime_get_boottime());
	if (emergency || !strcmp(command, "auto")) {
		if (!emergency && owner != token) {
			ret = -EPERM;
			goto out;
		}
		if (fan.active)
			reason = "manual";
		ret = stop_locked();
	} else if (!strcmp(command, "heartbeat")) {
		if (owner != token) {
			ret = -EPERM;
			goto out;
		}
		temperature = cpu_temperature();
		ret = hp_heartbeat(&fan, &io, now, temperature);
		if (ret) {
			last_error = ret;
			if (temperature < 0)
				reason = "sensor";
			else if (temperature >= HP_TEMP_LIMIT_MC)
				reason = "temperature";
			else if (ret != -ESHUTDOWN)
				reason = "firmware";
		}
		finish_locked();
	} else if (!strcmp(command, "cool") || !strcmp(command, "max")) {
		if (fan.active) {
			ret = -EBUSY;
			goto out;
		}
		temperature = cpu_temperature();
		if (!try_module_get(THIS_MODULE)) {
			ret = -ENODEV;
			goto out;
		}
		pinned = true;
		owner = token;
		pulses = 0;
		reason = "none";
		if (!strcmp(command, "cool"))
			ret = hp_start(&fan, &io, 48, 45, now, temperature);
		else
			ret = hp_start(&fan, &io, 60, 58, now, temperature);
		last_error = ret;
		if (ret)
			reason = "activation";
		finish_locked();
	} else {
		ret = -EINVAL;
	}
out:
	mutex_unlock(&fan_lock);
	return ret;
}

static int state_get(char *buffer, const struct kernel_param *kp)
{
	int count;

	mutex_lock(&fan_lock);
	count = scnprintf(buffer, PAGE_SIZE,
		"abi=2 active=%u stopping=%u cpu=%u gpu=%u error=%d restore_error=%d owner=%llx pulses=%u reason=%s\n",
		fan.active, fan.stopping, fan.cpu * 100, fan.gpu * 100,
		last_error, fan.restore_error, owner, pulses, reason);
	mutex_unlock(&fan_lock);
	return count;
}

static int reject_set(const char *value, const struct kernel_param *kp)
{
	return -EPERM;
}

static const struct kernel_param_ops control_ops = { .set = control_set };
static const struct kernel_param_ops state_ops = {
	.set = reject_set,
	.get = state_get,
};
module_param_cb(control, &control_ops, NULL, 0200);
MODULE_PARM_DESC(control, "Explicit cool/max/heartbeat/auto commands; root only");
module_param_cb(state, &state_ops, NULL, 0444);
MODULE_PARM_DESC(state, "Serialized controller state; ABI 2");

static int pm_event(struct notifier_block *nb, unsigned long event, void *data)
{
	int ret = 0;

	if (event != PM_SUSPEND_PREPARE && event != PM_HIBERNATION_PREPARE &&
	    event != PM_RESTORE_PREPARE)
		return NOTIFY_DONE;
	mutex_lock(&fan_lock);
	if (ready) {
		if (fan.active)
			reason = "suspend";
		ret = stop_locked();
	}
	mutex_unlock(&fan_lock);

	return notifier_from_errno(ret);
}

static int reboot_event(struct notifier_block *nb, unsigned long event, void *data)
{
	int ret = 0;

	mutex_lock(&fan_lock);
	if (ready) {
		if (fan.active)
			reason = "shutdown";
		ret = stop_locked();
	}
	mutex_unlock(&fan_lock);
	if (ret)
		pr_err("hp_envy_fan: unable to verify fan restoration before shutdown\n");
	return NOTIFY_DONE;
}

static struct notifier_block pm_notifier = { .notifier_call = pm_event };
static struct notifier_block reboot_notifier = { .notifier_call = reboot_event };

static int __init hp_envy_fan_init(void)
{
	int ret;

	if (!dmi_match(DMI_SYS_VENDOR, "HP") ||
	    !dmi_match(DMI_PRODUCT_NAME, "HP ENVY Laptop 14-eb0xxx") ||
	    !dmi_match(DMI_BOARD_NAME, "8815") ||
	    !dmi_match(DMI_BIOS_VERSION, "F.46") || !wmi_has_guid(HP_GUID))
		return -ENODEV;
	INIT_DELAYED_WORK(&safety_work, safety_poll);
	ret = register_pm_notifier(&pm_notifier);
	if (ret)
		return ret;
	ret = register_reboot_notifier(&reboot_notifier);
	if (ret) {
		unregister_pm_notifier(&pm_notifier);
		return ret;
	}
	mutex_lock(&fan_lock);
	ready = true;
	mutex_unlock(&fan_lock);
	pr_info("hp_envy_fan: loaded inactive; explicit activation required\n");
	return 0;
}

static void __exit hp_envy_fan_exit(void)
{

	mutex_lock(&fan_lock);
	ready = false;
	mutex_unlock(&fan_lock);
	unregister_pm_notifier(&pm_notifier);
	unregister_reboot_notifier(&reboot_notifier);
	disable_delayed_work_sync(&safety_work);
	pr_info("hp_envy_fan: unloaded with no active override\n");
}
module_init(hp_envy_fan_init);
module_exit(hp_envy_fan_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experimental HP 8815 F.46 fan presets with verified restoration");
MODULE_VERSION("0.1.0");

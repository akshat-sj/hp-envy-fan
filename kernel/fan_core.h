/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HP_ENVY_FAN_CORE_H
#define HP_ENVY_FAN_CORE_H

#include <linux/errno.h>
#include <linux/types.h>


#define HP_WATCHDOG_MS 10000U
#define HP_TEMP_LIMIT_MC 80000
#define HP_KEEPALIVE_MS 30000U

struct hp_hw_state {
	u8 ready, omcc, max_flag, profile, cpu, gpu;
};

struct hp_io {
	int (*read)(void *context, struct hp_hw_state *hw);
	int (*write)(void *context, u8 cpu, u8 gpu);
	int (*keepalive)(void *context);
	void *context;
};

struct hp_fan_state {
	bool active, stopping;
	u8 saved_cpu, saved_gpu, cpu, gpu;
	u64 deadline, next_keepalive;
	int restore_error;
};

static inline bool hp_preset_valid(u8 cpu, u8 gpu)
{
	return (cpu == 48 && gpu == 45) || (cpu == 60 && gpu == 58);
}

static inline int hp_restore(struct hp_fan_state *s, const struct hp_io *io)
{
	struct hp_hw_state hw;
	int ret, read_ret;

	if (!s->active)
		return 0;
	s->stopping = true;
	ret = io->write(io->context, s->saved_cpu, s->saved_gpu);
	read_ret = io->read(io->context, &hw);
	if (read_ret)
		ret = read_ret;
	else if (hw.cpu != s->saved_cpu || hw.gpu != s->saved_gpu || hw.omcc)
		ret = -EIO;
	else
		ret = 0;
	s->restore_error = ret;
	if (!ret)
		s->active = false;
	return ret;
}

static inline int hp_start(struct hp_fan_state *s, const struct hp_io *io,
			   u8 cpu, u8 gpu, u64 now, int temperature)
{
	struct hp_hw_state hw;
	int ret;

	if (!hp_preset_valid(cpu, gpu))
		return -EINVAL;
	if (s->active)
		return -EBUSY;
	if (temperature < 0 || temperature >= HP_TEMP_LIMIT_MC)
		return -ERANGE;
	ret = io->read(io->context, &hw);
	if (ret)
		return ret;
	if (hw.ready != 1 || hw.omcc || hw.max_flag || hw.profile != 0x30 ||
	    hw.cpu != 0xff || hw.gpu != 0xff)
		return -EBUSY;
	s->saved_cpu = hw.cpu;
	s->saved_gpu = hw.gpu;
	s->cpu = cpu;
	s->gpu = gpu;
	s->deadline = now + HP_WATCHDOG_MS;
	s->next_keepalive = now + HP_KEEPALIVE_MS;
	s->restore_error = 0;
	s->stopping = false;
	s->active = true;
	ret = io->keepalive(io->context);
	if (!ret)
		ret = io->write(io->context, cpu, gpu);
	if (!ret)
		ret = io->read(io->context, &hw);
	if (!ret && (hw.cpu != cpu || hw.gpu != gpu || hw.max_flag ||
		     hw.profile != 0x30 || hw.ready != 1))
		ret = -EIO;
	if (ret)
		hp_restore(s, io);
	return ret;
}

static inline int hp_tick(struct hp_fan_state *s, const struct hp_io *io,
			  u64 now, int temperature)
{
	int ret;

	if (!s->active)
		return 0;
	if (s->stopping || now >= s->deadline || temperature < 0 ||
	    temperature >= HP_TEMP_LIMIT_MC)
		return hp_restore(s, io);
	if (now >= s->next_keepalive) {
		ret = io->keepalive(io->context);
		if (ret) {
			hp_restore(s, io);
			return ret;
		}
		s->next_keepalive = now + HP_KEEPALIVE_MS;
	}
	return 0;
}

static inline int hp_heartbeat(struct hp_fan_state *s, const struct hp_io *io,
			       u64 now, int temperature)
{
	struct hp_hw_state hw;
	int ret;

	if (!s->active || s->stopping)
		return -ESHUTDOWN;
	if (now >= s->deadline || temperature < 0 || temperature >= HP_TEMP_LIMIT_MC) {
		hp_restore(s, io);
		return -ETIME;
	}
	ret = io->read(io->context, &hw);
	if (ret || hw.ready != 1 || hw.cpu != s->cpu || hw.gpu != s->gpu ||
	    hw.max_flag || hw.profile != 0x30) {
		hp_restore(s, io);
		return ret ? ret : -EIO;
	}
	s->deadline = now + HP_WATCHDOG_MS;
	return 0;
}

#endif

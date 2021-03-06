/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "lights"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include <cutils/log.h>
#include <hardware/lights.h>
#define LED_BRIGHTNESS_OFF 0
#define LED_BRIGHTNESS_MAX 254

#define ALPHA_MASK 0xff000000
#define COLOR_MASK 0x00ffffff

#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC 1000000000ULL

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

char const *const LCD_FILE = "/sys/devices/platform/omap_pwm_led.0/leds/lcd-backlight/brightness";
const char *const LED_DIR = "/sys/devices/platform/leds_pwm/leds/power";


const char *const LED_BATTERY_FILE = "/sys/devices/platform/leds_pwm/leds/power/brightness";
const char *const LED_BRIGHTNESS_FILE = "brightness";
const char *const LED_DELAY_ON_FILE = "delay_on";
const char *const LED_DELAY_OFF_FILE = "delay_off";
const char *const LED_TRIGGER_FILE = "trigger";

enum LED_STATE {
	OFF,
	ON,
	BLINK,
};

struct power_led_info {
	unsigned int color;
	unsigned int delay_on;
	unsigned int delay_off;
	enum LED_STATE state;
};

static int write_int(char const *path, int value)
{
	int fd;
	static int already_warned;

	ALOGV("write_int: path %s, value %d", path, value);
	fd = open(path, O_RDWR);

	if (fd >= 0) {
		char buffer[20];
		int bytes = sprintf(buffer, "%d\n", value);
		int amt = write(fd, buffer, bytes);
		close(fd);
		return amt == -1 ? -errno : 0;
	} else {
		if (already_warned == 0) {
			ALOGE("write_int failed to open %s\n", path);
			already_warned = 1;
		}
		return -errno;
	}
}
static int
rgb_to_brightness(struct light_state_t const* state)
{
    int color = state->color & 0x00ffffff;
    return ((77*((color>>16)&0x00ff))
            + (150*((color>>8)&0x00ff)) + (29*(color&0x00ff))) >> 8;
}
static int set_light_backlight(struct light_device_t *dev,
			struct light_state_t const *state)
{
	int err = 0;
	
	int brightness = rgb_to_brightness(state);
	////ALOGD("set_light_backlight brightness=%d",brightness);
	pthread_mutex_lock(&g_lock);
	err = write_int(LCD_FILE, brightness);

	pthread_mutex_unlock(&g_lock);
	return err;
}

static int close_lights(struct hw_device_t *dev)
{
	ALOGV("close_light is called");
	free(dev);

	return 0;
}



static void time_add(struct timespec *time, int sec, int nsec)
{
	time->tv_nsec += nsec;
	time->tv_sec += time->tv_nsec / NSEC_PER_SEC;
	time->tv_nsec %= NSEC_PER_SEC;
	time->tv_sec += sec;
}

static bool time_after(struct timespec *t)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec > t->tv_sec || (now.tv_sec == t->tv_sec && now.tv_nsec > t->tv_nsec);
}

static int led_sysfs_write(char *buf, const char *command, char *format, ...)
{
	int fd;
	char path_name[PATH_MAX];
	int err;
	int len;
	va_list args;
	struct timespec timeout;
	int ret;

	err = sprintf(path_name, "%s/%s", LED_DIR, command);
	if (err < 0)
		return err;

	clock_gettime(CLOCK_MONOTONIC, &timeout);
	time_add(&timeout, 0, 100 * NSEC_PER_MSEC);

	do {
		fd = open(path_name, O_WRONLY);
		err = -errno;
		if (fd < 0) {
			if (errno != EINTR && errno != EACCES && time_after(&timeout)) {
				ALOGE("failed to open sysfs %s!", path_name);
				return err;
			}
			sched_yield();
		}
	} while (fd < 0);

	va_start(args, format);
	len = vsprintf(buf, format, args);
	va_end(args);
	if (len < 0)
		return len;

	err = write(fd, buf, len);
	if (err == -1)
		return -errno;

	err = close(fd);
	if (err == -1)
		return -errno;

	return 0;
}

static int write_leds(struct power_led_info *leds)
{
	char buf[20];
	int err = -1;

	pthread_mutex_lock(&g_lock);
 
 
	switch(leds->state) {
	case OFF:
		err = led_sysfs_write(buf, LED_TRIGGER_FILE, "%s",
			"default-off");
		break;
	case BLINK:
		err = led_sysfs_write(buf, LED_TRIGGER_FILE, "%s", "timer");
		if (err)
			goto err_write_fail;
		err = led_sysfs_write(buf, LED_DELAY_ON_FILE, "%u", leds->delay_on);
		if (err)
			goto err_write_fail;
		err = led_sysfs_write(buf, LED_DELAY_OFF_FILE, "%u", leds->delay_off);
		if (err)
			goto err_write_fail;
	case ON:
		err = led_sysfs_write(buf, LED_TRIGGER_FILE, "%s", "default-on");
		if (err)
			goto err_write_fail;
		err = led_sysfs_write(buf, LED_BRIGHTNESS_FILE, "%d",
			LED_BRIGHTNESS_MAX);
		if (err)
			goto err_write_fail;
	default:
		break;
	}

err_write_fail:
	pthread_mutex_unlock(&g_lock);

	return err;
}
static int set_light_leds_battery(struct light_device_t* dev,
        struct light_state_t const* state)
{
	int err = 0;
	
	int brightness = rgb_to_brightness(state);
	////ALOGD("set_light_leds_battery brightness=%d",brightness);
	pthread_mutex_lock(&g_lock);
	err = write_int(LED_BATTERY_FILE, brightness);

	pthread_mutex_unlock(&g_lock);
	return err;
}

static int set_light_leds(struct light_state_t const *state, int type)
{
	struct power_led_info leds;

	//ALOGD("set_light_leds type=%d",type);
	memset(&leds, 0, sizeof(leds));

	switch (state->flashMode) {
	case LIGHT_FLASH_NONE:{
		////ALOGD("set_light_leds LIGHT_FLASH_NONE");
		leds.state = OFF;
		break; }
	case LIGHT_FLASH_TIMED:
		////ALOGD("set_light_leds LIGHT_FLASH_TIMED");
	case LIGHT_FLASH_HARDWARE:{
		////ALOGD("set_light_leds LIGHT_FLASH_HARDWARE");
		if (state->flashOnMS < 0 || state->flashOffMS < 0){
			ALOGE("error in set_light_leds state->flashOnMS=%d state->flashOffMS=%d",state->flashOnMS,state->flashOffMS);
			return -EINVAL;
		}
		////ALOGD("set_light_leds state->flashOnMS=%d state->flashOffMS=%d",state->flashOnMS,state->flashOffMS);
		leds.delay_off = state->flashOffMS;
		leds.delay_on = state->flashOnMS;
		
    

		if (leds.delay_on == 0){
			leds.state = OFF;
			////ALOGD("set_light_leds leds.state=OFF");
		}else if (leds.delay_off){
			////ALOGD("set_light_leds leds.state=BLINK");
			leds.state = BLINK;
		}else{
			////ALOGD("set_light_leds leds.state=ON");
			leds.state = ON;
		}
		break;
    }
	default:{
		ALOGW("error : set_light_leds mode unknown state->flashMode=%d ",state->flashMode);
		return -EINVAL;
	}
	}

	return write_leds(&leds);
}

static int set_light_leds_notifications(struct light_device_t *dev,
			struct light_state_t const *state)
{
	int err = 0;
	int brightness = rgb_to_brightness(state);

	pthread_mutex_lock(&g_lock);
	err = write_int(LED_DIR, brightness);

	pthread_mutex_unlock(&g_lock);
	return err;
	
	return set_light_leds(state, 0);
}

static int set_light_leds_attention(struct light_device_t *dev,
			struct light_state_t const *state)
{
	return set_light_leds(state, 3);
}

static int open_lights(const struct hw_module_t *module, char const *name,
						struct hw_device_t **device)
{
	
	//ALOGD("open_lights name=%s",name);
	int (*set_light)(struct light_device_t *dev,
		struct light_state_t const *state);

	if (strcmp(LIGHT_ID_BACKLIGHT, name) == 0)
		set_light = set_light_backlight;
	else if (strcmp(LIGHT_ID_NOTIFICATIONS, name) == 0)
		set_light = set_light_leds_notifications;
	else if (strcmp(LIGHT_ID_BATTERY, name) == 0)
		set_light = set_light_leds_battery;
	else if (strcmp(LIGHT_ID_ATTENTION, name) == 0)
		set_light = set_light_leds_attention;
	else
		return -EINVAL;

	struct light_device_t *dev = malloc(sizeof(struct light_device_t));
	if (!dev){
		ALOGE("open_lights failed name=%s",name);
		return -ENOMEM;
	}
	memset(dev, 0, sizeof(*dev));

	dev->common.tag = HARDWARE_DEVICE_TAG;
	dev->common.version = 0;
	dev->common.module = (struct hw_module_t *)module;
	dev->common.close = close_lights;
	dev->set_light = set_light;

	*device = (struct hw_device_t *)dev;

	return 0;
}

static struct hw_module_methods_t lights_module_methods = {
	.open = open_lights,
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "Archos A80S lights Module",
    .author = "Google, Inc.",
    .methods = &lights_module_methods,
};

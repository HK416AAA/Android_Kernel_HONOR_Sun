/*
 * hnlog_kernel.h
 *
 * hnlog expansion interfaces, supporting jank and dubai
 *
 * Copyright (c) 2018-2019 Honor Device Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#ifndef _LINUX_HNLOG_KERNEL_H
#define _LINUX_HNLOG_KERNEL_H

#include <log/janklogconstants.h>

#define HN_LOG_PRIO_VERBOSE 2
#define HN_LOG_PRIO_DEBUG 3
#define HN_LOG_PRIO_INFO 4
#define HN_LOG_PRIO_WARN 5
#define HN_LOG_PRIO_ERROR 6

enum hnlog_id {
	HN_LOG_ID_MIN = 0,
	HN_LOG_ID_EXCEPTION = HN_LOG_ID_MIN,
	HN_LOG_ID_JANK = 1,
	HN_LOG_ID_DUBAI = 2,
	HN_LOG_ID_MAX
};

#define MAX_MSG_SIZE 256

#ifdef MODULE
int hnlog_wq_init(void);
void hnlog_wq_destroy(void);
int hievent_to_write(int prio, int bufid, const char *tag, const char *fmt,
		     ...);
int hievent_to_jank_impl(int tag, int prio, const char *fmt, ...);
int hievent_to_jank_vh_ogki_impl(int tag, int prio, const char *buf);

/*
 * For forward compatibility, HN_LOG_PRIO_DEBUG level is just for HN service.
 * And the interface name stay the same "pr_HN".
 * Use LOG_HN_W / LOG_HN_V / LOG_HN_I / LOG_HN_E for other purpose.
 */

#define hievent_to_jank(tag, prio, fmt, ...)                               \
	({                                                                 \
		int ret = 0;                                               \
		ret = hievent_to_jank_impl(tag, prio, fmt, ##__VA_ARGS__); \
		ret;                                                       \
	})

#ifndef pr_jank
#define pr_jank(tag, fmt, ...) \
	hievent_to_jank_impl(tag, HN_LOG_PRIO_DEBUG, fmt, ##__VA_ARGS__)
#endif

#ifndef LOG_JANK_D
#define LOG_JANK_D(tag, fmt, ...) \
	hievent_to_jank_impl(tag, HN_LOG_PRIO_DEBUG, fmt, ##__VA_ARGS__)
#endif

#ifndef LOG_JANK_W
#define LOG_JANK_W(tag, fmt, ...) \
	hievent_to_jank_impl(tag, HN_LOG_PRIO_WARN, fmt, ##__VA_ARGS__)
#endif

#ifndef LOG_JANK_V
#define LOG_JANK_V(tag, fmt, ...) \
	hievent_to_jank_impl(tag, HN_LOG_PRIO_VERBOSE, fmt, ##__VA_ARGS__)
#endif

#ifndef LOG_JANK_I
#define LOG_JANK_I(tag, fmt, ...) \
	hievent_to_jank_impl(tag, HN_LOG_PRIO_INFO, fmt, ##__VA_ARGS__)
#endif

#ifndef LOG_JANK_E
#define LOG_JANK_E(tag, fmt, ...) \
	hievent_to_jank_impl(tag, HN_LOG_PRIO_ERROR, fmt, ##__VA_ARGS__)
#endif

#ifndef HNDUBAI_pr
#define HNDUBAI_pr(tag, fmt, ...)                                      \
	hievent_to_write(HN_LOG_PRIO_DEBUG, HN_LOG_ID_DUBAI, tag, fmt, \
			 ##__VA_ARGS__)
#endif

#ifndef HNDUBAI_LOGV
#define HNDUBAI_LOGV(tag, fmt, ...)                                      \
	hievent_to_write(HN_LOG_PRIO_VERBOSE, HN_LOG_ID_DUBAI, tag, fmt, \
			 ##__VA_ARGS__)
#endif

#ifndef HNDUBAI_LOGD
#define HNDUBAI_LOGD(tag, fmt, ...)                                    \
	hievent_to_write(HN_LOG_PRIO_DEBUG, HN_LOG_ID_DUBAI, tag, fmt, \
			 ##__VA_ARGS__)
#endif

#ifndef HNDUBAI_LOGI
#define HNDUBAI_LOGI(tag, fmt, ...)                                   \
	hievent_to_write(HN_LOG_PRIO_INFO, HN_LOG_ID_DUBAI, tag, fmt, \
			 ##__VA_ARGS__)
#endif

#ifndef HNDUBAI_LOGW
#define HNDUBAI_LOGW(tag, fmt, ...)                                   \
	hievent_to_write(HN_LOG_PRIO_WARN, HN_LOG_ID_DUBAI, tag, fmt, \
			 ##__VA_ARGS__)
#endif

#ifndef HNDUBAI_LOGE
#define HNDUBAI_LOGE(tag, fmt, ...)                                    \
	hievent_to_write(HN_LOG_PRIO_ERROR, HN_LOG_ID_DUBAI, tag, fmt, \
			 ##__VA_ARGS__)
#endif

#else

#ifdef CREATE_TRACE_POINTS
#undef CREATE_TRACE_POINTS
#endif
#include <trace/hooks/ogki_honor.h>

static inline void msg_impl(char *msg, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vscnprintf(msg, MAX_MSG_SIZE, fmt, args);
	va_end(args);
}

#define hievent_to_jank(tag, prio, fmt, ...)                                 \
	({                                                                   \
		int ret = 0;                                                 \
		char msg[MAX_MSG_SIZE];                                      \
		memset(msg, 0, MAX_MSG_SIZE);                                \
		msg_impl(msg, fmt, ##__VA_ARGS__);                           \
		trace_android_vh_ogki_hievent_to_jank(tag, prio, msg, &ret); \
		ret;                                                         \
	})

#ifndef pr_jank
#define pr_jank(tag, fmt, ...) \
	hievent_to_jank(tag, HN_LOG_PRIO_DEBUG, fmt, ##__VA_ARGS__)
#endif

#ifndef LOG_JANK_D
#define LOG_JANK_D(tag, fmt, ...) \
	hievent_to_jank(tag, HN_LOG_PRIO_DEBUG, fmt, ##__VA_ARGS__)
#endif

#ifndef LOG_JANK_W
#define LOG_JANK_W(tag, fmt, ...) \
	hievent_to_jank(tag, HN_LOG_PRIO_WARN, fmt, ##__VA_ARGS__)
#endif

#ifndef LOG_JANK_V
#define LOG_JANK_V(tag, fmt, ...) \
	hievent_to_jank(tag, HN_LOG_PRIO_VERBOSE, fmt, ##__VA_ARGS__)
#endif

#ifndef LOG_JANK_I
#define LOG_JANK_I(tag, fmt, ...) \
	hievent_to_jank(tag, HN_LOG_PRIO_INFO, fmt, ##__VA_ARGS__)
#endif

#ifndef LOG_JANK_E
#define LOG_JANK_E(tag, fmt, ...) \
	hievent_to_jank(tag, HN_LOG_PRIO_ERROR, fmt, ##__VA_ARGS__)
#endif

#ifndef HNDUBAI_pr
#define HNDUBAI_pr(tag, fmt, ...)
#endif

#ifndef HNDUBAI_LOGV
#define HNDUBAI_LOGV(tag, fmt, ...)
#endif

#ifndef HNDUBAI_LOGD
#define HNDUBAI_LOGD(tag, fmt, ...)
#endif

#ifndef HNDUBAI_LOGI
#define HNDUBAI_LOGI(tag, fmt, ...)
#endif

#ifndef HNDUBAI_LOGW
#define HNDUBAI_LOGW(tag, fmt, ...)
#endif

#ifndef HNDUBAI_LOGE
#define HNDUBAI_LOGE(tag, fmt, ...)
#endif
#endif

#endif /* _LINUX_HNLOG_KERNEL_H */

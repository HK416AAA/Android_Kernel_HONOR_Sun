/*
 * Copyright (c) Honor Device Co., Ltd. 2019-2020. All rights reserved.
 * Description: Define for hievent interface for kernel
 * Author: xiaocong
 * Create: 2019-10-17
 */

#ifndef HIVIEW_HIEVENT_H
#define HIVIEW_HIEVENT_H

#ifdef __cplusplus
extern "C" {
#endif

struct hiview_hievent;

#ifdef MODULE

struct hiview_hievent *hiview_hievent_create_impl(unsigned int event_id);
int hiview_hievent_report_impl(struct hiview_hievent *event);
void hiview_hievent_destroy_impl(struct hiview_hievent *event);
int hiview_hievent_put_integral_impl(struct hiview_hievent *event,
				     const char *key, long long value);
int hiview_hievent_put_string_impl(struct hiview_hievent *event,
				   const char *key, const char *value);
int hiview_hievent_set_time(struct hiview_hievent *event, long long seconds);
int hiview_hievent_add_file_path(struct hiview_hievent *event,
				 const char *path);
int hiview_hievent_set_tag(struct hiview_hievent *event, const char *value);
int hiview_hievent_set_tag_array(struct hiview_hievent *event,
				 const char *value_array[], int tag_num);

#define hiview_hievent_create(event_id)                       \
	({                                                    \
		struct hiview_hievent *event = NULL;          \
		event = hiview_hievent_create_impl(event_id); \
		event;                                        \
	})

#define hiview_hievent_report(event)                     \
	({                                               \
		unsigned int ret = 0;                    \
		ret = hiview_hievent_report_impl(event); \
		ret;                                     \
	})

#define hiview_hievent_destroy(event) ({ hiview_hievent_destroy_impl(event); })

#define hiview_hievent_put_integral(event, key, value)                     \
	({                                                                 \
		unsigned int ret = 0;                                      \
		ret = hiview_hievent_put_integral_impl(event, key, value); \
		ret;                                                       \
	})

#define hiview_hievent_put_string(event, key, value)                     \
	({                                                               \
		unsigned int ret = 0;                                    \
		ret = hiview_hievent_put_string_impl(event, key, value); \
		ret;                                                     \
	})

#else

#ifdef CREATE_TRACE_POINTS
#undef CREATE_TRACE_POINTS
#endif
#include <trace/hooks/ogki_honor.h>

#define hiview_hievent_create(event_id)                                     \
	({                                                                  \
		struct hiview_hievent *event = NULL;                        \
		void *event_vh = NULL;                                      \
		trace_android_rvh_ogki_hievent_create(event_id, &event_vh); \
		event = (struct hiview_hievent *)event_vh;                  \
		event;                                                      \
	})

#define hiview_hievent_report(event)                                   \
	({                                                             \
		unsigned int ret = 0;                                  \
		void *event_vh = (void *)event;                        \
		trace_android_rvh_ogki_hievent_report(event_vh, &ret); \
		ret;                                                   \
	})

#define hiview_hievent_destroy(event)                             \
	({                                                        \
		void *event_vh = (void *)event;                   \
		trace_android_rvh_ogki_hievent_destroy(event_vh); \
	})

#define hiview_hievent_put_integral(event, key, value)                     \
	({                                                                 \
		int ret = 0;                                               \
		void *event_vh = (void *)event;                            \
		trace_android_rvh_ogki_hievent_put_integral(event_vh, key, \
							    value, &ret);  \
		ret;                                                       \
	})

#define hiview_hievent_put_string(event, key, value)                     \
	({                                                               \
		int ret = 0;                                             \
		void *event_vh = (void *)event;                          \
		trace_android_rvh_ogki_hievent_put_string(event_vh, key, \
							  value, &ret);  \
		ret;                                                     \
	})

#endif

#ifdef __cplusplus
}
#endif

#endif /* HIVIEW_HIEVENT_H */

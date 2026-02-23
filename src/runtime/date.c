/*
 * date.c - Date prototype for Clay runtime
 */
#include "clay.h"
#include <time.h>
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#pragma warning(disable:4996)
#endif

/* Helper: extract struct tm from a Date object's _timestamp slot */
static struct tm date_to_tm(ClayObject *self) {
    ClayValue ts_val = clay_get_slot(self, "_timestamp");
    time_t ts = (time_t)clay_to_int(ts_val);
    struct tm *tm = localtime(&ts);
    return *tm;
}

/* Date now() - create a Date with current time */
static ClayValue date_now(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *d = clay_clone(self);
    clay_set_slot(d, "_timestamp", clay_int((intptr_t)time(NULL)));
    return CLAY_VAL(d);
}

/* Date fromTimestamp(ts) - create a Date from epoch seconds */
static ClayValue date_fromTimestamp(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc;
    ClayObject *d = clay_clone(self);
    intptr_t ts = clay_to_int(argv[0]);
    clay_set_slot(d, "_timestamp", clay_int(ts));
    return CLAY_VAL(d);
}

/* Date year() */
static ClayValue date_year(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    struct tm t = date_to_tm(self);
    return clay_int(t.tm_year + 1900);
}

/* Date month() - 1-12 */
static ClayValue date_month(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    struct tm t = date_to_tm(self);
    return clay_int(t.tm_mon + 1);
}

/* Date day() - 1-31 */
static ClayValue date_day(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    struct tm t = date_to_tm(self);
    return clay_int(t.tm_mday);
}

/* Date hour() - 0-23 */
static ClayValue date_hour(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    struct tm t = date_to_tm(self);
    return clay_int(t.tm_hour);
}

/* Date minute() - 0-59 */
static ClayValue date_minute(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    struct tm t = date_to_tm(self);
    return clay_int(t.tm_min);
}

/* Date second() - 0-59 */
static ClayValue date_second(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    struct tm t = date_to_tm(self);
    return clay_int(t.tm_sec);
}

/* Date weekday() - 0=Sun, 1=Mon, ..., 6=Sat */
static ClayValue date_weekday(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    struct tm t = date_to_tm(self);
    return clay_int(t.tm_wday);
}

/* Date timestamp() - epoch seconds */
static ClayValue date_timestamp(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_get_slot(self, "_timestamp");
}

/* Date toString() - "YYYY-MM-DD HH:MM:SS" */
static ClayValue date_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    struct tm t = date_to_tm(self);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    return CLAY_VAL(clay_string_new(buf));
}

/* Date format(pattern) - strftime-style formatting */
static ClayValue date_format(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc;
    const char *pattern = clay_string_data(CLAY_OBJ(argv[0]));
    struct tm t = date_to_tm(self);
    char buf[256];
    strftime(buf, sizeof(buf), pattern, &t);
    return CLAY_VAL(clay_string_new(buf));
}

/* Register Date methods */
void clay_date_init_methods(ClayObject *date) {
    clay_set_method(date, "now", date_now);
    clay_set_method(date, "fromTimestamp", date_fromTimestamp);
    clay_set_method(date, "year", date_year);
    clay_set_method(date, "month", date_month);
    clay_set_method(date, "day", date_day);
    clay_set_method(date, "hour", date_hour);
    clay_set_method(date, "minute", date_minute);
    clay_set_method(date, "second", date_second);
    clay_set_method(date, "weekday", date_weekday);
    clay_set_method(date, "timestamp", date_timestamp);
    clay_set_method(date, "toString", date_toString);
    clay_set_method(date, "format", date_format);
    clay_set_slot(date, "_timestamp", clay_int(0));
}

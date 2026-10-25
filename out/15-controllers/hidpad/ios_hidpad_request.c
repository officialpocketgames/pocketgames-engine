// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Parts B4/B5: the HID pad's answers to WriteFile and IOCTLs, as hidclass.sys gives
// them to its device files (behaviour of refs/wine-11.4/dlls/hidclass.sys/device.c and
// Microsoft's IOCTL_HID_* documentation; the code is our own). See ios_hidpad_request.h.

#include <string.h>

#include "ios_hidpad_request.h"

/* hidclass's lookup (find_report_with_type_and_id, collection 1, exact id). */
const struct ios_hidparse_report *ios_hidparse_find_report(const struct ios_hidparse_desc *desc,
                                                           enum ios_hidparse_report_type type,
                                                           uint8_t id)
{
    uint32_t i;

    for (i = 0; i < desc->report_count; i++)
    {
        const struct ios_hidparse_report *report = &desc->reports[i];
        if (report->collection != 1) continue;
        if (report->id && report->id != id) continue;
        if (type == IOS_HIDPARSE_INPUT && report->input_len) return report;
        if (type == IOS_HIDPARSE_OUTPUT && report->output_len) return report;
        if (type == IOS_HIDPARSE_FEATURE && report->feature_len) return report;
    }
    return NULL;
}

void ios_hidpad_handle_init(struct ios_hidpad_handle *handle)
{
    memset(handle, 0, sizeof(*handle));
    handle->input_buffers = IOS_HIDPAD_DEFAULT_INPUT_BUFFERS;
}

int ios_hidpad_presence(struct ios_hidpad_device *dev, int connected, uint64_t now_us)
{
    if (connected)
    {
        dev->published = 1;
        dev->was_published = 1;
        dev->unpublished_since_us = 0;
        return 1;
    }
    if (dev->published)
    {
        dev->published = 0;
        dev->unpublished_since_us = now_us;
    }
    if (!dev->was_published) return 0;
    /* a sample older than the first unpublished one (callers' clocks race) is within the grace */
    return now_us < dev->unpublished_since_us ||
           now_us - dev->unpublished_since_us < IOS_HIDPAD_UNPUBLISH_GRACE_US;
}

int ios_hidpad_handle_lost(struct ios_hidpad_handle *handle, int present)
{
    if (!present) handle->lost = 1;
    return handle->lost;
}

enum ios_hidpad_read_outcome ios_hidpad_read_decide(struct ios_hidpad_device *dev,
                                                    struct ios_hidpad_handle *handle,
                                                    const struct pgios_hidpad *pad,
                                                    int queued, uint64_t now_us)
{
    if (ios_hidpad_handle_lost(handle, ios_hidpad_presence(dev, pad->connected, now_us)))
        return IOS_HIDPAD_READ_LOST;
    if (queued) return IOS_HIDPAD_READ_WAIT;
    if (ios_hidpad_read_ready(now_us, handle->have_last, handle->last_report_us,
                              handle->last_packet, pad->packet))
        return IOS_HIDPAD_READ_NOW;
    return IOS_HIDPAD_READ_WAIT;
}

uint32_t ios_hidpad_read_report(struct ios_hidpad_device *dev, struct ios_hidpad_handle *handle,
                                const struct pgios_hidpad *pad, uint64_t now_us, uint8_t *out)
{
    handle->have_last = 1;
    handle->last_report_us = now_us;
    handle->last_packet = pad->packet;
    return (uint32_t)ios_hidpad_build_input(dev->id, pad, dev->report_state, now_us, out);
}

uint32_t ios_hidpad_input_size(uint32_t code, uint32_t data_size, uint32_t out_size)
{
    if ((code & 3) == IOS_METHOD_BUFFERED) return data_size;
    return data_size > out_size ? data_size - out_size : 0;
}

static void reply_init(struct ios_hidpad_reply *reply)
{
    memset(reply, 0, sizeof(*reply));
    reply->status = IOS_STATUS_SUCCESS;
}

static void reply_error(struct ios_hidpad_reply *reply, uint32_t status)
{
    reply->status = status;
    reply->information = 0;
    reply->data = NULL;
    reply->size = 0;
}

static void reply_bytes(struct ios_hidpad_reply *reply, const void *data, uint32_t size)
{
    if (size > sizeof(reply->buffer)) size = sizeof(reply->buffer);
    if (data != reply->buffer) memcpy(reply->buffer, data, size);
    reply->data = reply->buffer;
    reply->size = size;
    reply->information = size;
}

static void reply_ulong(struct ios_hidpad_reply *reply, uint32_t value)
{
    uint8_t le[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    reply_bytes(reply, le, sizeof(le));
}

static uint32_t read_ulong(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A string as UTF-16 with its terminator; hidclass's BUFFER_TOO_SMALL when it doesn't fit. */
static void reply_string(struct ios_hidpad_reply *reply, const char *str, uint32_t out_size)
{
    uint32_t i, n = (uint32_t)strlen(str), bytes = (n + 1) * 2;

    if (bytes > out_size || bytes > sizeof(reply->buffer))
    {
        reply_error(reply, IOS_STATUS_BUFFER_TOO_SMALL);
        return;
    }
    for (i = 0; i <= n; i++)
    {
        reply->buffer[2 * i] = (uint8_t)(i < n ? str[i] : 0);
        reply->buffer[2 * i + 1] = 0;
    }
    reply_bytes(reply, reply->buffer, bytes);
}

/* hidclass's checks for a report going to or coming from the device: a buffer, a report of
 * that type with the id in byte 0, and at least the report's length. */
static const struct ios_hidparse_report *check_report(const struct ios_hidpad_device *dev,
                                                      enum ios_hidparse_report_type type,
                                                      const uint8_t *data, uint32_t len,
                                                      struct ios_hidpad_reply *reply)
{
    const struct ios_hidparse_report *report;
    uint32_t report_len;

    if (!data || !len)
    {
        reply_error(reply, IOS_STATUS_INVALID_USER_BUFFER);
        return NULL;
    }
    if (!(report = ios_hidparse_find_report(dev->desc, type, data[0])))
    {
        reply_error(reply, IOS_STATUS_INVALID_PARAMETER);
        return NULL;
    }
    switch (type)
    {
    case IOS_HIDPARSE_INPUT: report_len = report->input_len; break;
    case IOS_HIDPARSE_OUTPUT: report_len = report->output_len; break;
    default: report_len = report->feature_len; break;
    }
    if (len < report_len)
    {
        reply_error(reply, IOS_STATUS_INVALID_PARAMETER);
        return NULL;
    }
    return report;
}

/* Fold an output report (byte 0 its id, `len` the descriptor's output report length). */
static void take_output(struct ios_hidpad_device *dev, const uint8_t *data, uint32_t len,
                        struct ios_hidpad_reply *reply)
{
    struct pgios_hidpad_output before = *dev->output;

    reply->output_report = 1;
    reply->output_result = ios_hidpad_decode_output(data, len, dev->output, &reply->output_flags);
    reply->output_changed = !ios_hidpad_output_same(&before, dev->output);
    reply->information = len;
}

void ios_hidpad_write(struct ios_hidpad_device *dev, struct ios_hidpad_handle *handle,
                      const struct pgios_hidpad *pad, uint64_t now_us, const uint8_t *data,
                      uint32_t size, struct ios_hidpad_reply *reply)
{
    const struct ios_hidparse_report *report;

    reply_init(reply);
    if (ios_hidpad_handle_lost(handle, ios_hidpad_presence(dev, pad->connected, now_us)))
    {
        reply_error(reply, IOS_STATUS_DELETE_PENDING);
        return;
    }
    if (!(report = check_report(dev, IOS_HIDPARSE_OUTPUT, data, size, reply))) return;
    take_output(dev, data, report->output_len, reply);
}

void ios_hidpad_ioctl(struct ios_hidpad_device *dev, struct ios_hidpad_handle *handle, uint32_t code,
                      const uint8_t *data, uint32_t data_size, uint32_t out_size,
                      const struct pgios_hidpad *pad, uint64_t now_us, struct ios_hidpad_reply *reply)
{
    const struct ios_hidparse_report *report;
    uint32_t in_size = ios_hidpad_input_size(code, data_size, out_size), value;
    uint8_t info[IOS_HIDPAD_COLLECTION_INFO_SIZE];

    reply_init(reply);

    /* clarifications 4 and 5: a handle that saw the pad gone is a removed device's handle */
    if (ios_hidpad_handle_lost(handle, ios_hidpad_presence(dev, pad->connected, now_us)))
    {
        reply_error(reply, IOS_STATUS_DELETE_PENDING);
        return;
    }

    switch (code)
    {
    case IOS_IOCTL_HID_GET_COLLECTION_INFORMATION:
        if (out_size < sizeof(info))
        {
            reply_error(reply, IOS_STATUS_BUFFER_OVERFLOW);
            break;
        }
        memset(info, 0, sizeof(info));
        info[0] = (uint8_t)dev->desc->preparsed_len;          /* DescriptorSize (ULONG) */
        info[1] = (uint8_t)(dev->desc->preparsed_len >> 8);
        info[2] = (uint8_t)(dev->desc->preparsed_len >> 16);
        info[3] = (uint8_t)(dev->desc->preparsed_len >> 24);
        info[4] = 0;                                           /* Polled: no */
        info[6] = (uint8_t)dev->id->vendor_id;                 /* VendorID */
        info[7] = (uint8_t)(dev->id->vendor_id >> 8);
        info[8] = (uint8_t)dev->id->product_id;                /* ProductID */
        info[9] = (uint8_t)(dev->id->product_id >> 8);
        info[10] = (uint8_t)dev->id->version;                  /* VersionNumber */
        info[11] = (uint8_t)(dev->id->version >> 8);
        reply_bytes(reply, info, sizeof(info));
        break;

    case IOS_IOCTL_HID_GET_COLLECTION_DESCRIPTOR:
        if (out_size < dev->desc->preparsed_len)
        {
            reply_error(reply, IOS_STATUS_INVALID_BUFFER_SIZE);
            break;
        }
        reply->data = dev->desc->preparsed;
        reply->size = dev->desc->preparsed_len;
        reply->information = dev->desc->preparsed_len;
        break;

    case IOS_IOCTL_HID_GET_INPUT_REPORT:
        if (!check_report(dev, IOS_HIDPARSE_INPUT, data, out_size, reply)) break;
        value = (uint32_t)ios_hidpad_build_input(dev->id, pad, dev->report_state, now_us, reply->buffer);
        reply_bytes(reply, reply->buffer, value);
        break;

    case IOS_IOCTL_HID_GET_FEATURE:
        if (!(report = check_report(dev, IOS_HIDPARSE_FEATURE, data, out_size, reply))) break;
        ios_hidpad_build_feature(dev->id, data[0], reply->buffer, report->feature_len);
        reply_bytes(reply, reply->buffer, report->feature_len);
        break;

    case IOS_IOCTL_HID_SET_FEATURE:
        if (!(report = check_report(dev, IOS_HIDPARSE_FEATURE, data, in_size, reply))) break;
        reply->information = report->feature_len;   /* accepted */
        break;

    case IOS_IOCTL_HID_SET_OUTPUT_REPORT:
        if (!(report = check_report(dev, IOS_HIDPARSE_OUTPUT, data, in_size, reply))) break;
        take_output(dev, data, report->output_len, reply);
        break;

    case IOS_IOCTL_HID_GET_MANUFACTURER_STRING:
        reply_string(reply, dev->id->manufacturer, out_size);
        break;
    case IOS_IOCTL_HID_GET_PRODUCT_STRING:
        reply_string(reply, dev->id->product, out_size);
        break;
    case IOS_IOCTL_HID_GET_SERIALNUMBER_STRING:
        reply_string(reply, dev->id->serial, out_size);
        break;
    case IOS_IOCTL_HID_GET_INDEXED_STRING:
        if (in_size < 4)
        {
            reply_error(reply, IOS_STATUS_INVALID_PARAMETER);
            break;
        }
        switch (read_ulong(data) & 0xffff)   /* the high word is a language id */
        {
        case 1: reply_string(reply, dev->id->manufacturer, out_size); break;
        case 2: reply_string(reply, dev->id->product, out_size); break;
        case 3: reply_string(reply, dev->id->serial, out_size); break;
        default: reply_error(reply, IOS_STATUS_INVALID_PARAMETER); break;
        }
        break;

    case IOS_IOCTL_HID_GET_POLL_FREQUENCY_MSEC:
        if (out_size < 4) reply_error(reply, IOS_STATUS_BUFFER_OVERFLOW);
        else reply_ulong(reply, dev->poll_interval);
        break;
    case IOS_IOCTL_HID_SET_POLL_FREQUENCY_MSEC:
        if (in_size < 4)
        {
            reply_error(reply, IOS_STATUS_BUFFER_TOO_SMALL);
            break;
        }
        value = read_ulong(data);
        if (value) dev->poll_interval = value < IOS_HIDPAD_MAX_POLL_INTERVAL ? value : IOS_HIDPAD_MAX_POLL_INTERVAL;
        break;

    case IOS_IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS:
        if (out_size < 4) reply_error(reply, IOS_STATUS_BUFFER_TOO_SMALL);
        else reply_ulong(reply, handle->input_buffers);
        break;
    case IOS_IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS:
        if (in_size != 4)
        {
            reply_error(reply, IOS_STATUS_BUFFER_OVERFLOW);
            break;
        }
        value = read_ulong(data);
        if (value < 2 || value > 512)
        {
            reply_error(reply, IOS_STATUS_INVALID_PARAMETER);
            break;
        }
        handle->input_buffers = value;   /* stored only: reports are built when read */
        break;

    case IOS_IOCTL_HID_FLUSH_QUEUE:
        break;                           /* nothing is queued: reports are built when read */

    case IOS_IOCTL_HID_GET_WINE_RAWINPUT_HANDLE:
        if (out_size < 4) reply_error(reply, IOS_STATUS_BUFFER_OVERFLOW);
        else reply_ulong(reply, dev->rawinput_handle ? dev->rawinput_handle : IOS_HIDPAD_RAWINPUT_HANDLE);
        break;

    default:
        reply_error(reply, IOS_STATUS_NOT_SUPPORTED);
        break;
    }
}

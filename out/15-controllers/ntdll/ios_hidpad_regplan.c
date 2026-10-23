// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part C: the HID pad's registry entries as a plan. See ios_hidpad_regplan.h.
// Key and value names follow what setupapi reads for a HID device (Microsoft's documentation
// of device instance keys, device interface classes and their "#" reference-string subkeys;
// GUID_DEVINTERFACE_HID and the HIDClass setup class GUID).

#include <string.h>

#include "ios_hidpad_regplan.h"

#define ENUM_KEY          "System\\CurrentControlSet\\Enum"
#define DEVICE_CLASSES    "System\\CurrentControlSet\\Control\\DeviceClasses\\"

static int set_text(struct ios_hidpad_reg_value *v, const char *name, const char *text)
{
    size_t n = strlen(text);

    v->name = name;
    v->type = IOS_HIDPAD_REG_SZ;
    if (n + 1 > sizeof(v->text)) return 0;
    memcpy(v->text, text, n + 1);
    v->text_len = n + 1;
    return 1;
}

static void set_dword(struct ios_hidpad_reg_value *v, const char *name, uint32_t value)
{
    v->name = name;
    v->type = IOS_HIDPAD_REG_DWORD;
    v->text[0] = 0;
    v->text_len = 0;
    v->dword = value;
}

static int append(char *dst, size_t size, const char *a, const char *b, const char *c)
{
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    if (la + lb + lc + 1 > size) return 0;
    memcpy(dst, a, la);
    memcpy(dst + la, b, lb);
    memcpy(dst + la + lb, c, lc + 1);
    return 1;
}

unsigned int ios_hidpad_registry_plan(const struct ios_hidpad_identity *id,
                                      struct ios_hidpad_reg_key keys[IOS_HIDPAD_REG_KEYS])
{
    char link_key[IOS_HIDPAD_REG_PATH], user_path[IOS_HIDPAD_REG_PATH], instance_id[IOS_HIDPAD_REG_PATH];
    char hwid[IOS_HIDPAD_REG_PATH], enumerator[32];
    struct ios_hidpad_reg_key *k;
    struct ios_hidpad_reg_value *v;
    const char *rest;
    size_t n, len;
    unsigned int i;

    memset(keys, 0, sizeof(*keys) * IOS_HIDPAD_REG_KEYS);
    if (id->kind == IOS_HIDPAD_NONE) return 0;
    if (!ios_hidpad_registry_link_key(id, link_key, sizeof(link_key))) return 0;
    if (!ios_hidpad_user_path(id, user_path, sizeof(user_path))) return 0;
    if (!ios_hidpad_instance_id(id, instance_id, sizeof(instance_id))) return 0;

    /* the device id's enumerator ("HID") is an ordinary key, the rest is the pad's own */
    if (!(rest = strchr(id->device_id, '\\'))) return 0;
    n = (size_t)(rest - id->device_id);
    if (n + 1 > sizeof(enumerator)) return 0;
    memcpy(enumerator, id->device_id, n);
    enumerator[n] = 0;
    rest++;

    /* 1: HKLM\System\CurrentControlSet\Enum\<device id>\<instance> */
    k = &keys[0];
    if (!append(k->base, sizeof(k->base), ENUM_KEY, "\\", enumerator)) return 0;
    if (!append(k->tail, sizeof(k->tail), rest, "\\", id->instance)) return 0;
    v = k->values;
    if (!set_text(v++, "ClassGUID", IOS_HIDPAD_HIDCLASS_GUID)) return 0;
    if (!set_text(v++, "Class", "HIDClass")) return 0;
    if (!set_text(v++, "DeviceDesc", "HID-compliant game controller")) return 0;
    if (!set_text(v++, "Mfg", id->manufacturer)) return 0;
    v->name = "HardwareID";
    v->type = IOS_HIDPAD_REG_MULTI_SZ;
    for (i = 0, len = 0; i < IOS_HIDPAD_HWID_COUNT; i++)
    {
        if (!(n = ios_hidpad_hardware_id(id, i, hwid, sizeof(hwid)))) return 0;
        if (len + n + 2 > sizeof(v->text)) return 0;
        memcpy(v->text + len, hwid, n + 1);
        len += n + 1;
    }
    v->text[len++] = 0;
    v->text_len = len;
    v++;
    if (!set_text(v++, "ContainerID", id->container_id)) return 0;
    set_dword(v++, "ConfigFlags", 0);
    k->value_count = (unsigned int)(v - k->values);

    /* 2: ...\Control\DeviceClasses\{GUID_DEVINTERFACE_HID}\##?#<link> */
    k = &keys[1];
    if (!append(k->base, sizeof(k->base), DEVICE_CLASSES, IOS_HIDPAD_HID_INTERFACE_GUID, "")) return 0;
    if (!append(k->tail, sizeof(k->tail), link_key, "", "")) return 0;
    if (!set_text(&k->values[0], "DeviceInstance", instance_id)) return 0;
    k->value_count = 1;

    /* 3: its "#" subkey (the interface with an empty reference string) */
    k = &keys[2];
    memcpy(k->base, keys[1].base, sizeof(k->base));
    if (!append(k->tail, sizeof(k->tail), link_key, "\\", "#")) return 0;
    if (!set_text(&k->values[0], "SymbolicLink", user_path)) return 0;
    k->value_count = 1;

    /* 4: "#\Control": Linked = 1, what setupapi's DIGCF_PRESENT filter checks */
    k = &keys[3];
    memcpy(k->base, keys[1].base, sizeof(k->base));
    if (!append(k->tail, sizeof(k->tail), link_key, "\\#\\", "Control")) return 0;
    set_dword(&k->values[0], "Linked", 1);
    k->value_count = 1;

    return IOS_HIDPAD_REG_KEYS;
}

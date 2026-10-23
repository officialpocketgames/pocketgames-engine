// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part F: host tests of Part C's registry plan (key paths, value names, types, data).

#include <stdio.h>
#include <string.h>

#include "ios_hidpad.h"
#include "ios_hidpad_regplan.h"
#include "test_common.h"

#define DS_LINK "HID#VID_054C&PID_0CE6&MI_03#9&4d616465&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}"
#define CLASSES "System\\CurrentControlSet\\Control\\DeviceClasses\\{4d1e55b2-f16f-11cf-88cb-001111000030}"

static const struct ios_hidpad_reg_value *find_value(const struct ios_hidpad_reg_key *k, const char *name)
{
    unsigned int i;
    for (i = 0; i < k->value_count; i++) if (!strcmp(k->values[i].name, name)) return &k->values[i];
    return NULL;
}

static void check_sz(const struct ios_hidpad_reg_key *k, const char *name, const char *text)
{
    const struct ios_hidpad_reg_value *v = find_value(k, name);
    CHECK(v != NULL);
    if (!v) return;
    CHECK_EQ(v->type, IOS_HIDPAD_REG_SZ);
    CHECK_STR(v->text, text);
    CHECK_EQ(v->text_len, strlen(text) + 1);
}

static void check_dword(const struct ios_hidpad_reg_key *k, const char *name, unsigned int value)
{
    const struct ios_hidpad_reg_value *v = find_value(k, name);
    CHECK(v != NULL);
    if (!v) return;
    CHECK_EQ(v->type, IOS_HIDPAD_REG_DWORD);
    CHECK_EQ(v->dword, value);
}

static void test_dualsense(void)
{
    static const char *hwids[] =
    {
        "HID\\VID_054C&PID_0CE6&REV_0100&MI_03", "HID\\VID_054C&PID_0CE6&MI_03",
        "HID\\VID_054C&UP:0001_U:0005", "HID_DEVICE_SYSTEM_GAME", "HID_DEVICE_UP:0001_U:0005", "HID_DEVICE",
    };
    struct ios_hidpad_reg_key keys[IOS_HIDPAD_REG_KEYS];
    struct ios_hidpad_identity id;
    const struct ios_hidpad_reg_value *v;
    const char *p;
    size_t total = 0;
    unsigned int i;

    ios_hidpad_identity_select("dualsense", NULL, &id);
    CHECK_EQ(ios_hidpad_registry_plan(&id, keys), 4);

    CHECK_STR(keys[0].base, "System\\CurrentControlSet\\Enum\\HID");
    CHECK_STR(keys[0].tail, "VID_054C&PID_0CE6&MI_03\\9&4d616465&0&0000");
    CHECK_EQ(keys[0].value_count, 7);
    check_sz(&keys[0], "ClassGUID", "{745a17a0-74d3-11d0-b6fe-00a0c90f57da}");
    check_sz(&keys[0], "Class", "HIDClass");
    check_sz(&keys[0], "DeviceDesc", "HID-compliant game controller");
    check_sz(&keys[0], "Mfg", "Sony Interactive Entertainment");
    check_sz(&keys[0], "ContainerID", id.container_id);
    check_dword(&keys[0], "ConfigFlags", 0);
    v = find_value(&keys[0], "HardwareID");
    CHECK(v != NULL);
    if (v)
    {
        CHECK_EQ(v->type, IOS_HIDPAD_REG_MULTI_SZ);
        for (i = 0, p = v->text; i < 6; i++)
        {
            CHECK_STR(p, hwids[i]);
            total += strlen(hwids[i]) + 1;
            p += strlen(p) + 1;
        }
        CHECK_EQ(*p, 0);                /* the list's own terminator */
        CHECK_EQ(v->text_len, total + 1);
    }

    CHECK_STR(keys[1].base, CLASSES);
    CHECK_STR(keys[1].tail, "##?#" DS_LINK);
    CHECK_EQ(keys[1].value_count, 1);
    check_sz(&keys[1], "DeviceInstance", "HID\\VID_054C&PID_0CE6&MI_03\\9&4d616465&0&0000");

    CHECK_STR(keys[2].base, CLASSES);
    CHECK_STR(keys[2].tail, "##?#" DS_LINK "\\#");
    CHECK_EQ(keys[2].value_count, 1);
    check_sz(&keys[2], "SymbolicLink", "\\\\?\\" DS_LINK);

    CHECK_STR(keys[3].base, CLASSES);
    CHECK_STR(keys[3].tail, "##?#" DS_LINK "\\#\\Control");
    CHECK_EQ(keys[3].value_count, 1);
    check_dword(&keys[3], "Linked", 1);
}

static void test_generic(void)
{
    struct ios_hidpad_reg_key keys[IOS_HIDPAD_REG_KEYS];
    struct ios_hidpad_identity id;
    const struct ios_hidpad_reg_value *v;

    ios_hidpad_identity_select("generic", "Some Pad", &id);
    CHECK_EQ(ios_hidpad_registry_plan(&id, keys), 4);
    CHECK_STR(keys[0].tail, "VID_1209&PID_4D47\\9&4d616465&0&0000");
    check_sz(&keys[0], "Mfg", "MYIOSDECK");
    v = find_value(&keys[0], "HardwareID");
    CHECK(v && !strcmp(v->text, "HID\\VID_1209&PID_4D47&REV_0100"));
    check_sz(&keys[1], "DeviceInstance", "HID\\VID_1209&PID_4D47\\9&4d616465&0&0000");
    check_sz(&keys[2], "SymbolicLink",
             "\\\\?\\HID#VID_1209&PID_4D47#9&4d616465&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}");
    CHECK(strcmp(id.container_id, "") != 0);
    check_sz(&keys[0], "ContainerID", id.container_id);
}

static void test_none(void)
{
    struct ios_hidpad_reg_key keys[IOS_HIDPAD_REG_KEYS];
    struct ios_hidpad_identity id;

    ios_hidpad_identity_select(NULL, NULL, &id);
    CHECK_EQ(ios_hidpad_registry_plan(&id, keys), 0);
    CHECK_EQ(keys[0].value_count, 0);
}

int main(void)
{
    test_dualsense();
    test_generic();
    test_none();
    return test_report("test_regplan");
}

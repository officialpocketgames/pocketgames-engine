// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 19 follow-up A14, test driver: compiles one HLSL file with vkd3d-shader as Wine's
// D3DCompile does (vkd3d-utils' D3DCompile2VKD3D: HLSL source, the API version option, entry
// "main"), prints the compiler's messages to stderr and the disassembly of the result to stdout.
//   hlslc [-p profile] file.hlsl     (default profile ps_5_0; exit status 1 on a failed compile)
// Profiles below 4.0 (A15's ps_2_0 check) compile to and disassemble Direct3D 9 bytecode.
// -s (A16) also prints the input and output signatures (".param" lines) before the code.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vkd3d_shader.h>

static char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    char *data;
    long n;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET))
    {
        fclose(f);
        return NULL;
    }
    if (!(data = malloc((size_t)n + 1)) || fread(data, 1, (size_t)n, f) != (size_t)n)
    {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    data[n] = 0;
    *size = (size_t)n;
    return data;
}

int main(int argc, char **argv)
{
    struct vkd3d_shader_compile_option option = {VKD3D_SHADER_COMPILE_OPTION_API_VERSION,
            VKD3D_SHADER_API_VERSION_1_19};
    struct vkd3d_shader_compile_option formatting = {VKD3D_SHADER_COMPILE_OPTION_FORMATTING,
            VKD3D_SHADER_COMPILE_OPTION_FORMATTING_INDENT | VKD3D_SHADER_COMPILE_OPTION_FORMATTING_HEADER
            | VKD3D_SHADER_COMPILE_OPTION_FORMATTING_IO_SIGNATURES};
    int signatures = 0;
    struct vkd3d_shader_hlsl_source_info hlsl_info;
    struct vkd3d_shader_compile_info info;
    struct vkd3d_shader_code dxbc, text;
    const char *profile = "ps_5_0", *path = NULL;
    enum vkd3d_shader_target_type target = VKD3D_SHADER_TARGET_DXBC_TPF;
    char *messages = NULL, *src;
    size_t size;
    int i, ret;

    for (i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "-p") && i + 1 < argc)
            profile = argv[++i];
        else if (!strcmp(argv[i], "-s"))
            signatures = 1;
        else
            path = argv[i];
    }
    if (!path || !(src = read_file(path, &size)))
    {
        fprintf(stderr, "usage: hlslc [-s] [-p profile] file.hlsl\n");
        return 2;
    }

    memset(&hlsl_info, 0, sizeof(hlsl_info));
    hlsl_info.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl_info.profile = profile;
    hlsl_info.entry_point = "main";

    memset(&info, 0, sizeof(info));
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &hlsl_info;
    info.source.code = src;
    info.source.size = size;
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    if (strlen(profile) > 3 && profile[3] >= '1' && profile[3] <= '3')
        target = VKD3D_SHADER_TARGET_D3D_BYTECODE;
    info.target_type = target;
    info.options = &option;
    info.option_count = 1;
    info.log_level = VKD3D_SHADER_LOG_INFO;
    info.source_name = path;

    ret = vkd3d_shader_compile(&info, &dxbc, &messages);
    if (messages)
        fputs(messages, stderr);
    vkd3d_shader_free_messages(messages);
    free(src);
    if (ret < 0)
        return 1;

    memset(&info, 0, sizeof(info));
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.source = dxbc;
    info.source_type = target == VKD3D_SHADER_TARGET_D3D_BYTECODE
            ? VKD3D_SHADER_SOURCE_D3D_BYTECODE : VKD3D_SHADER_SOURCE_DXBC_TPF;
    info.target_type = VKD3D_SHADER_TARGET_D3D_ASM;
    if (signatures)
    {
        info.options = &formatting;
        info.option_count = 1;
    }
    info.log_level = VKD3D_SHADER_LOG_INFO;
    messages = NULL;
    ret = vkd3d_shader_compile(&info, &text, &messages);
    if (messages)
        fputs(messages, stderr);
    vkd3d_shader_free_messages(messages);
    vkd3d_shader_free_shader_code(&dxbc);
    if (ret < 0)
        return 1;
    fwrite(text.code, 1, text.size, stdout);
    vkd3d_shader_free_shader_code(&text);
    return 0;
}

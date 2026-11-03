#!/usr/bin/perl
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# test-wow-classify.pl <opengl32 unixlib.h> [--list]
#
# Spec 35 Part J1: the generator's type classification (pe/dlls/opengl32/make_opengl_ios.pm), on
# every parameter of every entry of the given unixlib.h (Wine 11.4's, which the registry run of
# make_opengl wrote: its struct <name>_params hold each function's parameters as the registry
# declares them). Checks: every pointer type, handle and client object gets a pointer-aware class
# (never the plain number of upstream); the known cases (HDC a handle; HGLRC, HPBUFFERARB and
# strings host pointers; GLsync; callbacks and PROC numbers; GLintptr numbers; glVertexPointer,
# glDrawElements, glTexImage2D and glReadPixels buffer offsets); every function and parameter
# of the buffer-offset table exists. --list prints "<type> <class>" per type (CHANGES.md's table).

use strict;
use warnings;
use FindBin;
use lib "$FindBin::Bin/../pe/dlls/opengl32";
use make_opengl_ios qw( wow64_class wow64_ret_class wow64_array_class %buffer_offset_params );

my ($header, $list) = @ARGV;
die "usage: $0 <unixlib.h> [--list]\n" unless $header;

my (%params, %types, $func);
open my $fh, "<", $header or die "cannot open $header: $!";
while (my $line = <$fh>)
{
    $line =~ s/\r//;
    if ($line =~ /^struct (\w+)_params$/) { $func = $1; next; }
    if (defined $func && $line =~ /^\};/) { undef $func; next; }
    next unless defined $func;
    next unless $line =~ /^\s+(.*?)\s*\b(\w+);\s*$/;
    my ($type, $name) = ($1, $2);
    next if $name eq "teb";
    $type =~ s/\s+$//;
    $params{$func}{$name} = $type;
    push @{$types{$type}}, [ $func, $name ];
}
close $fh;

my ($checks, $failures) = (0, 0);
sub check($$)
{
    my ($ok, $what) = @_;
    $checks++;
    unless ($ok) { $failures++; print "CHECK failed: $what\n"; }
}

check( scalar(keys %params) > 2000, "parsed the param structs (" . scalar(keys %params) . ")" );

# every type: pointers never stay plain numbers, except the documented number types
my %class_of;
foreach my $type (sort keys %types)
{
    foreach my $use (@{$types{$type}})
    {
        my ($f, $p) = @$use;
        my $class = $p eq "ret" ? wow64_ret_class( $type, $f ) : wow64_class( $type, $f, $p );
        $class_of{$type}{$class}++;
        if (($type =~ /\*|\[/ || $type =~ /^(const )?(LP\w+|GLstring)$/) && $p ne "ret")
        {
            my $ok = $class =~ /^(HOST|OFFSET:GL_\w+_BINDING)$/ ||
                     ($class eq "NUMBER" && ($type =~ /PROC|eglImage|ClientBuffer/ || ($f =~ /^glDebugMessageCallback/ && $p eq "userParam")));
            check( $ok, "$f $p: $type is $class" );
        }
        check( $class ne "NUMBER" || $type !~ /^(const )?H[A-Z0-9]+$/, "$f $p: handle $type as a number" );
    }
}

# the known cases
check( wow64_class( "HDC", "wglSwapBuffers", "hdc" ) eq "HANDLE", "HDC" );
check( wow64_class( "HGLRC", "wglMakeCurrent", "newContext" ) eq "HOST", "HGLRC" );
check( wow64_class( "HPBUFFERARB", "wglGetPbufferDCARB", "hPbuffer" ) eq "HOST", "HPBUFFERARB" );
check( wow64_class( "GLsync", "glClientWaitSync", "sync" ) eq "SYNC", "GLsync" );
check( wow64_class( "LPCSTR", "wglGetProcAddress", "lpszProc" ) eq "HOST", "LPCSTR" );
check( wow64_class( "const GLchar *", "glGetUniformLocation", "name" ) eq "HOST", "GLchar *" );
check( wow64_class( "GLDEBUGPROC", "glDebugMessageCallback", "callback" ) eq "NUMBER", "GLDEBUGPROC" );
check( wow64_class( "const void *", "glDebugMessageCallback", "userParam" ) eq "NUMBER", "userParam" );
check( wow64_class( "GLintptr", "glBufferSubData", "offset" ) eq "NUMBER", "GLintptr" );
check( wow64_class( "GLsizeiptr", "glBufferData", "size" ) eq "NUMBER", "GLsizeiptr" );
check( wow64_class( "const void *", "glBufferData", "data" ) eq "HOST", "glBufferData data" );
check( wow64_class( "const void *", "glVertexPointer", "pointer" ) eq "OFFSET:GL_ARRAY_BUFFER_BINDING", "glVertexPointer" );
check( wow64_class( "const void *", "glDrawElements", "indices" ) eq "OFFSET:GL_ELEMENT_ARRAY_BUFFER_BINDING", "glDrawElements" );
check( wow64_class( "const void *", "glTexImage2D", "pixels" ) eq "OFFSET:GL_PIXEL_UNPACK_BUFFER_BINDING", "glTexImage2D" );
check( wow64_class( "void *", "glReadPixels", "pixels" ) eq "OFFSET:GL_PIXEL_PACK_BUFFER_BINDING", "glReadPixels" );
check( wow64_class( "const GLvoid *", "glCallLists", "lists" ) eq "HOST", "glCallLists" );
check( wow64_ret_class( "PROC", "wglGetProcAddress" ) eq "NUMBER", "PROC result" );
check( wow64_ret_class( "void *", "glMapBuffer" ) eq "HOST", "glMapBuffer result" );
check( wow64_ret_class( "HGLRC", "wglCreateContext" ) eq "HOST", "HGLRC result" );
check( wow64_ret_class( "HDC", "wglGetPbufferDCARB" ) eq "HANDLE", "HDC result" );
check( wow64_ret_class( "GLsync", "glFenceSync" ) eq "HOST", "GLsync result" );
check( wow64_array_class( "const GLchar *const*", "glShaderSource", "string" ) eq "PTRS", "glShaderSource" );
check( wow64_array_class( "const void *const*", "glMultiDrawElements", "indices" ) eq "PTRS_OR_OFFSETS:GL_ELEMENT_ARRAY_BUFFER_BINDING", "glMultiDrawElements" );
check( wow64_array_class( "const GLintptr *", "glBindBuffersRange", "offsets" ) eq "VALUES", "glBindBuffersRange" );

# every function and parameter of the buffer-offset table exists, as a pointer
foreach my $f (sort keys %buffer_offset_params)
{
    foreach my $p (sort keys %{$buffer_offset_params{$f}})
    {
        my $type = $params{$f}{$p};
        check( defined $type, "$f has no parameter $p" );
        check( !defined $type || $type =~ /\*/, "$f $p is not a pointer" );
    }
}

if ($list && $list eq "--list")
{
    foreach my $type (sort keys %class_of)
    {
        printf "%-40s %s\n", $type, join( ", ", sort keys %{$class_of{$type}} );
    }
}

print "test-wow-classify: $checks checks, $failures failures (" . scalar(keys %types) . " types)\n";
exit( $failures ? 1 : 0 );

# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 35 Part G1: how make_opengl's WoW64 thunks convert each 32-bit field on iOS, where a
# 32-bit program's address a is host address ios_wow_base() + a (spec 06, spec 10 E). Kept apart
# from make_opengl so the host tests can check it without the Khronos registry or XML::LibXML
# (tests/test-wow-classify.pl).
#
# wow64_class( type, function, parameter ) classifies one parameter by its C type (as the
# registry gives it, e.g. "const void *") and, for the few that need it, by function and
# parameter name:
#   NUMBER          a 32-bit number widened as upstream does (GLintptr, callbacks, PROC, ...)
#   HANDLE          a kernel handle (HDC and every other H* but the two below): WOW_HANDLE
#   HOST            a pointer into the guest window (T *, strings, HGLRC and HPBUFFERARB, which
#                   point to opengl32's client objects in PE memory): WOW_HOST
#   SYNC            a GLsync client object: get_unix_sync( WOW_HOST(x) )
#   OFFSET:<b>      a pointer, or an offset into the buffer object bound to binding <b> when one
#                   is bound (glVertexPointer, glDrawElements, glTexImage2D, glReadPixels, ...):
#                   WOW_HOST_OR_OFFSET( teb, <b>, x )
# EGL images and client buffers (host driver objects no Windows program holds) count as NUMBER.

package make_opengl_ios;

use strict;
use warnings;

use Exporter 'import';
our @EXPORT_OK = qw( wow64_class wow64_ret_class wow64_array_class %buffer_offset_params );

# Parameters that are offsets when a buffer object is bound (OpenGL 1.5's vertex and index
# buffers, OpenGL 2.1's pixel pack and unpack buffers), by function and parameter name.
my $array   = "GL_ARRAY_BUFFER_BINDING";
my $element = "GL_ELEMENT_ARRAY_BUFFER_BINDING";
my $unpack  = "GL_PIXEL_UNPACK_BUFFER_BINDING";
my $pack    = "GL_PIXEL_PACK_BUFFER_BINDING";

our %buffer_offset_params =
    (
     # vertex arrays
     "glVertexPointer" => { pointer => $array },
     "glNormalPointer" => { pointer => $array },
     "glColorPointer" => { pointer => $array },
     "glIndexPointer" => { pointer => $array },
     "glTexCoordPointer" => { pointer => $array },
     "glEdgeFlagPointer" => { pointer => $array },
     "glSecondaryColorPointer" => { pointer => $array },
     "glSecondaryColorPointerEXT" => { pointer => $array },
     "glFogCoordPointer" => { pointer => $array },
     "glFogCoordPointerEXT" => { pointer => $array },
     "glVertexAttribPointer" => { pointer => $array },
     "glVertexAttribPointerARB" => { pointer => $array },
     "glVertexAttribIPointer" => { pointer => $array },
     "glVertexAttribIPointerEXT" => { pointer => $array },
     "glVertexAttribPointerNV" => { pointer => $array },
     "glInterleavedArrays" => { pointer => $array },
     "glVertexPointerEXT" => { pointer => $array },
     "glNormalPointerEXT" => { pointer => $array },
     "glColorPointerEXT" => { pointer => $array },
     "glIndexPointerEXT" => { pointer => $array },
     "glTexCoordPointerEXT" => { pointer => $array },
     "glEdgeFlagPointerEXT" => { pointer => $array },
     "glWeightPointerARB" => { pointer => $array },
     "glMatrixIndexPointerARB" => { pointer => $array },
     # indices
     "glDrawElements" => { indices => $element },
     "glDrawRangeElements" => { indices => $element },
     "glDrawRangeElementsEXT" => { indices => $element },
     "glDrawElementsInstanced" => { indices => $element },
     "glDrawElementsInstancedARB" => { indices => $element },
     "glDrawElementsInstancedEXT" => { indices => $element },
     "glDrawElementsBaseVertex" => { indices => $element },
     "glDrawRangeElementsBaseVertex" => { indices => $element },
     "glDrawElementsInstancedBaseVertex" => { indices => $element },
     # arrays of indices (each element)
     "glMultiDrawElements" => { indices => $element },
     "glMultiDrawElementsEXT" => { indices => $element },
     "glMultiDrawElementsBaseVertex" => { indices => $element },
     # pixel unpack
     "glTexImage1D" => { pixels => $unpack },
     "glTexImage2D" => { pixels => $unpack },
     "glTexImage3D" => { pixels => $unpack },
     "glTexImage3DEXT" => { pixels => $unpack },
     "glTexSubImage1D" => { pixels => $unpack },
     "glTexSubImage2D" => { pixels => $unpack },
     "glTexSubImage3D" => { pixels => $unpack },
     "glTexSubImage1DEXT" => { pixels => $unpack },
     "glTexSubImage2DEXT" => { pixels => $unpack },
     "glTexSubImage3DEXT" => { pixels => $unpack },
     "glCompressedTexImage1D" => { data => $unpack },
     "glCompressedTexImage2D" => { data => $unpack },
     "glCompressedTexImage3D" => { data => $unpack },
     "glCompressedTexSubImage1D" => { data => $unpack },
     "glCompressedTexSubImage2D" => { data => $unpack },
     "glCompressedTexSubImage3D" => { data => $unpack },
     "glCompressedTexImage1DARB" => { data => $unpack },
     "glCompressedTexImage2DARB" => { data => $unpack },
     "glCompressedTexImage3DARB" => { data => $unpack },
     "glCompressedTexSubImage1DARB" => { data => $unpack },
     "glCompressedTexSubImage2DARB" => { data => $unpack },
     "glCompressedTexSubImage3DARB" => { data => $unpack },
     "glDrawPixels" => { pixels => $unpack },
     "glBitmap" => { bitmap => $unpack },
     "glPolygonStipple" => { mask => $unpack },
     "glColorTable" => { table => $unpack },
     "glColorTableEXT" => { table => $unpack },
     "glColorSubTable" => { data => $unpack },
     "glColorSubTableEXT" => { data => $unpack },
     "glConvolutionFilter1D" => { image => $unpack },
     "glConvolutionFilter2D" => { image => $unpack },
     "glSeparableFilter2D" => { row => $unpack, column => $unpack },
     "glPixelMapfv" => { values => $unpack },
     "glPixelMapuiv" => { values => $unpack },
     "glPixelMapusv" => { values => $unpack },
     # pixel pack
     "glReadPixels" => { pixels => $pack },
     "glReadnPixels" => { data => $pack },
     "glReadnPixelsARB" => { data => $pack },
     "glGetTexImage" => { pixels => $pack },
     "glGetCompressedTexImage" => { img => $pack },
     "glGetCompressedTexImageARB" => { img => $pack },
     "glGetPolygonStipple" => { mask => $pack },
     "glGetColorTable" => { table => $pack },
     "glGetConvolutionFilter" => { image => $pack },
     "glGetSeparableFilter" => { row => $pack, column => $pack, span => $pack },
     "glGetHistogram" => { values => $pack },
     "glGetMinmax" => { values => $pack },
     "glGetPixelMapfv" => { values => $pack },
     "glGetPixelMapuiv" => { values => $pack },
     "glGetPixelMapusv" => { values => $pack },
    );

# client objects in PE memory, reached through pointers
my %client_objects = ( "HGLRC" => 1, "HPBUFFERARB" => 1 );

# pointer types without a '*' in their name (make_opengl's %arg_types "ptr" and "str")
my %pointer_typedefs = ( "LPCSTR" => 1, "LPGLYPHMETRICSFLOAT" => 1, "GLstring" => 1 );

# pointer-sized values that aren't addresses in the guest window
my %numbers =
    (
     "PROC" => 1,                   # wglGetProcAddress hands back a registry index
     "GLDEBUGPROC" => 1,            # callbacks are guest code addresses, kept as numbers
     "GLDEBUGPROCARB" => 1,
     "GLDEBUGPROCAMD" => 1,
     "GLDEBUGPROCKHR" => 1,
     "GLVULKANPROCNV" => 1,
     "GLeglImageOES" => 1,          # EGL objects of the host driver (not reachable from Windows)
     "GLeglClientBufferEXT" => 1,
    );

sub base_type($)
{
    my $type = shift;
    $type =~ s/^\s+|\s+$//g;
    $type =~ s/^const\s+//;
    return $type;
}

sub wow64_class($$$)
{
    my ($type, $func, $pname) = @_;
    my $base = base_type( $type );

    # glDebugMessageCallback*'s userParam is handed back to the guest callback as it came
    return "NUMBER" if $func =~ /^glDebugMessageCallback/ && $pname eq "userParam";
    return "NUMBER" if defined $numbers{$base};
    return "SYNC" if $base eq "GLsync";
    return "HOST" if defined $client_objects{$base};
    return "HANDLE" if $base =~ /^H[A-Z0-9]+$/;
    if ($type =~ /\*|\[/ || defined $pointer_typedefs{$base})
    {
        my $binding = $buffer_offset_params{$func} ? $buffer_offset_params{$func}->{$pname} : undef;
        return "OFFSET:$binding" if defined $binding;
        return "HOST";
    }
    return "NUMBER" if $base =~ /(sizei|int)ptr/;
    return "NUMBER";
}

# a function's result of pointer size: HOST results go back with WOW_GUEST, the rest as numbers
sub wow64_ret_class($$)
{
    my ($type, $func) = @_;
    my $base = base_type( $type );

    return "NUMBER" if defined $numbers{$base};
    return "HOST" if $base eq "GLsync" || defined $client_objects{$base};
    return "HANDLE" if $base =~ /^H[A-Z0-9]+$/;
    return "HOST" if $type =~ /\*/ || defined $pointer_typedefs{$base};
    return "NUMBER";
}

# an array of pointer_array_count: PTRS (guest pointers, each WOW_HOST), PTRS_OR_OFFSETS:<b>
# (buffer offsets when a buffer is bound) or VALUES (GLintptr / GLsizeiptr numbers)
sub wow64_array_class($$$)
{
    my ($type, $func, $pname) = @_;

    return "VALUES" if $type =~ /(sizei|int)ptr.*\*/;
    my $binding = $buffer_offset_params{$func} ? $buffer_offset_params{$func}->{$pname} : undef;
    return "PTRS_OR_OFFSETS:$binding" if defined $binding;
    return "PTRS";
}

1;

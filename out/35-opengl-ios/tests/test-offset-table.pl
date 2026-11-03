#!/usr/bin/perl
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# test-offset-table.pl <gl.xml> [--list]
#
# Spec 35 Part G1 and clarification 2.3: the buffer-offset table of the generator
# (pe/dlls/opengl32/make_opengl_ios.pm, %buffer_offset_params, the authoritative list) against
# the Khronos registry (gl.xml at OpenGL-Registry 0b449b97, what Wine 11.4's opengl32 is generated
# from). Plain Perl: the registry is read with regular expressions, no XML module.
#
# For each entry: the command is in the registry, the parameter is one of its parameters and a
# pointer, the binding is a registry enum, and the binding fits the command (vertex arrays: the
# array buffer; indices: the element buffer; reading pixels back: the pack buffer; the rest of the
# pixel commands: the unpack buffer). The other way: every command of OpenGL 1.0-2.1 and
# GL_ARB_imaging that reads or writes vertex, index or pixel data through a pointer (by family:
# *Pointer, *Elements*, the texture images, pixels, bitmaps, stipples, pixel maps and the imaging
# tables, filters, histogram and minmax) has its data parameter in the table. --list prints the
# table with each parameter's registry type and the version or extension that brings it.

use strict;
use warnings;
use FindBin;
use lib "$FindBin::Bin/../pe/dlls/opengl32";
use make_opengl_ios qw( %buffer_offset_params );

my ($xml, $list) = @ARGV;
die "usage: $0 <gl.xml> [--list]\n" unless $xml;
open my $fh, "<", $xml or die "$0: cannot read $xml: $!\n";
my $text = do { local $/; <$fh> };
close $fh;

my $checks = 0;
my $failures = 0;
sub check($$)
{
    my ($ok, $what) = @_;
    $checks++;
    unless ($ok) { $failures++; print "FAIL test-offset-table: $what\n"; }
}

# ---- the registry: commands, enums, where each command comes from
my (%commands, %enums, %since);
while ($text =~ m{<command\b[^>]*>(.*?)</command>}gs)
{
    my $body = $1;
    next unless $body =~ m{<proto\b[^>]*>.*?<name>(\w+)</name>.*?</proto>}s;
    my $name = $1;
    my @params;
    while ($body =~ m{<param\b[^>]*>(.*?)</param>}gs)
    {
        my $p = $1;
        next unless $p =~ m{<name>(\w+)</name>};
        my $pname = $1;
        (my $type = $p) =~ s{<name>\w+</name>}{};
        $type =~ s{<[^>]+>}{}g;
        $type =~ s/\s+/ /g;
        $type =~ s/^\s+|\s+$//g;
        push @params, [ $pname, $type ];
    }
    $commands{$name} = \@params unless $commands{$name};
}
while ($text =~ m{<enum\b([^>]*?)/?>}g)
{
    my $attrs = $1;
    next unless $attrs =~ /\bname="(\w+)"/;
    my $name = $1;
    $enums{$name} = $1 if $attrs =~ /\bvalue="([^"]+)"/;
}
while ($text =~ m{<feature\b[^>]*\bapi="gl"[^>]*\bnumber="([\d.]+)"[^>]*>(.*?)</feature>}gs)
{
    my ($number, $body) = ($1, $2);
    $body =~ s{<remove\b.*?</remove>}{}gs;
    while ($body =~ m{<command name="(\w+)"}g) { $since{$1} = "GL $number" unless $since{$1}; }
}
while ($text =~ m{<extension\b([^>]*)>}g)
{
    my $attrs = $1;
    next if $attrs =~ m{/\s*$};   # <extension .../>: nothing required
    next unless $attrs =~ /\bname="(\w+)"/;
    my $ext = $1;
    my $start = pos( $text );
    my $end = index( $text, "</extension>", $start );
    last if $end < 0;
    my $body = substr( $text, $start, $end - $start );
    while ($body =~ m{<command name="(\w+)"}g) { $since{$1} = $ext unless $since{$1}; }
}
check( scalar keys %commands > 2000, "the registry has commands (" . scalar( keys %commands ) . ")" );

# ---- the families: which binding a command's data parameter belongs to
sub family($)
{
    my $f = shift;
    return "GL_ARRAY_BUFFER_BINDING" if $f =~ /Pointer(EXT|ARB|NV)?$/ || $f eq "glInterleavedArrays";
    return "GL_ELEMENT_ARRAY_BUFFER_BINDING" if $f =~ /Elements/;
    return "GL_PIXEL_PACK_BUFFER_BINDING"
        if $f =~ /^glReadn?Pixels/ || $f =~ /^glGet(Compressed)?TexImage/ || $f =~ /^glGet(PolygonStipple|ColorTable|ConvolutionFilter|SeparableFilter|Histogram|Minmax|PixelMap)/;
    return "GL_PIXEL_UNPACK_BUFFER_BINDING"
        if $f =~ /^gl(Compressed)?Tex(Sub)?Image[123]D/ || $f =~ /^gl(DrawPixels|Bitmap|PolygonStipple|ColorTable|ColorSubTable|ConvolutionFilter[12]D|SeparableFilter2D|PixelMap)/;
    return undef;
}

# ---- every entry against the registry
my @rows;
foreach my $f (sort keys %buffer_offset_params)
{
    my $params = $commands{$f};
    check( $params, "$f is not in the registry" );
    foreach my $p (sort keys %{$buffer_offset_params{$f}})
    {
        my $binding = $buffer_offset_params{$f}->{$p};
        my ($param) = $params ? grep { $_->[0] eq $p } @$params : ();
        check( !$params || $param, "$f has no parameter $p in the registry" );
        check( !$param || $param->[1] =~ /\*/, "$f $p is not a pointer in the registry ($param->[1])" ) if $param;
        check( exists $enums{$binding}, "$f $p: $binding is not a registry enum" );
        my $want = family( $f );
        check( defined $want && $want eq $binding, "$f $p: $binding, but the command's family says " . ($want // "none") );
        push @rows, [ $f, $p, $param ? $param->[1] : "?", $binding, $enums{$binding} // "?", $since{$f} // "?" ];
    }
}

# ---- the other way: OpenGL 1.0-2.1 and GL_ARB_imaging commands of the families
my %data_names = map { $_ => 1 } qw( pointer indices pixels data img bitmap mask table image row column span values );
my $covered = 0;
foreach my $f (sort keys %commands)
{
    my $since = $since{$f} // "";
    next unless $since =~ /^GL (1\.\d|2\.[01])$/ || $since eq "GL_ARB_imaging";
    next unless defined family( $f );
    foreach my $param (@{$commands{$f}})
    {
        my ($p, $type) = @$param;
        next unless $data_names{$p} && $type =~ /\*/;
        $covered++;
        check( $buffer_offset_params{$f} && $buffer_offset_params{$f}->{$p}, "$f ($since) $p: not in the table" );
    }
}
check( $covered > 50, "the families' parameters of OpenGL 1.0-2.1 found ($covered)" );

if ($list && $list eq "--list")
{
    printf "%-34s %-8s %-18s %-32s %-7s %s\n", "function", "param", "registry type", "binding", "value", "since";
    printf "%-34s %-8s %-18s %-32s %-7s %s\n", @$_ foreach @rows;
}
printf "test-offset-table: %d checks, %d failures (%d entries; %d parameters of OpenGL 1.0-2.1 and GL_ARB_imaging)\n",
    $checks, $failures, scalar @rows, $covered;
exit( $failures ? 1 : 0 );

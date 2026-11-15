#!/usr/bin/perl
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
# verify-advsets.pl - read-only checker for the BlueToolFixup -btlfxadvsets / -btlfxa2dpcheck patterns.
# Usage: perl verify-advsets.pl [/usr/sbin/bluetoothd] [path/to/BlueToolFixup.cpp]
# Prints the x86_64 slice LC_UUID and, for each pattern, the number of matches (original form and
# already-patched form), VA, and whether the match crosses a 4 KiB page (the kext patches page by page).
# NOTE: Lilu patches pages in the UBC, so while bluetoothd runs with the kext active, a read of the
# file may show the PATCHED bytes. That is why the patched form is also searched.
use strict; use warnings; no warnings "portable";

my $file = shift // "/usr/sbin/bluetoothd";
my $src  = shift; # optional: BlueToolFixup.cpp, to check the C arrays themselves
open my $fh, '<:raw', $file or die "open $file: $!";
local $/; my $all = <$fh>; close $fh;

# --- extract x86_64 slice ---
my $d;
my $magic = unpack('N', $all);
if ($magic == 0xcafebabe) {
    my $n = unpack('N', substr($all, 4, 4));
    for my $i (0 .. $n - 1) {
        my ($ct, $cst, $off, $sz) = unpack('N4', substr($all, 8 + $i * 20, 16));
        if ($ct == 0x01000007) { $d = substr($all, $off, $sz); printf "fat: x86_64 slice at file offset 0x%x size 0x%x\n", $off, $sz; }
    }
    die "no x86_64 slice\n" unless defined $d;
} elsif (unpack('V', $all) == 0xfeedfacf) {
    $d = $all;
} else { die "not Mach-O\n"; }

# --- LC_UUID, __TEXT ---
my ($mg, $ct, $cs, $ft, $ncmds, $sizeofcmds) = unpack('V6', $d);
my $p = 32; my $textva = 4294967296;
for (1 .. $ncmds) {
    my ($cmd, $len) = unpack('VV', substr($d, $p, 8));
    if ($cmd == 0x1b) {
        my @u = unpack('C16', substr($d, $p + 8, 16));
        printf "LC_UUID (at header offset 0x%x): %s\n", $p, join('', map { sprintf '%02X', $_ } @u);
        printf "  as C array: { %s }\n", join(', ', map { sprintf '0x%02X', $_ } @u);
    }
    $p += $len;
}
printf "ncmds=%d sizeofcmds=0x%x (header+cmds end at 0x%x, %s first 4 KiB page)\n\n",
    $ncmds, $sizeofcmds, 32 + $sizeofcmds, (32 + $sizeofcmds <= 4096 ? 'inside' : 'BEYOND');

sub search {
    my ($name, $hex) = @_;
    my @b = split ' ', $hex; my $len = @b;
    my $re = join '', map { $_ eq '??' ? '[\x00-\xff]' : sprintf('\x%s', $_) } @b;
    my @hits; while ($d =~ /(?=$re)/gs) { push @hits, pos($d); pos($d) = pos($d) + 1; }
    printf "  %-10s hits=%d", $name, scalar @hits;
    for my $o (@hits) {
        my $cross = (int($o / 4096) != int(($o + $len - 1) / 4096)) ? ' CROSSES-PAGE' : '';
        printf "  va=0x%x%s", $o + $textva, $cross;
    }
    print "\n";
    return scalar @hits;
}

# [name, original pattern, patched pattern]  (?? = wildcard)
my @seq = (
 ['1 SpecAdvSetsCap (15.x, exact)', '48 8B 08 48 89 C7 FF 91 60 08 00 00 41 89 C6', '48 8B 08 48 89 C7 B0 01 90 90 90 90 41 89 C6'],
 ['2 ExtScanCap (15.x, exact)',     '48 8B 08 48 89 C7 FF 91 48 08 00 00 41 89 C6', '48 8B 08 48 89 C7 B0 01 90 90 90 90 41 89 C6'],
 ['3 LeEventMask (15.x, masked)',   '85 C0 48 8D 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 0F 44 D0', '85 C0 48 8D 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 0F 1F 40 00'],
 ['4 AdvSetAddr (15.x, masked)',    '83 B8 ?? ?? 00 00 03 77 ?? 31 FF E8', '83 B8 ?? ?? 00 00 03 EB 07 31 FF E8'],
 ['5 ExtCreateConn (15.x, masked)', '0F B6 BD ?? FF FF FF E8 ?? ?? ?? FF 44 0F B6 BD', 'BF 01 00 00 00 66 90 E8 ?? ?? ?? FF 44 0F B6 BD'],
 ['6 AdvSetAddrResend (15.x, masked)', '8B ?? ?? 03 33 ?? ?? 11 0D 00 00 44 09 ?? 74 ??', '8B ?? ?? 03 33 ?? ?? 11 0D 00 00 44 09 ?? 66 90'],
);
my @tahoe = (
 ['1 SpecAdvSetsCap (26.4.1)', '48 8B 08 48 89 C7 FF 91 C0 09 00 00 41 89 C6'],
 ['2 ExtScanCap (26.4.1)',     '48 8B 08 48 89 C7 FF 91 A8 09 00 00 41 89 C6'],
 ['3 LeEventMask (26.4.1)',    '85 C0 48 8D 05 A0 F2 8F 00 48 8D 15 A1 F2 8F 00 48 0F 44 D0'],
 ['4 AdvSetAddr (26.4.1)',     '83 B8 34 03 00 00 03 77 2A 31 FF E8 A2 7D CE FF'],
 ['5 ExtCreateConn (26.4.1)',  '0F B6 BD 5E FF FF FF E8 2F B0 CF FF'],
 ['6 AdvSetAddrResend (26.4.1)', '8B 54 3A 03 33 94 08 11 0D 00 00 44 09 C2 0F 84 80 00 00 00'],
);
my @a2dp = (
 ['A2DP gate 14+ (masked)',  '45 85 F6 74 25 48 8D 05 ?? ?? ?? ?? 48 8B 18', '45 31 F6 EB 25 48 8D 05 ?? ?? ?? ?? 48 8B 18'],
 ['A2DP gate 14+ proposed (+ mov esi,0x10)', '45 85 F6 74 25 48 8D 05 ?? ?? ?? ?? 48 8B 18 48 89 DF BE 10 00 00 00', '45 31 F6 EB 25 48 8D 05 ?? ?? ?? ?? 48 8B 18 48 89 DF BE 10 00 00 00'],
);

my $ok = 1;
print "== Sequoia table (each must be exactly 1 match, original or patched) ==\n";
for my $e (@seq) {
    print "$e->[0]\n";
    my $h = search('original', $e->[1]) + search('patched', $e->[2]);
    $ok = 0 unless $h == 1;
}
print "\n== Tahoe 26.4.1 table (informative; expected 0 on 15.x) ==\n";
for my $e (@tahoe) { print "$e->[0]\n"; search('original', $e->[1]); }
print "\n== a2dpcheck ==\n";
for my $e (@a2dp) { print "$e->[0]\n"; search('original', $e->[1]); search('patched', $e->[2]); }
# --- optional: check the C arrays in BlueToolFixup.cpp (Sequoia table) against this binary ---
my $uuidHex = '';
{ my ($pp) = (32); my ($nc) = (unpack('V6', $d))[4];
  for (1 .. $nc) { my ($c, $l) = unpack('VV', substr($d, $pp, 8)); $uuidHex = uc unpack('H*', substr($d, $pp + 8, 16)) if $c == 0x1b; $pp += $l; } }
if (defined $src) {
    open my $sf, '<', $src or die "open $src: $!"; my $code = do { local $/; <$sf> }; close $sf;
    my %arr;
    while ($code =~ /static const uint8_t (k\w+)\[\d*\]\s*=\s*\{(.*?)\};/gs) {
        my ($n, $body) = ($1, $2); $body =~ s{//[^\n]*}{}g;
        $arr{$n} = [ map { hex } ($body =~ /0x([0-9A-Fa-f]{2})/g) ];
    }
    print "\n== Source check: $src ==\n";
    my $u = join '', map { sprintf '%02X', $_ } @{ $arr{kBluetoothdUuid24H32} || [] };
    printf "  kBluetoothdUuid24H32 %s binary LC_UUID (%s vs %s)\n", ($u eq $uuidHex ? '==' : '!='), $u, $uuidHex;
    $ok = 0 unless $u eq $uuidHex;
    my ($tbl) = $code =~ /kAdvSetsPatches24\[kAdvSetsPatchCount\]\s*=\s*\{(.*?)\};/s;
    my $i = 0;
    while ($tbl =~ /ADVSETS_(EXACT|MASKED)\("([^"]+)",\s*(\w+),\s*(\w+)(?:,\s*(\w+),\s*(\w+))?\)/g) {
        my ($kind, $name, $f, $x, $r, $rm) = ($1, $2, $3, $4, $5, $6);
        my ($fm, $rep, $repm) = $kind eq 'EXACT' ? (undef, $x, undef) : ($x, $r, $rm);
        my @f = @{ $arr{$f} }; my @fm = $fm ? @{ $arr{$fm} } : (0xFF) x @f;
        my @r = @{ $arr{$rep} }; my @rm = $repm ? @{ $arr{$repm} } : (0xFF) x @f;
        die "size mismatch in $name\n" unless @f == @fm && @f == @r && @f == @rm;
        my $orig = join ' ', map { $fm[$_] == 0xFF ? sprintf('%02X', $f[$_]) : ($fm[$_] == 0 ? '??' : die "partial mask") } 0 .. $#f;
        my $patched = join ' ', map { $rm[$_] == 0xFF ? sprintf('%02X', $r[$_]) : ($fm[$_] == 0xFF ? sprintf('%02X', $f[$_]) : '??') } 0 .. $#f;
        $i++;
        print "$i $name ($kind, ".scalar(@f)." bytes)\n";
        my $h = search('original', $orig) + search('patched', $patched);
        $ok = 0 unless $h == 1;
        # simulate Lilu: d = (d & ~rm) | (r & rm) at the match, show before/after
        my $re = join '', map { $_ eq '??' ? '[\x00-\xff]' : sprintf('\x%s', $_) } split ' ', $orig;
        if ($d =~ /$re/s) { my $o = $-[0]; my @b = unpack('C*', substr($d, $o, scalar @f));
            my @a = map { ($b[$_] & ~$rm[$_] & 0xFF) | ($r[$_] & $rm[$_]) } 0 .. $#b;
            printf "     before: %s\n     after : %s\n", join(' ', map { sprintf '%02X', $_ } @b), join(' ', map { sprintf '%02X', $_ } @a); }
    }
    print "  table entries parsed: $i\n"; $ok = 0 unless $i == 6;
}
print "\nSequoia table: ", ($ok ? "ALL SIX UNIQUE - OK" : "FAILED (not exactly one match for every pattern)"), "\n";
exit($ok ? 0 : 1);

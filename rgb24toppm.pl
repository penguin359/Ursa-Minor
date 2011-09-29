#!/usr/bin/perl


use 5.008_001;
use strict;
use warnings;


my $in = *STDIN;
my $out = *STDOUT;

print $out "P3\n";
print $out "# Created by $0\n";
print $out "480 800\n";
print $out "255\n";

while(<$in>) {
	#next if $_ !~ /^0x/;
	last if $_ =~ /^}/;
	foreach(split /\s*,\s*/) {
		next if $_ !~ /0x([0-9a-fA-F][0-9a-fA-F])([0-9a-fA-F][0-9a-fA-F])([0-9a-fA-F][0-9a-fA-F])([0-9a-fA-F][0-9a-fA-F])/;
		my $alpha = hex $1;
		my $red = hex $2;
		my $green = hex $3;
		my $blue = hex $4;
		warn "Alpha channel in use" if $alpha != 0;
		#print "$_ ";
		#print $out "$2 $3 $4 ";
		printf $out "%3d %3d %3d   ", $red, $green, $blue;
		#print "\n";
	}
	print $out "\n";
	#print $out $_;
}
